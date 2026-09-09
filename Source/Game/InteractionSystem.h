#pragma once
#include "../ECS/Registry.h"
#include "../ECS/View.h"
#include "../Threading/JobSystem.h"
#include "../Core/InputSystem.h"
#include "../ECS/Components.h"
#include "../Graphics/SimpleWindow.h" // 座標変換用
#include <cmath>

namespace GLFD::Systems {
  class InteractionSystem {
  public:
    /// @param explosionRadius / explosionForce / screenScale 設定由来 (interaction)
    static void Update(ECS::Registry& registry,
                       Thread::JobSystem& jobSystem,
                       const Core::InputSystem& input,
                       const Graphics::SimpleWindow& window,
                       float explosionRadius,
                       float explosionForce,
                       float screenScale) {
     
      // 左クリックした瞬間だけ処理
      if (input.IsTriggered(GLFD::Core::KeyCode::MouseLeft)) {
        // 1. マウス座標をワールド座標へ変換
        // RenderSystemでの変換式の逆を行う
        int mx = input.GetMouseX();
        int my = input.GetMouseY();
        int halfW = window.GetWidth() / 2;
        int halfH = window.GetHeight() / 2;
        float scale = screenScale; // RenderSystemと合わせる

        float worldX = (mx - halfW) / scale;
        float worldY = (my - halfH) / scale; // Y軸の向きに注意（GDIは下が+なのでそのまま）

        // 2. 爆発処理（並列実行）
        // 全エンティティに対して距離チェックを行い、近ければ吹き飛ばす

        // 位置と速度の**組**を回す (1-5 / R-32)。以前は 2 本の dense 配列を
        // 同じ添字で触っていた
        auto view = registry.View<Components::Position, Components::Velocity>();
        const size_t count = view.BaseSize();
        if (count == 0) { return; }

        size_t threadCount = System::WorkerThreadCount();   // 0 を返し得るので丸める (ECS-0 3-7)
        size_t batchSize = count / threadCount;
        Thread::JobCounter counter;
        auto handle = jobSystem.CreateHandle(counter);

        for (size_t t = 0; t < threadCount; ++t) {
          size_t start = t * batchSize;
          size_t end = (t == threadCount - 1) ? count : start + batchSize;

          jobSystem.KickJob([view, start, end, worldX, worldY,
                             explosionRadius, explosionForce]() {
              for (auto [entity, pos, vel] : view.Slice(start, end)) {
                (void)entity;
                float dx = pos.x - worldX;
                float dy = pos.y - worldY;
                float distSq = dx * dx + dy * dy;

                // 範囲内なら
                if (distSq < explosionRadius * explosionRadius && distSq > 0.0001f) {
                  float dist = std::sqrt(distSq);
                  // 中心に近いほど強く
                  float power = (1.0f - (dist / explosionRadius)) * explosionForce;

                  // 速度に加算（吹き飛ばす）
                  vel.vx += (dx / dist) * power;
                  vel.vy += (dy / dist) * power;
                }
              }
            }, &handle);
        }
        jobSystem.WaitFor(handle);
      }
    }
  };
}