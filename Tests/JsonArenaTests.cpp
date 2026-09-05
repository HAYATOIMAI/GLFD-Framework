/**
 * @file  JsonArenaTests.cpp
 * @brief JsonArena の単体テスト(GLFD JSON サブシステム フェーズ 1-1 / 要件 T-5)
 *
 * @details
 *  リポジトリにテストフレームワークが存在しないため、外部依存ゼロの
 *  自前ハーネスで実装している。ゲーム本体の vcxproj には含めず、
 *  Tests/build_and_run.bat から cl.exe 単体でビルド・実行する。
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <malloc.h>   // _aligned_malloc / _aligned_free (MSVC)

#include "TestHarness.h"
#include "MockMemoryResource.h"
#include "Core/Json/JsonArena.h"

using GLFD::Json::JsonArena;
using GLFD::Test::MockMemoryResource;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::Summarize;

namespace {

  /// ポインタが alignment 境界に載っているか
  bool IsAligned(const void* p, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(p) & (alignment - 1)) == 0;
  }

  /// [begin, begin+size) を全て書き、書いた内容が読み戻せることを確認する
  bool WriteAndVerify(void* begin, size_t size, unsigned char pattern) {
    unsigned char* const bytes = static_cast<unsigned char*>(begin);
    for (size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<unsigned char>(pattern + (i & 0x3F));
    }
    for (size_t i = 0; i < size; ++i) {
      if (bytes[i] != static_cast<unsigned char>(pattern + (i & 0x3F))) {
        return false;
      }
    }
    return true;
  }

}

// ---------------------------------------------------------------------------
// 1. 基本: アライメント遵守と領域の非重複
// ---------------------------------------------------------------------------

static void Test_BasicAllocation() {
  BeginCase("basic: alignment is honored and allocations do not overlap");

  MockMemoryResource mock;
  {
    JsonArena arena(&mock, 4096);

    CHECK(arena.BlockCount() == 0);       // 構築時点では確保しない(遅延確保)
    CHECK(arena.UsedBytes() == 0);
    CHECK(arena.AllocatedBytes() == 0);
    CHECK(mock.AllocateCalls() == 0);

    static constexpr size_t kAlignments[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    static constexpr size_t kCount        = sizeof(kAlignments) / sizeof(kAlignments[0]);

    void*  blocks[kCount] = {};
    size_t sizes[kCount]  = {};

    for (size_t i = 0; i < kCount; ++i) {
      const size_t size = 1 + i * 13;
      void* const  p    = arena.Allocate(size, kAlignments[i]);

      CHECK(p != nullptr);
      CHECK(IsAligned(p, kAlignments[i]));
      CHECK(WriteAndVerify(p, size, static_cast<unsigned char>(0x10 + i)));

      blocks[i] = p;
      sizes[i]  = size;
    }

    // 全ペアで領域が重ならないことを確認する
    for (size_t i = 0; i < kCount; ++i) {
      for (size_t j = i + 1; j < kCount; ++j) {
        const unsigned char* a = static_cast<unsigned char*>(blocks[i]);
        const unsigned char* b = static_cast<unsigned char*>(blocks[j]);
        const bool disjoint = (a + sizes[i] <= b) || (b + sizes[j] <= a);
        CHECK(disjoint);
      }
    }

    // 全て 1 ブロックに収まる大きさなので、resource への要求は 1 回だけ
    CHECK(arena.BlockCount() == 1);
    CHECK(mock.AllocateCalls() == 1);
    CHECK(arena.UsedBytes() >= 1 + 14 + 27);          // パディング込みなので下限のみ
    CHECK(arena.AllocatedBytes() >= 4096);            // ヘッダぶん上乗せされる
    CHECK(arena.AllocatedBytes() == mock.TotalBytes());

    // size == 0 でも有効なポインタを返す(失敗と区別できること)
    void* const zero = arena.Allocate(0, 8);
    CHECK(zero != nullptr);
    CHECK(IsAligned(zero, 8));
  }

  CHECK(mock.LiveBlockCount() == 0);
  CHECK(mock.LiveBytes() == 0);
}

// ---------------------------------------------------------------------------
// 2. ブロック伸長
// ---------------------------------------------------------------------------

static void Test_BlockChainGrowth() {
  BeginCase("growth: chain extends and BlockCount increases");

  MockMemoryResource mock;
  {
    constexpr size_t kBlockSize = 1024;
    constexpr size_t kChunk     = 128;

    JsonArena arena(&mock, kBlockSize);

    size_t previousBlockCount = 0;
    size_t growthObserved     = 0;

    for (size_t i = 0; i < 64; ++i) {
      void* const p = arena.Allocate(kChunk, 8);
      CHECK(p != nullptr);
      CHECK(WriteAndVerify(p, kChunk, 0x5A));

      if (arena.BlockCount() > previousBlockCount) {
        ++growthObserved;
        previousBlockCount = arena.BlockCount();
      }
    }

    // 64 * 128 = 8192 バイトを 1024 バイトのブロックへ流すので 8 ブロック必要
    CHECK(arena.BlockCount() == 8);
    CHECK(growthObserved == 8);
    CHECK(static_cast<size_t>(mock.AllocateCalls()) == arena.BlockCount());
    CHECK(arena.UsedBytes() == 64 * kChunk);

    // AllocatedBytes は resource へ要求した総量と厳密に一致する(ヘッダ込みの定義)
    CHECK(arena.AllocatedBytes() == mock.TotalBytes());
    CHECK(arena.AllocatedBytes() > 8 * kBlockSize);   // ヘッダぶんだけ必ず上回る

    // N-4: 確保回数はノード数ではなくブロック数に比例する
    CHECK(mock.AllocateCalls() == 8);
  }

  CHECK(mock.LiveBlockCount() == 0);
}

// ---------------------------------------------------------------------------
// 3. 巨大確保(blockSize の 3 倍)
// ---------------------------------------------------------------------------

static void Test_HugeAllocation() {
  BeginCase("huge: request larger than blockSize gets a dedicated block");

  MockMemoryResource mock;
  {
    constexpr size_t kBlockSize = 4096;
    constexpr size_t kHugeSize  = kBlockSize * 3;

    JsonArena arena(&mock, kBlockSize);

    // 先に通常ブロックを起こし、残り領域を持たせておく
    void* const small = arena.Allocate(64, 8);
    CHECK(small != nullptr);
    CHECK(arena.BlockCount() == 1);
    const size_t usedBeforeHuge = arena.UsedBytes();

    void* const huge = arena.Allocate(kHugeSize, 8);
    CHECK(huge != nullptr);
    CHECK(IsAligned(huge, 8));
    CHECK(arena.BlockCount() == 2);

    // 領域全体が書き込み可能であること
    CHECK(WriteAndVerify(huge, kHugeSize, 0x3C));
    CHECK(arena.UsedBytes() == usedBeforeHuge + kHugeSize);

    // 論点 2: 専用ブロックを取っても現ブロックの残りは捨てられない。
    // 直後の通常確保が新しいブロックを起こさないことで確認する
    const size_t blocksAfterHuge = arena.BlockCount();
    void* const  afterHuge       = arena.Allocate(64, 8);
    CHECK(afterHuge != nullptr);
    CHECK(arena.BlockCount() == blocksAfterHuge);   // 増えていない = 残りを再利用した

    // 1 番目のブロック(先頭 4096 バイト)の続きから取れているはず
    const unsigned char* const a = static_cast<const unsigned char*>(small);
    const unsigned char* const b = static_cast<const unsigned char*>(afterHuge);
    const ptrdiff_t            d = b - a;
    CHECK(d > 0 && d < static_cast<ptrdiff_t>(kBlockSize));

    // 過剰アライメント(SIMD 用)も巨大確保で満たせること
    void* const wide = arena.Allocate(kHugeSize, 64);
    CHECK(wide != nullptr);
    CHECK(IsAligned(wide, 64));
    CHECK(WriteAndVerify(wide, kHugeSize, 0x71));
  }

  CHECK(mock.LiveBlockCount() == 0);
  CHECK(mock.LiveBytes() == 0);
  CHECK(!mock.SizeMismatch());
  CHECK(!mock.AlignmentMismatch());
}

// ---------------------------------------------------------------------------
// 4. 失敗注入 (R0-3 / T-5) — 最重要
// ---------------------------------------------------------------------------

static void Test_AllocationFailureInjection() {
  BeginCase("failure injection: Allocate returns nullptr without aborting");

  // 4-a: 1 回目の確保から失敗させる
  {
    MockMemoryResource mock;
    mock.SetFailAfter(0);

    JsonArena arena(&mock, 1024);

    void* const p = arena.Allocate(16, 8);
    CHECK(p == nullptr);
    CHECK(arena.BlockCount() == 0);
    CHECK(arena.AllocatedBytes() == 0);
    CHECK(arena.UsedBytes() == 0);
    CHECK(mock.InjectedFailures() == 1);

    // 失敗後も Reset とデストラクタが安全に動作すること
    arena.Reset();
    CHECK(arena.BlockCount() == 0);

    // 失敗を解除すれば復帰できること
    mock.ClearFailure();
    void* const q = arena.Allocate(16, 8);
    CHECK(q != nullptr);
    CHECK(arena.BlockCount() == 1);
  }

  // 4-b: 途中(3 ブロック目)から失敗させる
  {
    MockMemoryResource mock;
    JsonArena arena(&mock, 1024);

    CHECK(arena.Allocate(1000, 8) != nullptr);   // ブロック 1
    CHECK(arena.Allocate(1000, 8) != nullptr);   // ブロック 2
    CHECK(arena.BlockCount() == 2);

    mock.SetFailAfter(2);

    // 現ブロックの残りに収まる要求はまだ成功する
    void* const stillFits = arena.Allocate(8, 8);
    CHECK(stillFits != nullptr);

    // 新しいブロックを要する要求は失敗する
    for (int i = 0; i < 5; ++i) {
      CHECK(arena.Allocate(1000, 8) == nullptr);
    }
    CHECK(arena.BlockCount() == 2);              // 鎖は壊れていない
    CHECK(mock.InjectedFailures() == 5);

    // 巨大確保の失敗も同様に nullptr を返すだけであること
    CHECK(arena.Allocate(1024 * 1024, 8) == nullptr);

    // 失敗が混ざったあとでも Reset は安全
    arena.Reset();
    CHECK(arena.BlockCount() == 2);
    CHECK(arena.UsedBytes() == 0);

    mock.ClearFailure();
    CHECK(arena.Allocate(1000, 8) != nullptr);

    // Release 後も再利用できること
    arena.Release();
    CHECK(arena.BlockCount() == 0);
    CHECK(arena.AllocatedBytes() == 0);
    CHECK(mock.LiveBlockCount() == 0);
    CHECK(arena.Allocate(16, 8) != nullptr);
  }

  // 4-c: resource が nullptr でも落ちないこと
  {
    JsonArena arena(nullptr, 1024);
    CHECK(arena.Allocate(16, 8) == nullptr);
    CHECK(arena.AllocateArray<int>(4) == nullptr);
    arena.Reset();
    arena.Release();
    CHECK(arena.BlockCount() == 0);
  }
}

// ---------------------------------------------------------------------------
// 5. Reset による再利用 (R0-4)
// ---------------------------------------------------------------------------

static void Test_ResetReuse() {
  BeginCase("reset: reuses blocks without asking the resource again");

  MockMemoryResource mock;
  {
    constexpr size_t kBlockSize = 1024;
    JsonArena arena(&mock, kBlockSize);

    // 大量に確保してブロックを伸ばす
    for (int i = 0; i < 40; ++i) {
      CHECK(arena.Allocate(200, 8) != nullptr);
    }
    const size_t blocksAfterFirstPass = arena.BlockCount();
    const int    callsAfterFirstPass  = mock.AllocateCalls();
    const size_t bytesAfterFirstPass  = arena.AllocatedBytes();
    CHECK(blocksAfterFirstPass > 1);
    CHECK(static_cast<size_t>(callsAfterFirstPass) == blocksAfterFirstPass);

    arena.Reset();

    CHECK(arena.UsedBytes() == 0);
    CHECK(arena.BlockCount() == blocksAfterFirstPass);      // ブロックは保持される
    CHECK(arena.AllocatedBytes() == bytesAfterFirstPass);
    CHECK(mock.AllocateCalls() == callsAfterFirstPass);     // Reset 自体は何も要求しない
    CHECK(mock.DeallocateCalls() == 0);                     // 何も返却しない
    CHECK(mock.LiveBlockCount() == blocksAfterFirstPass);

    // 同じ確保列を再実行しても resource への追加要求は発生しない
    for (int i = 0; i < 40; ++i) {
      void* const p = arena.Allocate(200, 8);
      CHECK(p != nullptr);
      CHECK(WriteAndVerify(p, 200, 0x2E));
    }
    CHECK(mock.AllocateCalls() == callsAfterFirstPass);
    CHECK(arena.BlockCount() == blocksAfterFirstPass);

    // 複数回 Reset しても同じ
    for (int pass = 0; pass < 8; ++pass) {
      arena.Reset();
      for (int i = 0; i < 40; ++i) {
        CHECK(arena.Allocate(200, 8) != nullptr);
      }
    }
    CHECK(mock.AllocateCalls() == callsAfterFirstPass);
  }

  CHECK(mock.LiveBlockCount() == 0);
}

// ---------------------------------------------------------------------------
// 6. リーク検証: 確保と解放の回数・サイズが完全に一致すること
// ---------------------------------------------------------------------------

static void Test_NoLeaks() {
  BeginCase("leak: allocate/deallocate counts and sizes match exactly");

  MockMemoryResource mock;
  int expectedBlocks = 0;

  {
    JsonArena arena(&mock, 2048);

    for (int i = 0; i < 30; ++i) {
      CHECK(arena.Allocate(300, 8) != nullptr);
    }
    CHECK(arena.Allocate(2048 * 3, 16) != nullptr);   // 専用ブロック
    CHECK(arena.Allocate(64, 64) != nullptr);         // 過剰アライメント
    arena.Reset();
    for (int i = 0; i < 10; ++i) {
      CHECK(arena.Allocate(500, 8) != nullptr);
    }

    expectedBlocks = static_cast<int>(arena.BlockCount());
    CHECK(mock.AllocateCalls() == expectedBlocks);
    CHECK(mock.LiveBytes() == arena.AllocatedBytes());
  }
  // ここでデストラクタが走る

  CHECK(mock.DeallocateCalls() == expectedBlocks);
  CHECK(mock.AllocateCalls() == mock.DeallocateCalls());
  CHECK(mock.LiveBlockCount() == 0);
  CHECK(mock.LiveBytes() == 0);

  // Deallocate へ渡したサイズ・アライメントが確保時と厳密に一致していること
  CHECK(!mock.SizeMismatch());
  CHECK(!mock.AlignmentMismatch());
  CHECK(!mock.DoubleFree());
  CHECK(!mock.UnknownFree());
  CHECK(!mock.RecordOverflow());

  // 明示的な Release でも同じ結果になること
  {
    MockMemoryResource mock2;
    JsonArena arena(&mock2, 1024);
    for (int i = 0; i < 20; ++i) {
      CHECK(arena.Allocate(200, 8) != nullptr);
    }
    const int blocks = static_cast<int>(arena.BlockCount());

    arena.Release();

    CHECK(mock2.DeallocateCalls() == blocks);
    CHECK(mock2.LiveBlockCount() == 0);
    CHECK(mock2.LiveBytes() == 0);
    CHECK(arena.BlockCount() == 0);
    CHECK(arena.AllocatedBytes() == 0);
    CHECK(arena.UsedBytes() == 0);

    // Release の二重呼び出しが安全であること
    arena.Release();
    CHECK(mock2.DeallocateCalls() == blocks);
    CHECK(!mock2.DoubleFree());
  }
}

// ---------------------------------------------------------------------------
// 7. オーバーフロー安全性
// ---------------------------------------------------------------------------

namespace {
  struct Big64 { unsigned char bytes[64]; };
}

static void Test_OverflowSafety() {
  BeginCase("overflow: wrap-around requests fail instead of returning a small buffer");

  MockMemoryResource mock;
  {
    JsonArena arena(&mock, 1024);

    // 乗算オーバーフロー: count * sizeof(T) がラップアラウンドする入力
    CHECK(arena.AllocateArray<Big64>(SIZE_MAX) == nullptr);
    CHECK(arena.AllocateArray<Big64>(SIZE_MAX / 2) == nullptr);
    CHECK(arena.AllocateArray<int>(SIZE_MAX) == nullptr);
    CHECK(arena.AllocateArray<int>(SIZE_MAX / 4 + 1) == nullptr);
    CHECK(arena.AllocateArray<std::uint64_t>(SIZE_MAX / 8 + 1) == nullptr);

    // 加算オーバーフロー: size + (alignment - 1) がラップアラウンドする入力
    CHECK(arena.Allocate(SIZE_MAX, 64) == nullptr);
    CHECK(arena.Allocate(SIZE_MAX - 8, 64) == nullptr);

    // ラップアラウンドしないが物理的に確保不能な巨大要求も nullptr であること
    CHECK(arena.Allocate(SIZE_MAX / 2, 8) == nullptr);

    // オーバーフロー要求で会計が汚れていないこと
    CHECK(arena.BlockCount() == 0);
    CHECK(arena.UsedBytes() == 0);
    CHECK(arena.AllocatedBytes() == 0);

    // 妥当な AllocateArray は正しく動くこと
    Big64* const array = arena.AllocateArray<Big64>(4);
    CHECK(array != nullptr);
    CHECK(IsAligned(array, alignof(Big64)));
    CHECK(WriteAndVerify(array, sizeof(Big64) * 4, 0x11));

    // 不正なアライメントは nullptr(デバッグビルドでは assert も発火するため
    // NDEBUG ビルドでのみ検証する)
#ifdef NDEBUG
    CHECK(arena.Allocate(16, 0) == nullptr);
    CHECK(arena.Allocate(16, 3) == nullptr);
    CHECK(arena.Allocate(16, 24) == nullptr);
#endif
  }

  CHECK(mock.LiveBlockCount() == 0);
}

// ---------------------------------------------------------------------------
// 8. ムーブ
// ---------------------------------------------------------------------------

static void Test_MoveSemantics() {
  BeginCase("move: moved-from arena does not double-free");

  // 8-a: ムーブ構築
  {
    MockMemoryResource mock;
    int blocks = 0;
    {
      JsonArena source(&mock, 1024);
      for (int i = 0; i < 10; ++i) {
        CHECK(source.Allocate(200, 8) != nullptr);
      }
      blocks = static_cast<int>(source.BlockCount());
      const size_t used      = source.UsedBytes();
      const size_t allocated = source.AllocatedBytes();

      JsonArena moved(static_cast<JsonArena&&>(source));

      CHECK(moved.BlockCount() == static_cast<size_t>(blocks));
      CHECK(moved.UsedBytes() == used);
      CHECK(moved.AllocatedBytes() == allocated);

      // ムーブ元は「有効だが空」
      CHECK(source.BlockCount() == 0);
      CHECK(source.UsedBytes() == 0);
      CHECK(source.AllocatedBytes() == 0);
      CHECK(source.Resource() == &mock);

      // ムーブ元も引き続き使える(新規ブロックを取りに行く)
      CHECK(source.Allocate(16, 8) != nullptr);
      CHECK(source.BlockCount() == 1);
      // ここで source と moved の両方のデストラクタが走る
    }

    CHECK(mock.AllocateCalls() == mock.DeallocateCalls());
    CHECK(mock.LiveBlockCount() == 0);
    CHECK(mock.LiveBytes() == 0);
    CHECK(!mock.DoubleFree());
    CHECK(!mock.UnknownFree());
  }

  // 8-b: ムーブ代入。代入先が抱えていたブロックが正しく返却されること
  {
    MockMemoryResource mock;
    {
      JsonArena source(&mock, 1024);
      JsonArena target(&mock, 1024);

      for (int i = 0; i < 6; ++i) { CHECK(source.Allocate(200, 8) != nullptr); }
      for (int i = 0; i < 6; ++i) { CHECK(target.Allocate(200, 8) != nullptr); }

      const size_t targetBlocks = target.BlockCount();
      const size_t sourceBlocks = source.BlockCount();
      const int    freedBefore  = mock.DeallocateCalls();

      target = static_cast<JsonArena&&>(source);

      // 代入先の元ブロックが解放されている(リークしていない)
      CHECK(mock.DeallocateCalls() == freedBefore + static_cast<int>(targetBlocks));
      CHECK(target.BlockCount() == sourceBlocks);
      CHECK(source.BlockCount() == 0);
    }

    CHECK(mock.AllocateCalls() == mock.DeallocateCalls());
    CHECK(mock.LiveBlockCount() == 0);
    CHECK(!mock.DoubleFree());
  }

  // 8-c: 自己ムーブ代入で自壊しないこと
  {
    MockMemoryResource mock;
    {
      JsonArena arena(&mock, 1024);
      CHECK(arena.Allocate(200, 8) != nullptr);
      const size_t blocks = arena.BlockCount();

      JsonArena& alias = arena;
      arena = static_cast<JsonArena&&>(alias);

      CHECK(arena.BlockCount() == blocks);
      CHECK(arena.Allocate(200, 8) != nullptr);
    }
    CHECK(mock.AllocateCalls() == mock.DeallocateCalls());
    CHECK(mock.LiveBlockCount() == 0);
    CHECK(!mock.DoubleFree());
  }
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("JsonArena");

  Test_BasicAllocation();
  Test_BlockChainGrowth();
  Test_HugeAllocation();
  Test_AllocationFailureInjection();
  Test_ResetReuse();
  Test_NoLeaks();
  Test_OverflowSafety();
  Test_MoveSemantics();

  return Summarize();
}
