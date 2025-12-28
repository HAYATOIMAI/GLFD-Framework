#pragma once

namespace GLFD::Components {
  struct alignas(16) BoidAgent {
    // 視界半径
    float viewRadius = 5.0f;

    // 各ルールの重み付け（性格）
    float separationWeight = 1.5f; // 離れる力を強めにするとぶつかりにくい
    float alignmentWeight = 1.0f;
    float cohesionWeight = 1.0f;
  };
}