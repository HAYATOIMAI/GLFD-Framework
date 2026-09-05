/**
 * @file  JsonArena.cpp
 * @brief JsonArena の実装
 */

#include "Core/Json/JsonArena.h"

#include <cassert>
#include <cstring>
#include <new>

namespace GLFD::Json {

  namespace {

    /// 標準アライメント。通常ブロックのデータ領域はこの境界に載る
    constexpr size_t kMaxAlign = alignof(std::max_align_t);

#ifdef _DEBUG
    /**
     * @brief Reset 時に使用済み領域を塗り潰すパターン
     * @note  MSVC CRT の 0xCD(未初期化)/ 0xDD(解放済み)/ 0xFD(ガード)と
     *        区別できるよう、あえて別の値を使う
     */
    constexpr int kResetPattern = 0xA7;
#endif

    /// alignment が「0 でない 2 の冪」であるか
    [[nodiscard]] constexpr bool IsValidAlignment(size_t alignment) noexcept {
      return (alignment != 0) && ((alignment & (alignment - 1)) == 0);
    }

    /**
     * @brief value を alignment の倍数へ切り上げる
     * @return 切り上げ結果。オーバーフローする場合は 0
     * @note   alignment は検証済みの 2 の冪であること
     */
    [[nodiscard]] constexpr size_t AlignUpChecked(size_t value, size_t alignment) noexcept {
      const size_t mask = alignment - 1;
      if (value > SIZE_MAX - mask) {
        return 0;
      }
      return (value + mask) & ~mask;
    }

  }

  /**
   * @brief ブロック鎖のノード。ブロック先頭に配置され、直後がデータ領域になる
   * @note  IMemoryResource::Deallocate が (ptr, size, alignment) を要求するため、
   *        totalSize / alignment の保持は必須。省略すると正しく返却できない
   */
  struct JsonArena::BlockHeader {
    BlockHeader* next;       ///< 鎖の次のブロック(末尾は nullptr)
    std::byte*   data;       ///< データ領域の先頭(ヘッダ直後をアラインした位置)
    size_t       capacity;   ///< データ領域のバイト数
    size_t       used;       ///< データ領域内の使用済みバイト数(バンプカーソル)
    size_t       totalSize;  ///< resource->Allocate へ渡した総バイト数
    size_t       alignment;  ///< resource->Allocate へ渡したアライメント
  };

  JsonArena::JsonArena(Memory::IMemoryResource* resource, size_t blockSize) noexcept
    : m_resource(resource)
    , m_blockSize(blockSize < kMinBlockSize ? kMinBlockSize : blockSize) {
    // ここではメモリを確保しない。最初の Allocate まで遅延する
  }

  JsonArena::~JsonArena() {
    Release();
  }

  JsonArena::JsonArena(JsonArena&& other) noexcept
    : m_resource(other.m_resource)
    , m_head(other.m_head)
    , m_current(other.m_current)
    , m_tail(other.m_tail)
    , m_blockSize(other.m_blockSize)
    , m_usedBytes(other.m_usedBytes)
    , m_allocatedBytes(other.m_allocatedBytes)
    , m_blockCount(other.m_blockCount) {
    // ムーブ元は「有効だが空」。resource と blockSize は残すので、
    // 以後 Allocate すれば新規ブロックを取りに行ける。
    // 鎖を切り離すことでデストラクタの二重解放を防ぐ
    other.m_head           = nullptr;
    other.m_current        = nullptr;
    other.m_tail           = nullptr;
    other.m_usedBytes      = 0;
    other.m_allocatedBytes = 0;
    other.m_blockCount     = 0;
  }

  JsonArena& JsonArena::operator=(JsonArena&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    // 自身が抱えているブロックを先に返却する(リーク防止)
    Release();

    m_resource       = other.m_resource;
    m_head           = other.m_head;
    m_current        = other.m_current;
    m_tail           = other.m_tail;
    m_blockSize      = other.m_blockSize;
    m_usedBytes      = other.m_usedBytes;
    m_allocatedBytes = other.m_allocatedBytes;
    m_blockCount     = other.m_blockCount;

    other.m_head           = nullptr;
    other.m_current        = nullptr;
    other.m_tail           = nullptr;
    other.m_usedBytes      = 0;
    other.m_allocatedBytes = 0;
    other.m_blockCount     = 0;

    return *this;
  }

  void* JsonArena::TryBumpAllocate(BlockHeader* block, size_t size, size_t alignment) noexcept {
    const uintptr_t cursor   = reinterpret_cast<uintptr_t>(block->data) + block->used;
    const size_t    misalign = static_cast<size_t>(cursor & (alignment - 1));
    const size_t    padding  = (misalign != 0) ? (alignment - misalign) : 0;

    // 加算を一度も行わずに判定する。used <= capacity は不変条件なので
    // 以下の減算は決してアンダーフローしない(論点 5)
    size_t available = block->capacity - block->used;
    if (padding > available) {
      return nullptr;
    }
    available -= padding;
    if (size > available) {
      return nullptr;
    }

    std::byte* const result = block->data + block->used + padding;
    block->used += padding + size;
    m_usedBytes += padding + size;
    return result;
  }

