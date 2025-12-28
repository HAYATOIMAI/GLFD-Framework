#pragma once
#include <cstddef>
#include <cstdint>
#include <cassert>

namespace GLFD::Memory {
  /**
   * @brief アライメント調整に必要なパディングサイズを計算
   * @param address 現在のアドレス
   * @param alignment 要求アライメント (2の累乗であること)
   * @return 必要なバイト数
   */
  [[nodiscard]] constexpr size_t GetAlignmentPadding(size_t address, size_t alignment) noexcept {
    // 2の累乗チェック
    assert((alignment != 0) && ((alignment & (alignment - 1)) == 0));

    size_t multiplier = (address / alignment) + 1;
    size_t alignedAddress = multiplier * alignment;
    size_t padding = alignedAddress - address;

    // 既に揃っている場合は padding = alignment になってしまうため補正
    if (padding == alignment) return 0;

    return padding;
  }

  /**
   * @brief ポインタ版
   */
  [[nodiscard]] inline size_t GetAlignmentPadding(const void* ptr, size_t alignment) noexcept {
    return GetAlignmentPadding(reinterpret_cast<size_t>(ptr), alignment);
  }
}
