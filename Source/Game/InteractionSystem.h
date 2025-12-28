#pragma once
#include "../ECS/Registry.h"
#include "../Threading/JobSystem.h"
#include "../Core/InputSystem.h"
#include "../ECS/Components.h"
#include "../Graphics/SimpleWindow.h" // 座標変換用
#include <cmath>

namespace GLFD::Systems {
  class InteractionSystem {
  public:
    static void Update(ECS::Registry& registry, 
                       Thread::JobSystem& jobSystem,
                       const Core::InputSystem& input, 
                       const Graphics::SimpleWindow& window) {
     
      // 左クリックした瞬間だけ処理
      if (input.IsTriggered(GLFD::Core::KeyCode::MouseLeft)) {
        // 1. マウス座標をワールド座標へ変換
        // RenderSystemでの変換式の逆を行う
        int mx = input.GetMouseX();
        int my = input.GetMouseY();
        int halfW = window.GetWidth() / 2;
        int halfH = window.GetHeight() / 2;
        float scale = 6.0f; // RenderSystemと合わせる

        float worldX = (mx - halfW) / scale;
        float worldY = (my - halfH) / scale; // Y軸の向きに注意（GDIは下が+なのでそのまま）

        // 2. 爆発処理（並列実行）
        // 全エンティティに対して距離チェックを行い、近ければ吹き飛ばす

        auto& positions = registry.View<Components::Position>();
        auto& velocities = registry.View<Components::Velocity>();

        size_t count = positions.GetSize();
        auto* pData = positions.GetData();
        auto* vData = velocities.GetData();

        size_t threadCount = std::thread::hardware_concurrency();
        size_t batchSize = count / threadCount;
        Thread::JobCounter counter;
        auto handle = jobSystem.CreateHandle(counter);

        // 爆発パラメータ
        float explosionRadius = 20.0f;
        float explosionForce = 50.0f;

        for (size_t t = 0; t < threadCount; ++t) {
          size_t start = t * batchSize;
          size_t end = (t == threadCount - 1) ? count : start + batchSize;

          jobSystem.KickJob([=]() {
              for (size_t i = start; i < end; ++i) {
                float dx = pData[i].x - worldX;
                float dy = pData[i].y - worldY;
                float distSq = dx * dx + dy * dy;

                // 範囲内なら
                if (distSq < explosionRadius * explosionRadius && distSq > 0.0001f) {
                  float dist = std::sqrt(distSq);
                  // 中心に近いほど強く
                  float power = (1.0f - (dist / explosionRadius)) * explosionForce;

                  // 速度に加算（吹き飛ばす）
                  vData[i].vx += (dx / dist) * power;
                  vData[i].vy += (dy / dist) * power;
                }
              }
            }, &handle);
        }
        jobSystem.WaitFor(handle);
      }
    }
  };
}