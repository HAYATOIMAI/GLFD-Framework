#pragma once

/**
 * @file  HitSystem.h
 * @brief ECS 1-8: 弾が敵に重なったことを検出する(並列)
 *
 * @details
 *  ## `CollisionSystem` を使わない理由(1-8 論点2)
 *  `CollisionSystem` は `Position` を持つ全員で組んだグリッドを引き、
 *  **近傍の打ち切り(16 件)を相手の種類を問わず消費する**。群れや敵の塊の中では、
 *  弾が敵に重なっていても先に別のものが 16 件を使い切り、判定されない。
 *  当たるかどうかがバケットの鎖の並び順で決まってしまう。
 *
 *  ここでは**敵だけで組んだグリッド**を引く。打ち切りを使うのは標的だけになる。
 *  `CollisionSystem` は Boid と共用なので触らない。
 *
 *  ## このシステムは検出するだけ
 *  `HitEvent` を発行するだけで、成分も構造も変えない。**並列ループから
 *  `CommandBuffer` へは積めない**(R-18)。結果はイベントキューでメインスレッドへ
 *  渡り、`DispatchEvents` の購読者が解決する。
 *
 *  ## イプシロンが無い
 *  `CollisionSystem` の `distSq > 0.0001f` は押し返しの `dx / dist` を守るための
 *  もので、そのせいで**同じ位置の 2 体は衝突しない**(§20.7)。ここは割り算を
 *  しないので条件を持たない。完全に重なった弾と敵も当たる。
 */

#include "../Core/GameContext.h"
#include "../Core/HardwareConstants.h"
#include "../ECS/Components.h"
#include "../ECS/Entity.h"
#include "../ECS/Registry.h"
#include "../ECS/View.h"
#include "../Events/EventBus.h"
#include "../Events/Events.h"
#include "../Physics/CollisionComponents.h"
#include "../Physics/SpatialHashGrid.h"
#include "../Threading/JobSystem.h"
#include "../Threading/ParallelFor.h"
#include "GridBulidSystem.h"
#include "SurvivorComponents.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace GLFD::Systems {

  class HitSystem {
  public:
    /// 1 発の弾が見る候補の上限。**候補は敵だけ**なので、敵以外に消費されない
    static constexpr int kMaxChecks = 16;

    /// 1 本あたりの最小の対象数 (ECS 2-6、Threading/ParallelFor.h)。
    /// **3a では 1**(今までと同じ本数 = ワーカーの数)。3b で実測から決める
    static constexpr std::size_t kGrain = 1;

    /**
     * @brief 標的のグリッドを組む。**組み方と引き方の対はここ 1 箇所で決める**
     *
     * @warning **`Update` の `View` と型の並びを必ず揃えること。**
     *          グリッドに入る番号は `View<Position, Health, Collider>` の
     *          **基準プールの dense 添字**である。基準は最小のプールで、同じ大きさなら
     *          並びの先頭が選ばれる。並びを変えると、構造が変わっていなくても
     *          **別のプールの添字として読む**ことになる(1-5 の「2 つの View は
     *          基準の添字が違う」と同じ罠)。
     */
    static void BuildGrid(GameContext& ctx) {
      GridBuildSystem::Update<Components::Health, Components::Collider>(ctx);
    }

    /**
     * @brief グリッドが組まれた後に構造が変わっていないか (R-43)
     * @note  `Update` の `assert` と同じ判定。Release でも数えられるよう公開する
     */
    [[nodiscard]] static bool GridMatches(const GameContext& ctx) noexcept {
      return ctx.grid != nullptr
          && ctx.grid->BuildStamp() == ctx.registry->StructureVersion();
    }

    static void Update(GameContext& ctx) {
      if (ctx.grid == nullptr) { return; }

      auto& grid = *ctx.grid;
      auto& bus  = *ctx.eventBus;

      // **`BuildGrid` と同じ並び**(上の @warning)
      const auto targets = ctx.registry->View<Components::Position,
                                              Components::Health,
                                              Components::Collider>();
      const std::size_t        targetCount = targets.BaseSize();
      const ECS::Entity* const owners      = targets.BaseEntities();

      const auto bullets = ctx.registry->View<Components::Position,
                                              Components::Damage,
                                              Components::Lifetime,
                                              Components::Collider>();
      const std::size_t count = bullets.BaseSize();
      if (count == 0 || targetCount == 0 || owners == nullptr) { return; }

      // 並列ループへ入る前に 1 回だけ突き合わせる (1-5 / R-43)
      assert(GridMatches(ctx)
             && "HitSystem: the registry changed shape after the target grid was built. "
                "Spawning with the immediate API belongs before GridBuild (1-8).");

      // 分け方は ParallelFor.h の 1 か所 (ECS 2-6)
      Thread::ParallelForChunks(*ctx.jobSystem, count, kGrain, [&](std::size_t start, std::size_t end) {
          for (auto entry : bullets.Slice(start, end)) {
            const ECS::Entity           self = std::get<0>(entry);
            const Components::Position& posA = std::get<1>(entry);
            const Components::Lifetime& life = std::get<3>(entry);
            const float                 rA   = std::get<4>(entry).radius;

            // 使い切った弾(同じフレームで既に当たって破棄を積まれた)は見ない
            if (life.remaining <= 0.0f) { continue; }

            int checks = 0;
            grid.Query(posA, [&](std::uint32_t id) -> bool {
              if (id >= targetCount) { return true; }   // 純粋な防御

              const ECS::Entity enemy = owners[id];
              if (enemy == self) { return true; }       // 弾と敵を兼ねる組み合わせへの防御

              if (++checks > kMaxChecks) { return false; }

              const Components::Position* const posB = targets.Find<Components::Position>(enemy);
              const Components::Collider* const colB = targets.Find<Components::Collider>(enemy);
              if (posB == nullptr || colB == nullptr) { return true; }

              const float dx    = posA.x - posB->x;
              const float dy    = posA.y - posB->y;
              const float dz    = posA.z - posB->z;
              const float reach = rA + colB->radius;
              if (dx * dx + dy * dy + dz * dz < reach * reach) {
                // **重なった組を全部出す。** 1 発で 1 体だけ倒すという規則は
                // 購読者側が値で守る(論点3 の c)。どちらが倒れるかは配信の順で
                // 決まり、配信の順はワーカーの競合で**実行ごとに変わる**
                bus.Publish<Events::HitEvent>({ self, enemy });
              }
              return true;
            });
          }
        });
    }
  };

}
