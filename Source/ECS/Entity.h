#pragma once
#include <cstdint>
#include <limits>


namespace GLFD::ECS {
  /**
   * @brief エンティティIDを表す型
   */
  using Entity = uint32_t;

  constexpr Entity NullEntity = UINT32_MAX;

  // エンティティIDの最大数（Sparse配列のサイズ決定に使用）
  // タイトルに合わせて調整。
  // 暫定的に100万に指定する。
  constexpr size_t MaxEntities = 1000000;
}