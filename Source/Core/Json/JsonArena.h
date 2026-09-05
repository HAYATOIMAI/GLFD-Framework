#pragma once

/**
 * @file  JsonArena.h
 * @brief JSON サブシステム用の単調増加(モノトニック)アリーナアロケータ
 *
 * @details
 *  GLFD JSON サブシステム Layer 0。要件 R0-1 〜 R0-5 を実装する。
 *  例外・RTTI・STL コンテナを一切使用しない。
 */

#include <cstddef>
#include <cstdint>

#include "Core/MemoryResource.h"

namespace GLFD::Json {

  /**
   * @brief IMemoryResource 上に構築するバンプポインタ方式のアリーナ
   *
   * @details
   *  ## 生存期間の契約 (R0-5) — 最重要
   *  本アリーナから確保された **全てのメモリは JsonArena より長生きしない**。
   *  `Reset()` / `Release()` / デストラクタ / ムーブ代入のいずれかが走った時点で、
   *  それ以前に返したポインタは全て無効(ダングリング)になる。
   *  Document 破棄後の Value / StringView 参照が未定義動作になるのはこの契約による。
   *
   *  ## 解放モデル (R0-1)
   *  個別解放 API を持たない。確保はポインタ加算とアライメント調整のみで行う。
   *  デストラクタも呼ばれないため、非トリビアルな型を placement new した場合の
   *  デストラクタ呼び出しは呼び出し側の責任。
   *
   *  ## 失敗時の挙動 (R0-3)
   *  IMemoryResource からの確保に失敗した場合、`Allocate` は `nullptr` を返す。
   *  アボート・例外・assert のいずれも行わない。上位層がこれを
   *  `ErrorCode::OutOfMemory` へ変換する。
   *  引数が不正(alignment が 0 / 2 の冪でない)な場合も `nullptr` を返すが、
   *  こちらはプログラマのミスなのでデバッグビルドでは併せて assert する。
   *
   *  ## スレッド安全性
   *  **単一スレッド前提**。同一インスタンスへの並行アクセスは同期されない。
   *  異なるインスタンス同士は独立しており、JobSystem 上で並列に使用できる。
   *
   *  ## メトリクスの定義 (曖昧さを残さないため厳密に定義する)
   *  - `AllocatedBytes()` : 全ブロックについて IMemoryResource::Allocate へ要求した
   *                         バイト数の総和。**ブロックヘッダと内部パディングを含む**。
   *                         Release 時に Deallocate へ渡すサイズの総和と厳密に一致する。
   *  - `UsedBytes()`      : バンプカーソルが消費したバイト数の総和。
   *                         **確保間のアライメントパディングを含み**、
   *                         ブロックヘッダと未使用の末尾領域は**含まない**。
   *  - `BlockCount()`     : 鎖に繋がっているブロック数。専用ブロックおよび
   *                         Reset 後の未使用ブロックも数える。
   */
  class JsonArena final {
  public:
    /// 既定のブロックサイズ(データ領域のバイト数。ヘッダは別途上乗せされる)
    static constexpr size_t kDefaultBlockSize = 64 * 1024;

    /// ブロックサイズの下限。これを下回る指定は本値へ引き上げられる
    static constexpr size_t kMinBlockSize = 256;

    /**
     * @brief コンストラクタ
     * @param resource  ブロックの確保元。nullptr を渡した場合、Allocate は常に nullptr を返す
     * @param blockSize 1 ブロックあたりのデータ領域サイズ。kMinBlockSize 未満は切り上げ
     * @note  この時点ではメモリを確保しない(最初の Allocate まで遅延する)
     */
    explicit JsonArena(Memory::IMemoryResource* resource,
                       size_t blockSize = kDefaultBlockSize) noexcept;

    /// デストラクタ。Release() を呼び全ブロックを resource へ返却する
    ~JsonArena();

    JsonArena(JsonArena&& other) noexcept;
    JsonArena& operator=(JsonArena&& other) noexcept;