  JsonArena::BlockHeader* JsonArena::CreateBlock(size_t payloadSize, size_t alignment) noexcept {
    if (m_resource == nullptr) {
      return nullptr;
    }

    // ブロック自体は「標準アライメント」と「要求アライメント」の大きい方で確保する。
    // こうするとデータ領域の先頭が要求アライメントに載り、
    // このブロックへの最初の確保でパディングが発生しない
    const size_t blockAlign = (alignment > kMaxAlign) ? alignment : kMaxAlign;

    const size_t headerSpace = AlignUpChecked(sizeof(BlockHeader), blockAlign);
    if (headerSpace == 0) {
      return nullptr;  // 切り上げでオーバーフロー
    }
    if (payloadSize > SIZE_MAX - headerSpace) {
      return nullptr;  // 総サイズがオーバーフロー
    }
    const size_t totalSize = headerSpace + payloadSize;

    void* const raw = m_resource->Allocate(totalSize, blockAlign);
    if (raw == nullptr) {
      // R0-3: 落とさず、静かに失敗を伝播させる
      return nullptr;
    }

    BlockHeader* const block = ::new (raw) BlockHeader{};
    block->next      = nullptr;
    block->data      = static_cast<std::byte*>(raw) + headerSpace;
    block->capacity  = payloadSize;
    block->used      = 0;
    block->totalSize = totalSize;
    block->alignment = blockAlign;

    m_allocatedBytes += totalSize;
    ++m_blockCount;
    return block;
  }

  void* JsonArena::Allocate(size_t size, size_t alignment) noexcept {
    // 引数の誤用。R0-3 が禁じているのは「確保失敗で落とすこと」なので、
    // プログラマのミスはデバッグビルドでのみ assert して知らせる
    if (!IsValidAlignment(alignment)) {
      assert(false && "JsonArena::Allocate: alignment must be a non-zero power of two");
      return nullptr;
    }
    if (m_resource == nullptr) {
      return nullptr;
    }
    // 以降でパディング用に alignment - 1 を足し得るため、ここで遮断しておく
    if (size > SIZE_MAX - (alignment - 1)) {
      return nullptr;
    }

    // 1. 現ブロックから前方へ走査し、収まる場所を探す。
    //    Reset 直後は先頭ブロックが空なので即座に決まり、
    //    resource への追加要求は発生しない(R0-4)
    for (BlockHeader* block = m_current; block != nullptr; block = block->next) {
      if (void* const result = TryBumpAllocate(block, size, alignment)) {
        m_current = block;
        return result;
      }
    }

    // 2. 収まる既存ブロックが無いので新規に取得する。
    //    通常ブロックのデータ領域は kMaxAlign 境界にしか載らないため、
    //    過剰アライメント要求では最大 (alignment - kMaxAlign) のパディングを見込む。
    //    これにより「新規ブロックを作ったのに入らない」無限ループを防ぐ
    const size_t slack  = (alignment > kMaxAlign) ? (alignment - kMaxAlign) : 0;
    const size_t needed = size + slack;  // 上のガードによりオーバーフローしない

    if (needed > m_blockSize) {
      // R0-2: blockSize を超える単発要求には専用ブロックを充てる。
      // データ領域は blockAlign 境界に載るためパディングは発生せず、size ちょうどで足りる
      BlockHeader* const block = CreateBlock(size, alignment);
      if (block == nullptr) {
        return nullptr;
      }

      // 論点 2: 専用ブロックは鎖の「現在位置の直後」へ挿入し、m_current は動かさない。
      // 末尾へ繋いで m_current を進めると、現ブロックの残り領域を丸ごと捨ててしまう
      if (m_current != nullptr) {
        block->next     = m_current->next;
        m_current->next = block;
        if (m_tail == m_current) {
          m_tail = block;
        }
      }
      else {
        // 鎖が空の場合のみここへ来る。末尾へ追加し、現在位置も合わせる
        m_head    = block;
        m_tail    = block;
        m_current = block;
      }

      void* const result = TryBumpAllocate(block, size, alignment);
      assert(result != nullptr && "dedicated block must satisfy the request");
      return result;
    }

    // 通常ブロックを末尾へ追加する
    BlockHeader* const block = CreateBlock(m_blockSize, alignment);
    if (block == nullptr) {
      return nullptr;
    }

    if (m_tail != nullptr) {
      m_tail->next = block;
    }
    else {
      m_head = block;
    }
    m_tail    = block;
    m_current = block;

    void* const result = TryBumpAllocate(block, size, alignment);
    assert(result != nullptr && "fresh block must satisfy the request");
    return result;
  }

  void JsonArena::Reset() noexcept {
    for (BlockHeader* block = m_head; block != nullptr; block = block->next) {
#ifdef _DEBUG
      // 使用済みだった範囲のみを塗る。容量全体を塗ると Reset のコストが
      // 実使用量ではなく確保量に比例してしまう
      if (block->used != 0) {
        std::memset(block->data, kResetPattern, block->used);
      }
#endif
      block->used = 0;
    }

    // 論点 3: ブロックは 1 つも返却しない。
    // StackResource のように Deallocate が no-op なリソース上で返却すると、
    // その領域は FreeToMarker まで回収されず永久に失われる
    m_current   = m_head;
    m_usedBytes = 0;
  }

  void JsonArena::Release() noexcept {
    BlockHeader* block = m_head;
    while (block != nullptr) {
      BlockHeader* const next = block->next;

      // ヘッダを破棄する前に Deallocate 用の情報を退避する
      void* const  raw       = block;
      const size_t totalSize = block->totalSize;
      const size_t alignment = block->alignment;

      block->~BlockHeader();
      m_resource->Deallocate(raw, totalSize, alignment);

      block = next;
    }

    m_head           = nullptr;
    m_current        = nullptr;
    m_tail           = nullptr;
    m_usedBytes      = 0;
    m_allocatedBytes = 0;
    m_blockCount     = 0;
  }

}
