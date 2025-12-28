#pragma once
#include <immintrin.h>


namespace GLFD::Components {
  // 16バイトアライメントされた位置コンポーネント
  struct alignas(16) Position {
    float x, y, z;
    float w; // パディング (SIMDレジスタへのロードを安全にするため)

    // デバッグ用出力などは必要に応じて
  };

  // 16バイトアライメントされた速度コンポーネント
  struct alignas(16) Velocity {
    float vx, vy, vz;
    float vw; // パディング
  };
}