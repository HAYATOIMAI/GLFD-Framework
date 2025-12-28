#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <stdexcept>
#include <new>
#include <utility>
#include <malloc.h>
#include "MemoryUtils.h"

namespace  GLFD::Memory {
  class StackAllocator final {
  public:
    // ロールバック用のマーカー（現在のメモリ使用位置を記録するもの）
    using Marker = size_t;

    /**
     * @brief コンストラクタ
     * @param sizeBytes 管理するメモリバッファの総サイズ（バイト）
     */
    explicit StackAllocator(size_t sizeBytes);

    /**
     * @brief デストラクタ
     */
    ~StackAllocator();

    // コピー禁止 (所有権を一箇所にするため)
    StackAllocator(const StackAllocator&) = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;

    /**
     * @brief メモリを確保します。
     * @param size 確保するサイズ（バイト）
     * @param alignment アライメント（バイト、2の累乗である必要あり）
     * @return 確保されたメモリの先頭ポインタ
     */
    [[nodiscard]] void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t));

    /**
     * @brief 型を指定してメモリを確保し、コンストラクタを呼び出します (Placement New)
     * @tparam T 確保する型
     * @tparam Args コンストラクタ引数
     * @return 構築されたオブジェクトへのポインタ
     */
    template <typename T, typename... Args>
    [[nodiscard]] T* New(Args&&... args) {
      void* buf = Allocate(sizeof(T), alignof(T));
      if (!buf) return nullptr;

      return new(buf) T(std::forward<Args>(args)...);
    }

    /**
     * @brief 現在の状態（マーカー）を取得します。
     * @return 現在のオフセット値
     */
    [[nodiscard]] Marker GetMarker() const noexcept { return m_offset; }

    /**
     * @brief 指定したマーカーの位置までメモリを解放（巻き戻し）します。
     * @param marker 戻したい位置のマーカー
     */
    void FreeToMarker(Marker marker) noexcept;

    /**
     * @brief 全メモリを解放（リセット）します。
     */
    void Clear() noexcept { m_offset = 0; }

    // ゲッター
    [[nodiscard]] size_t GetUsedMemory() const noexcept { return m_offset; }
    [[nodiscard]] size_t GetTotalSize() const noexcept { return m_totalSize; }

  private:
    std::byte* m_startPtr = nullptr; // メモリブロックの先頭
    size_t   m_totalSize = 0;        // 全サイズ
    size_t   m_offset = 0;           // 現在の使用位置（先頭からのバイト数）
  };
}