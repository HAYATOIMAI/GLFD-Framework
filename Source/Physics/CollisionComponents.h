#pragma once

namespace GLFD::Components {
  struct alignas(16) Collider {
    float radius;
    float padding[3]; // 16バイトアライメント維持
  };
}
