/**
 * @file  DynamicArrayTests.cpp
 * @brief フェーズ 1-5a: DynamicArray の非投擲 API (Try*) のテスト
 *
 * @details
 *  R3-13 で追加した `TryReserve` / `TryPushBack` / `TryResize` を検証する。
 *  受け入れ条件は以下:
 *    - 確保失敗を `false` で返すこと (MockMemoryResource で注入)
 *    - **失敗時にコンテナの状態が不変**であること (強い例外安全性に相当)
 *    - 投擲版が非投擲版を呼ぶ形になっており、確保失敗が
 *      ヌル参照クラッシュではなく `std::bad_alloc` になること
 *
 *  このスイートは 1-5a で新規に追加したものであり、既存 8 スイートの
 *  チェック数には影響しない (T-27)。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 *        日本語はコメントのみに使う。
 */

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "Core/DynamicArray.h"
#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::DynamicArray;
using GLFD::Test::MockMemoryResource;

namespace {

  // ---------------------------------------------------------------------------
  // テスト用の要素型
  // ---------------------------------------------------------------------------

  /// 生存数を数える要素。nothrow ムーブ構築可能かつ非トリビアル破棄
  struct Counted {
    static int s_live;

    int v = 0;

    Counted() noexcept { ++s_live; }
    explicit Counted(int x) noexcept : v(x) { ++s_live; }
    Counted(const Counted& o) noexcept : v(o.v) { ++s_live; }
    Counted(Counted&& o) noexcept : v(o.v) { ++s_live; }
    Counted& operator=(const Counted&) noexcept = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --s_live; }
  };

  int Counted::s_live = 0;

  static_assert(!std::is_trivially_destructible_v<Counted>,
                "Counted must exercise the non-trivial destruction branches");

  /// ムーブもコピーも throw し得る型。noexcept 指定の条件分岐を検証するためだけに使う
  struct ThrowingCopy {
    int v = 0;

    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy& o) : v(o.v) {}   // noexcept(false)
    ThrowingCopy(ThrowingCopy&& o) : v(o.v) {}        // noexcept(false)
    ThrowingCopy& operator=(const ThrowingCopy&) = default;
    ThrowingCopy& operator=(ThrowingCopy&&) = default;
    ~ThrowingCopy() = default;
  };

  /// 要素型由来の例外を任意のタイミングで起こせる型 (kNothrowRelocate == false)
  struct BombError {};

  struct Bomb {
    static int s_live;
    static int s_copyBudget;      ///< 0 以上なら、この回数だけ複製に成功した後に投げる
    static int s_defaultBudget;   ///< 0 以上なら、この回数だけ既定構築に成功した後に投げる

    int v = 0;

    Bomb() {
      if (s_defaultBudget >= 0) {
        if (s_defaultBudget == 0) { throw BombError{}; }
        --s_defaultBudget;
      }
      ++s_live;
    }
    explicit Bomb(int x) : v(x) { ++s_live; }   // 準備用。予算を消費しない

    // ムーブもコピーも noexcept でないため kNothrowRelocate == false になり、
    // 再配置は uninitialized_copy_n (コピー) 経路を通る
    Bomb(const Bomb& o) : v(o.v) { Duplicate(); }
    Bomb(Bomb&& o) : v(o.v) { Duplicate(); }

    Bomb& operator=(const Bomb&) = default;
    Bomb& operator=(Bomb&&) = default;
    ~Bomb() { --s_live; }

    static void Disarm() { s_copyBudget = -1; s_defaultBudget = -1; }

  private:
    void Duplicate() {
      if (s_copyBudget >= 0) {
        if (s_copyBudget == 0) { throw BombError{}; }
        --s_copyBudget;
      }
      ++s_live;
    }
  };

  int Bomb::s_live          = 0;
  int Bomb::s_copyBudget    = -1;
  int Bomb::s_defaultBudget = -1;

  // ---------------------------------------------------------------------------
  // 静的検証
  // ---------------------------------------------------------------------------

  // 要素の再配置が throw し得ない型では Try* は本当に noexcept でなければならない。
  // アーカイブ層 (/EH 無し) が呼ぶ前提なので、これはコメントではなく静的に固定する
  static_assert(DynamicArray<int>::kNothrowRelocate,
                "int must be nothrow-relocatable");
  static_assert(DynamicArray<Counted>::kNothrowRelocate,
                "Counted must be nothrow-relocatable");
  static_assert(!DynamicArray<ThrowingCopy>::kNothrowRelocate,
                "ThrowingCopy must not be nothrow-relocatable");

  static_assert(noexcept(std::declval<DynamicArray<int>&>().TryReserve(size_t{ 1 })),
                "TryReserve<int> must be noexcept");
  static_assert(noexcept(std::declval<DynamicArray<int>&>().TryPushBack(std::declval<const int&>())),
                "TryPushBack<int>(const int&) must be noexcept");
  static_assert(noexcept(std::declval<DynamicArray<int>&>().TryPushBack(int{ 1 })),
                "TryPushBack<int>(int&&) must be noexcept");
  static_assert(noexcept(std::declval<DynamicArray<int>&>().TryResize(size_t{ 1 })),
                "TryResize<int> must be noexcept");
  static_assert(noexcept(std::declval<DynamicArray<Counted>&>().TryPushBack(Counted{})),
                "TryPushBack<Counted> must be noexcept");

  // 逆に、要素側が throw し得る型では noexcept を名乗ってはならない
  // (名乗ると terminate になり、失敗を戻り値で扱えなくなる)
  static_assert(!noexcept(std::declval<DynamicArray<ThrowingCopy>&>()
                            .TryPushBack(std::declval<const ThrowingCopy&>())),
                "TryPushBack<ThrowingCopy> must not claim noexcept");

  // 戻り値の無視をコンパイラに検出させる (R3-13 の要点)
  static_assert(std::is_same_v<decltype(std::declval<DynamicArray<int>&>().TryPushBack(1)), bool>,
                "Try* must return bool");

  // ---------------------------------------------------------------------------
  // 1. 成功経路
  // ---------------------------------------------------------------------------

  void TestSuccessPaths() {
    GLFD::Test::BeginCase("Try* succeed and behave like the throwing versions");

    MockMemoryResource mock;
    {
      DynamicArray<int> a(&mock);

      CHECK(a.TryReserve(16));
      CHECK(a.GetCapacity() == 16);
      CHECK(a.GetSize() == 0);

      // 既に足りている場合は確保しない
      const int before = mock.AllocateCalls();
      CHECK(a.TryReserve(8));
      CHECK(mock.AllocateCalls() == before);
      CHECK(a.GetCapacity() == 16);

      for (int i = 0; i < 16; ++i) {
        CHECK_QUIET(a.TryPushBack(i));
      }
      ++GLFD::Test::g_checkCount;
      CHECK(a.GetSize() == 16);

      bool contentsOk = true;
      for (size_t i = 0; i < a.GetSize(); ++i) {
        if (a[i] != static_cast<int>(i)) { contentsOk = false; }
      }
      CHECK(contentsOk);

      // 容量を超える追加で伸長する
      CHECK(a.TryPushBack(99));
      CHECK(a.GetSize() == 17);
      CHECK(a.GetCapacity() > 16);
      CHECK(a[16] == 99);

      // 右辺値版
      int moved = 123;
      CHECK(a.TryPushBack(std::move(moved)));
      CHECK(a.Back() == 123);

      // 拡大 / 縮小
      CHECK(a.TryResize(64));
      CHECK(a.GetSize() == 64);
      CHECK(a[63] == 0);              // 値初期化されている
      CHECK(a[0] == 0);
      CHECK(a.TryResize(4));
      CHECK(a.GetSize() == 4);
      CHECK(a[3] == 3);               // 残った要素は不変

      // 値指定の拡大
      CHECK(a.TryResize(8, 7));
      CHECK(a.GetSize() == 8);
      CHECK(a[7] == 7);
      CHECK(a[4] == 7);
      CHECK(a[3] == 3);
    }

    CHECK(mock.LiveBlockCount() == 0);
    CHECK(!mock.SizeMismatch());
    CHECK(!mock.AlignmentMismatch());
    CHECK(!mock.DoubleFree());
    CHECK(!mock.UnknownFree());
  }

  // ---------------------------------------------------------------------------
  // 2. 確保失敗時に false を返し、状態が不変であること
  // ---------------------------------------------------------------------------

  void TestFailureLeavesStateUnchanged() {
    GLFD::Test::BeginCase("allocation failure returns false and leaves the container untouched");

    // (a) 空のコンテナ: 一度も確保できない
    {
      MockMemoryResource mock;
      mock.SetFailAfter(0);

      DynamicArray<int> a(&mock);
      CHECK(!a.TryPushBack(1));
      CHECK(a.GetSize() == 0);
      CHECK(a.GetCapacity() == 0);
      CHECK(a.GetData() == nullptr);

      CHECK(!a.TryReserve(32));
      CHECK(a.GetCapacity() == 0);

      CHECK(!a.TryResize(4));
      CHECK(a.GetSize() == 0);

      // 縮小は確保を伴わないので、失敗注入中でも成功する
      CHECK(a.TryResize(0));
      CHECK(a.GetSize() == 0);

      CHECK(mock.LiveBlockCount() == 0);
    }

    // (b) 中身のあるコンテナ: 伸長に失敗しても内容が保たれる
    {
      MockMemoryResource mock;
      DynamicArray<int> a(&mock);
      for (int i = 0; i < 8; ++i) {
        CHECK_QUIET(a.TryPushBack(i * 10));
      }
      ++GLFD::Test::g_checkCount;

      const size_t sizeBefore     = a.GetSize();
      const size_t capacityBefore = a.GetCapacity();
      const int*   dataBefore     = a.GetData();
      CHECK(sizeBefore == 8);
      CHECK(capacityBefore == 8);

      // 次の TryPushBack は必ず伸長を要求する
      mock.SetFailAfter(mock.AllocateCalls());
      CHECK(!a.TryPushBack(999));

      CHECK(a.GetSize() == sizeBefore);
      CHECK(a.GetCapacity() == capacityBefore);
      CHECK(a.GetData() == dataBefore);       // 領域の付け替えも起きていない

      bool contentsOk = true;
      for (size_t i = 0; i < a.GetSize(); ++i) {
        if (a[i] != static_cast<int>(i) * 10) { contentsOk = false; }
      }
      CHECK(contentsOk);

      CHECK(!a.TryReserve(1024));
      CHECK(a.GetCapacity() == capacityBefore);
      CHECK(a.GetData() == dataBefore);

      CHECK(!a.TryResize(1024));
      CHECK(a.GetSize() == sizeBefore);
      CHECK(a.GetData() == dataBefore);

      // 失敗注入を解除すれば通常どおり続行できる (壊れていない)
      mock.ClearFailure();
      CHECK(a.TryPushBack(999));
      CHECK(a.GetSize() == 9);
      CHECK(a.Back() == 999);
    }
  }

  // ---------------------------------------------------------------------------
  // 3. 非トリビアルな要素型でのリーク / 二重破棄の検出
  // ---------------------------------------------------------------------------

  void TestNonTrivialElements() {
    GLFD::Test::BeginCase("non-trivial elements are destroyed exactly once");

    CHECK(Counted::s_live == 0);

    MockMemoryResource mock;
    {
      DynamicArray<Counted> a(&mock);

      // 何度も再確保させ、移動のたびに旧要素が破棄されることを確かめる
      for (int i = 0; i < 40; ++i) {
        CHECK_QUIET(a.TryPushBack(Counted{ i }));
      }
      ++GLFD::Test::g_checkCount;
      CHECK(a.GetSize() == 40);
      CHECK(Counted::s_live == 40);

      // 縮小で破棄される
      CHECK(a.TryResize(10));
      CHECK(Counted::s_live == 10);

      // 拡大で構築される
      CHECK(a.TryResize(25));
      CHECK(Counted::s_live == 25);

      // 伸長失敗時は生存数も変わらない
      mock.SetFailAfter(mock.AllocateCalls());
      CHECK(!a.TryReserve(4096));
      CHECK(Counted::s_live == 25);
      mock.ClearFailure();

      CHECK(a.GetSize() == 25);
      CHECK(a[0].v == 0);
      CHECK(a[9].v == 9);
      CHECK(a[10].v == 0);   // TryResize で値初期化された分
    }

    CHECK(Counted::s_live == 0);
    CHECK(mock.LiveBlockCount() == 0);
    CHECK(!mock.DoubleFree());
    CHECK(!mock.SizeMismatch());
  }

  // ---------------------------------------------------------------------------
  // 4. N 回目の確保を失敗させる総当たり
  // ---------------------------------------------------------------------------

  /// 同じ操作列を実行する。injectAt < 0 なら失敗注入なし
  void RunSequence(MockMemoryResource& mock, int injectAt, bool& outAnyFailure) {
    outAnyFailure = false;
    if (injectAt >= 0) {
      mock.SetFailAfter(injectAt);
    }

    DynamicArray<Counted> a(&mock);

    for (int i = 0; i < 30; ++i) {
      if (!a.TryPushBack(Counted{ i })) {
        outAnyFailure = true;
        break;
      }
    }
    if (!outAnyFailure && !a.TryReserve(256)) { outAnyFailure = true; }
    if (!outAnyFailure && !a.TryResize(300))  { outAnyFailure = true; }

    // 失敗の有無にかかわらず、コンテナは一貫した状態で破棄できなければならない
    CHECK_QUIET(a.GetSize() <= a.GetCapacity());
    CHECK_QUIET((a.GetData() != nullptr) || (a.GetCapacity() == 0));
  }

  void TestExhaustiveFailureInjection() {
    GLFD::Test::BeginCase("exhaustive N-th allocation failure injection");

    // まず失敗なしで必要な確保回数を数える
    int totalAllocations = 0;
    {
      MockMemoryResource mock;
      bool failed = false;
      RunSequence(mock, -1, failed);
      CHECK(!failed);
      CHECK(Counted::s_live == 0);
      CHECK(mock.LiveBlockCount() == 0);
      totalAllocations = mock.AllocateCalls();
      CHECK(totalAllocations > 0);
    }

    // 0 回目から必要回数を超えるまで総当たりする
    bool allClean       = true;
    bool failedWhenLate = false;
    for (int n = 0; n <= totalAllocations + 2; ++n) {
      MockMemoryResource mock;
      bool failed = false;
      RunSequence(mock, n, failed);

      // 全ての N で: リークしない・二重解放しない・要素が残らない
      CHECK_QUIET(Counted::s_live == 0);
      CHECK_QUIET(mock.LiveBlockCount() == 0);
      CHECK_QUIET(!mock.DoubleFree());
      CHECK_QUIET(!mock.UnknownFree());
      CHECK_QUIET(!mock.SizeMismatch());
      CHECK_QUIET(!mock.AlignmentMismatch());

      if (Counted::s_live != 0 || mock.LiveBlockCount() != 0 ||
          mock.DoubleFree() || mock.UnknownFree() ||
          mock.SizeMismatch() || mock.AlignmentMismatch()) {
        allClean = false;
      }

      // 必要回数より前で切れば必ずどこかで false が返る
      if (n < totalAllocations && !failed) { failedWhenLate = true; }

      // 必要回数を満たしていれば最後まで通る
      if (n >= totalAllocations && failed) { failedWhenLate = true; }
    }

    CHECK(allClean);
    CHECK(!failedWhenLate);
    CHECK(Counted::s_live == 0);
  }

  // ---------------------------------------------------------------------------
  // 5. 投擲版の挙動 (未定義動作 -> 定義された停止)
  // ---------------------------------------------------------------------------

  void TestThrowingWrappers() {
    GLFD::Test::BeginCase("throwing API now raises bad_alloc instead of dereferencing null");

    // 従来は Reallocate が m_resource->Allocate の戻り値を検査しておらず、
    // 確保失敗時は nullptr のまま未初期化領域へ書き込んでいた (未定義動作)。
    // 現在は TryReallocate へ委譲しているため bad_alloc になる。
    //
    // 注: /EH 無しのゲーム本体ビルドでは throw は terminate であり、
    //     「クラッシュしなくなる」わけではない。得られるのは
    //     「未定義動作が、その場で止まる定義された停止に変わる」ことである。
    //     テストは /EHsc でビルドされるため、ここでは捕捉して検証できる。

    // (a) 空のコンテナへの PushBack
    {
      MockMemoryResource mock;
      mock.SetFailAfter(0);
      DynamicArray<int> a(&mock);

      bool threw = false;
      try {
        a.PushBack(1);
      }
      catch (const std::bad_alloc&) {
        threw = true;
      }
      CHECK(threw);
      CHECK(a.GetSize() == 0);
      CHECK(a.GetData() == nullptr);
    }

    // (b) 中身のあるコンテナの Reserve
    {
      MockMemoryResource mock;
      DynamicArray<int> a(&mock);
      a.PushBack(1);
      a.PushBack(2);

      const size_t capacityBefore = a.GetCapacity();
      const int*   dataBefore     = a.GetData();

      mock.SetFailAfter(mock.AllocateCalls());
      bool threw = false;
      try {
        a.Reserve(4096);
      }
      catch (const std::bad_alloc&) {
        threw = true;
      }
      CHECK(threw);
      CHECK(a.GetCapacity() == capacityBefore);   // 状態は不変
      CHECK(a.GetData() == dataBefore);
      CHECK(a.GetSize() == 2);
      CHECK(a[0] == 1);
      CHECK(a[1] == 2);
    }

    // (c) Resize
    {
      MockMemoryResource mock;
      DynamicArray<int> a(&mock);
      a.PushBack(5);

      mock.SetFailAfter(mock.AllocateCalls());
      bool threw = false;
      try {
        a.Resize(1000);
      }
      catch (const std::bad_alloc&) {
        threw = true;
      }
      CHECK(threw);
      CHECK(a.GetSize() == 1);
      CHECK(a[0] == 5);
    }
  }

  // ---------------------------------------------------------------------------
  // 6. 桁溢れの防御
  // ---------------------------------------------------------------------------

  void TestOverflowGuard() {
    GLFD::Test::BeginCase("capacity overflow is rejected without calling the resource");

    MockMemoryResource mock;
    DynamicArray<std::int64_t> a(&mock);

    // newCapacity * sizeof(T) が size_t を溢れる要求。
    // 確保を試みる前に false を返さなければならない
    const size_t tooLarge = (static_cast<size_t>(-1) / sizeof(std::int64_t)) + 1u;

    const int before = mock.AllocateCalls();
    CHECK(!a.TryReserve(tooLarge));
    CHECK(mock.AllocateCalls() == before);   // リソースには一度も届いていない
    CHECK(a.GetCapacity() == 0);
    CHECK(a.GetSize() == 0);

    // 上限ちょうどは (現実には確保に失敗するが) 上限判定では弾かれない
    const size_t maxCapacity = static_cast<size_t>(-1) / sizeof(std::int64_t);
    CHECK(!a.TryReserve(maxCapacity));       // 確保失敗として false
    CHECK(mock.AllocateCalls() > before);    // 今度はリソースまで届いている
    CHECK(a.GetCapacity() == 0);
  }

  // ---------------------------------------------------------------------------
  // 7. 既存 API の非回帰
  // ---------------------------------------------------------------------------

  void TestExistingApiUnchanged() {
    GLFD::Test::BeginCase("existing (throwing) API keeps its behaviour");

    MockMemoryResource mock;
    {
      DynamicArray<int> a(&mock);

      a.PushBack(1);
      a.PushBack(2);
      a.PushBack(3);
      CHECK(a.GetSize() == 3);
      CHECK(a.Front() == 1);
      CHECK(a.Back() == 3);
      CHECK(a.At(1) == 2);

      a.EmplaceBack(4);
      CHECK(a.Back() == 4);

      a.Erase(0);                       // 順序保持
      CHECK(a.GetSize() == 3);
      CHECK(a[0] == 2);
      CHECK(a[1] == 3);
      CHECK(a[2] == 4);

      a.EraseSwap(0);                   // 順序非保持
      CHECK(a.GetSize() == 2);
      CHECK(a[0] == 4);

      a.PopBack();
      CHECK(a.GetSize() == 1);

      a.Reserve(100);
      CHECK(a.GetCapacity() >= 100);
      a.ShrinkToFit();
      CHECK(a.GetCapacity() == 1);

      // At の範囲外は依然として out_of_range (論理エラーなので Try* 版は作らない)
      bool threw = false;
      try {
        (void)a.At(99);
      }
      catch (const std::out_of_range&) {
        threw = true;
      }
      CHECK(threw);

      // コピー / ムーブ
      DynamicArray<int> b(a);
      CHECK(b.GetSize() == 1);
      CHECK(b[0] == 4);

      DynamicArray<int> c(std::move(b));
      CHECK(c.GetSize() == 1);
      CHECK(b.GetSize() == 0);

      a.Clear();
      CHECK(a.IsEmpty());
    }

    CHECK(mock.LiveBlockCount() == 0);
    CHECK(!mock.DoubleFree());
    CHECK(!mock.SizeMismatch());
    CHECK(!mock.AlignmentMismatch());
  }

  // ---------------------------------------------------------------------------
  // 8. 要素型由来の例外が伝播したときのコンテナの健全性
  //
  //    kNothrowRelocate == false の型でのみ起こる経路。確保失敗 (false) とは別物で、
  //    ここでは例外が呼び出し側へ抜ける。抜けた後もコンテナが有効であること、
  //    すなわち「強い保証」が成り立っていることを実測する。
  // ---------------------------------------------------------------------------

  void TestElementExceptionSafety() {
    GLFD::Test::BeginCase("element-type exceptions leave the container usable (strong guarantee)");

    static_assert(!DynamicArray<Bomb>::kNothrowRelocate,
                  "Bomb must take the copy-relocation path");

    Bomb::Disarm();
    CHECK(Bomb::s_live == 0);

    // (a) 再配置の途中で投げる (TryReserve)
    {
      MockMemoryResource mock;
      {
        DynamicArray<Bomb> a(&mock);
        CHECK(a.TryReserve(4));
        for (int i = 0; i < 4; ++i) {
          a.EmplaceBack(i);            // 予算を消費しない準備用コンストラクタ
        }
        CHECK(a.GetSize() == 4);
        CHECK(Bomb::s_live == 4);

        const size_t capacityBefore = a.GetCapacity();
        const Bomb*  dataBefore     = a.GetData();

        // 4 要素の移送の途中 (2 個目) で投げさせる
        Bomb::s_copyBudget = 2;
        bool threw = false;
        try {
          (void)a.TryReserve(64);
        }
        catch (const BombError&) {
          threw = true;
        }
        Bomb::Disarm();

        CHECK(threw);
        // 再確保は巻き戻る: 新領域は解放され、既存領域がそのまま使われ続ける
        CHECK(a.GetSize() == 4);
        CHECK(a.GetCapacity() == capacityBefore);
        CHECK(a.GetData() == dataBefore);
        // 移送に成功していた 2 個は uninitialized_copy_n が破棄している
        CHECK(Bomb::s_live == 4);

        bool contentsOk = true;
        for (size_t i = 0; i < a.GetSize(); ++i) {
          if (a[i].v != static_cast<int>(i)) { contentsOk = false; }
        }
        CHECK(contentsOk);

        // 例外が抜けた後も使い続けられる
        CHECK(a.TryReserve(64));
        CHECK(a.GetCapacity() == 64);
        CHECK(a.GetSize() == 4);
        CHECK(Bomb::s_live == 4);
      }
      CHECK(Bomb::s_live == 0);            // デストラクタが安全に働いた
      CHECK(mock.LiveBlockCount() == 0);   // 例外経路でも領域が漏れていない
      CHECK(!mock.DoubleFree());
      CHECK(!mock.SizeMismatch());
    }

    // (b) 再配置は成功したが、新要素の構築で投げる (TryPushBack)
    {
      MockMemoryResource mock;
      {
        DynamicArray<Bomb> a(&mock);
        CHECK(a.TryReserve(4));
        for (int i = 0; i < 4; ++i) {
          a.EmplaceBack(i);
        }
        CHECK(a.GetSize() == 4);
        CHECK(a.GetCapacity() == 4);

        const size_t capacityBefore = a.GetCapacity();

        // 移送の 4 回は成功させ、その直後の新要素の構築で投げさせる
        Bomb::s_copyBudget = 4;
        const Bomb extra(99);
        bool threw = false;
        try {
          (void)a.TryPushBack(extra);
        }
        catch (const BombError&) {
          threw = true;
        }
        Bomb::Disarm();

        CHECK(threw);
        // 要素数と内容は不変 = 強い保証
        CHECK(a.GetSize() == 4);
        CHECK(Bomb::s_live == 4 + 1);   // 配列の 4 個 + ローカルの extra

        bool contentsOk = true;
        for (size_t i = 0; i < a.GetSize(); ++i) {
          if (a[i].v != static_cast<int>(i)) { contentsOk = false; }
        }
        CHECK(contentsOk);

        // ただし再確保そのものは巻き戻らない。容量は増えている。
        // これが「状態が完全に不変」ではなく「強い保証」である所以で、
        // ヘッダにも明記してある (std::vector::push_back と同じ強さ)
        CHECK(a.GetCapacity() > capacityBefore);

        // 例外が抜けた後も使い続けられる
        CHECK(a.TryPushBack(extra));
        CHECK(a.GetSize() == 5);
        CHECK(a[4].v == 99);
      }
      CHECK(Bomb::s_live == 0);
      CHECK(mock.LiveBlockCount() == 0);
      CHECK(!mock.DoubleFree());
    }

    // (c) TryResize の要素構築で投げる
    {
      MockMemoryResource mock;
      {
        DynamicArray<Bomb> a(&mock);
        CHECK(a.TryReserve(4));
        for (int i = 0; i < 3; ++i) {
          a.EmplaceBack(i);
        }
        CHECK(a.GetSize() == 3);

        // 3 要素の移送を成功させたうえで、10 個目まで伸ばす途中の
        // 4 回目の既定構築で投げさせる
        Bomb::s_copyBudget    = 3;
        Bomb::s_defaultBudget = 3;
        bool threw = false;
        try {
          (void)a.TryResize(10);
        }
        catch (const BombError&) {
          threw = true;
        }
        Bomb::Disarm();

        CHECK(threw);
        // 途中まで構築した要素は catch 節で破棄され、要素数は据え置き
        CHECK(a.GetSize() == 3);
        CHECK(Bomb::s_live == 3);

        bool contentsOk = true;
        for (size_t i = 0; i < a.GetSize(); ++i) {
          if (a[i].v != static_cast<int>(i)) { contentsOk = false; }
        }
        CHECK(contentsOk);

        CHECK(a.TryResize(6));
        CHECK(a.GetSize() == 6);
        CHECK(Bomb::s_live == 6);
      }
      CHECK(Bomb::s_live == 0);
      CHECK(mock.LiveBlockCount() == 0);
      CHECK(!mock.DoubleFree());
      CHECK(!mock.SizeMismatch());
    }

    // (d) 確保失敗と例外は独立している。確保失敗は例外を投げず false を返す
    {
      MockMemoryResource mock;
      DynamicArray<Bomb> a(&mock);
      mock.SetFailAfter(0);

      bool threw = false;
      bool result = true;
      try {
        result = a.TryPushBack(Bomb(1));
      }
      catch (const BombError&) {
        threw = true;
      }
      CHECK(!threw);
      CHECK(!result);
      CHECK(a.GetSize() == 0);
      CHECK(a.GetCapacity() == 0);
      CHECK(a.GetData() == nullptr);
    }

    CHECK(Bomb::s_live == 0);
  }

  // ---------------------------------------------------------------------------
  // 9. アライメント
  // ---------------------------------------------------------------------------

  struct alignas(32) Wide {
    double a[4];
  };

  void TestAlignment() {
    GLFD::Test::BeginCase("over-aligned elements keep their alignment through Try*");

    MockMemoryResource mock;
    {
      DynamicArray<Wide> a(&mock);
      CHECK(a.TryReserve(4));
      CHECK(reinterpret_cast<std::uintptr_t>(a.GetData()) % alignof(Wide) == 0);

      for (int i = 0; i < 20; ++i) {
        Wide w{};
        w.a[0] = static_cast<double>(i);
        CHECK_QUIET(a.TryPushBack(w));
        CHECK_QUIET(reinterpret_cast<std::uintptr_t>(a.GetData()) % alignof(Wide) == 0);
      }
      ++GLFD::Test::g_checkCount;
      CHECK(a.GetSize() == 20);
      CHECK(a[19].a[0] == 19.0);
    }

    CHECK(!mock.AlignmentMismatch());
    CHECK(mock.LiveBlockCount() == 0);
  }

}

  // ---------------------------------------------------------------------------
  // TryEmplaceBack (ECS 1-1 で追加)
  // ---------------------------------------------------------------------------

  void TestTryEmplaceBack() {
    GLFD::Test::BeginCase("Try*: TryEmplaceBack constructs in place and returns nullptr on failure");

    GLFD::Test::MockMemoryResource mock;
    GLFD::DynamicArray<int>        values(&mock);

    int* const first = values.TryEmplaceBack(7);
    CHECK(first != nullptr);
    CHECK(first != nullptr && *first == 7);
    CHECK(values.GetSize() == 1u);
    CHECK(first == &values[0]);

    // 伸長をまたいでも構築できること
    bool grewCleanly = true;
    for (int i = 1; i < 64; ++i) {
      const int* const p = values.TryEmplaceBack(i);
      if (p == nullptr || *p != i) { grewCleanly = false; break; }
    }
    CHECK(grewCleanly);
    CHECK(values.GetSize() == 64u);
    CHECK(values[0] == 7);

    // **確保に失敗したら nullptr。投げない** (N-2)
    mock.SetFailAfter(mock.AllocateCalls());
    const size_t sizeBeforeFailure = values.GetSize();
    while (values.GetSize() < values.GetCapacity()) {
      if (values.TryEmplaceBack(0) == nullptr) { break; }
    }
    CHECK(values.GetSize() == values.GetCapacity());
    CHECK(values.TryEmplaceBack(1) == nullptr);
    CHECK(values.GetSize() == values.GetCapacity());   // 失敗しても壊れない
    CHECK(sizeBeforeFailure <= values.GetSize());
  }

int main() {
  GLFD::Test::BeginSuite("DynamicArray Try* (1-5a)");

  TestTryEmplaceBack();
  TestSuccessPaths();
  TestFailureLeavesStateUnchanged();
  TestNonTrivialElements();
  TestExhaustiveFailureInjection();
  TestThrowingWrappers();
  TestOverflowGuard();
  TestExistingApiUnchanged();
  TestElementExceptionSafety();
  TestAlignment();

  return GLFD::Test::Summarize();
}
