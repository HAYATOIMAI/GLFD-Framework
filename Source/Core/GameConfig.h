#pragma once

namespace GLFD{
  struct GameConfig {
    // ウィンドウ設定
    static constexpr int WindowWidth = 1024;
    static constexpr int WindowHeight = 768;
    static constexpr const wchar_t* WindowTitle = L"ECS Boids Engine";

    // シミュレーション設定
    static constexpr size_t ParticleCount = 20000;
    static constexpr float TimeStep = 0.016f; // 固定DT推奨

    // ワールド境界 (画面端)
    static constexpr float WorldBoundsX = 85.0f; // 画面アスペクト比に合わせて調整
    static constexpr float WorldBoundsY = 64.0f;
  };
}