    JsonArena(const JsonArena&) = delete;
    JsonArena& operator=(const JsonArena&) = delete;

    /**
     * @brief アリーナからメモリを確保する
     * @param size      確保するバイト数。0 を渡した場合も有効なポインタを返す
     * @param alignment アライメント。0 でない 2 の冪であること。過剰アライメント
     *                  (SIMD 用の 16 / 32 / 64 等)にも対応する
     * @return 確保された領域の先頭。失敗時は nullptr (R0-3)
     * @note  返された領域はゼロ初期化されない
     */
    [[nodiscard]] void* Allocate(size_t size,
                                 size_t alignment = alignof(std::max_align_t)) noexcept;

    /**
     * @brief T を count 個ぶん格納できる生メモリを確保する
     * @return 確保された領域。失敗時は nullptr
     * @note  コンストラクタは呼ばれない。返るのは未初期化の生メモリである
     */
    template <class T>
    [[nodiscard]] T* AllocateArray(size_t count) noexcept {
      // 乗算のオーバーフローを事前に遮断する。
      // ラップアラウンドして「小さいバッファ」を返してはならない(論点 5)
      if (count > (SIZE_MAX / sizeof(T))) {
        return nullptr;
      }
      return static_cast<T*>(Allocate(count * sizeof(T), alignof(T)));
    }

    /**
     * @brief ブロック鎖を保持したまま、全てのバンプカーソルを先頭へ巻き戻す (R0-4)
     * @details
     *  ブロックを resource へ返却しないため、Reset 後の再確保では
     *  IMemoryResource::Allocate が追加で呼ばれない。フレーム単位の再利用向け。
     *  StackResource のように Deallocate が no-op なリソース上でも
     *  領域を食い潰さないのはこの設計による。
     *  デバッグビルドでは使用済み領域を既知パターンで塗り潰し、
     *  use-after-reset を検出しやすくする。
     */
    void Reset() noexcept;

    /**
     * @brief 全ブロックを IMemoryResource へ返却する
     * @details 呼び出し後もアリーナは有効で、再度 Allocate すれば新規ブロックを取りに行く
     */
    void Release() noexcept;

    /// バンプカーソルが消費したバイト数(パディング込み / ブロックヘッダ除く)
    [[nodiscard]] size_t UsedBytes() const noexcept { return m_usedBytes; }

    /// IMemoryResource へ要求したバイト数の総和(ブロックヘッダ込み)
    [[nodiscard]] size_t AllocatedBytes() const noexcept { return m_allocatedBytes; }

    /// 鎖に繋がっているブロック数
    [[nodiscard]] size_t BlockCount() const noexcept { return m_blockCount; }

    /// 確保元のリソース。ムーブ元でも保持され続ける
    [[nodiscard]] Memory::IMemoryResource* Resource() const noexcept { return m_resource; }

    /// 1 ブロックあたりのデータ領域サイズ(コンストラクタで正規化済みの値)
    [[nodiscard]] size_t BlockSize() const noexcept { return m_blockSize; }

  private:
    /// ブロックヘッダ。定義は JsonArena.cpp 側に置き、外部へ公開しない
    struct BlockHeader;

    /// 指定ブロックのバンプカーソルから確保を試みる。入らなければ nullptr
    void* TryBumpAllocate(BlockHeader* block, size_t size, size_t alignment) noexcept;

    /// resource から新規ブロックを取得する。失敗時は nullptr
    BlockHeader* CreateBlock(size_t payloadSize, size_t alignment) noexcept;

    Memory::IMemoryResource* m_resource = nullptr;

    BlockHeader* m_head    = nullptr;  // 鎖の先頭
    BlockHeader* m_current = nullptr;  // 現在バンプ中のブロック
    BlockHeader* m_tail    = nullptr;  // 鎖の末尾(新規ブロックの追加先)

    size_t m_blockSize      = kDefaultBlockSize;
    size_t m_usedBytes      = 0;
    size_t m_allocatedBytes = 0;
    size_t m_blockCount     = 0;
  };

}
