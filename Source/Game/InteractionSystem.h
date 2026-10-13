#pragma once
#include "../ECS/Registry.h"
#include "../ECS/View.h"
#include "../Threading/JobSystem.h"
#include "../Threading/ParallelFor.h"
#include "../Core/InputSystem.h"
#include "../ECS/Components.h"
#include "../Graphics/ScreenMapping.h" // 画素 → 世界。描画と同じ対応の逆 (ECS 2-13)
#include <cmath>

namespace GLFD::Systems {
  class InteractionSystem {
  public:
    /// 1 本あたりの最小の対象数 (ECS 2-6、Threading/ParallelFor.h)。**未計測。**
    /// 左クリックしないと動かないので、ベンチで 1 体あたりの費用を測れていない。
    /// 今までと同じ本数(1 = ワーカーの数)のまま置いている
    static constexpr size_t kGrain = 1;

    /**
     * @param screen 世界と画面の対応。**描画(`RenderSystem`)に渡すのと同じもの** (ECS 2-13)
     * @param explosionRadius / explosionForce 設定由来 (interaction)
     * @details クリックした画素を `screen.PixelToWorld` で世界に直す。2-12 までは `interaction.screenScale`
     *          (1024x768 の窓での 1 単位あたりの画素)で割っていて、**縦の向きが描画と逆**だった
     *          (上をクリックすると下で爆発した。2-13 手順1 で実測)
     */
    static void Update(ECS::Registry& registry,
                       Thread::JobSystem& jobSystem,
                       const Core::InputSystem& input,
                       const Graphics::ScreenMapping& screen,
                       float explosionRadius,
                       float explosionForce) {
      // 左クリックした瞬間だけ処理
      if (!input.IsTriggered(GLFD::Core::KeyCode::MouseLeft)) { return; }
      const Graphics::WorldPoint center = screen.PixelToWorld(input.GetMouseX(), input.GetMouseY());
      Explode(registry, jobSystem, center, explosionRadius, explosionForce);
    }

    /// 世界の点 `center` から `explosionRadius` の内側を吹き飛ばす
    static void Explode(ECS::Registry& registry,
                        Thread::JobSystem& jobSystem,
                        Graphics::WorldPoint center,
                        float explosionRadius,
                        float explosionForce) {
      const float worldX = center.x;
      const float worldY = center.y;

      // 爆発処理（並列実行）
      // 全エンティティに対して距離チェックを行い、近ければ吹き飛ばす

      // 位置と速度の**組**を回す (1-5 / R-32)。以前は 2 本の dense 配列を
      // 同じ添字で触っていた
      auto view = registry.View<Components::Position, Components::Velocity>();
      const size_t count = view.BaseSize();
      if (count == 0) { return; }

      // 分け方は ParallelFor.h の 1 か所 (ECS 2-6)
      Thread::ParallelForChunks(jobSystem, count, kGrain, [&](size_t start, size_t end) {
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
          });
    }
  };
}