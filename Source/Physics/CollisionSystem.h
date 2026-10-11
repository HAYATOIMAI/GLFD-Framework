#pragma once

#include "Threading/JobSystem.h"
#include "Threading/ParallelFor.h"
#include "ECS/Registry.h"
#include "ECS/Components.h"
#include "CollisionComponents.h"
#include "SpatialHashGrid.h"
#include "Events/EventBus.h"
#include "Events/Events.h"
#include "../Core/GameContext.h"
#include "../Core/HardwareConstants.h"
#include "ECS/View.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>
#include <tuple>

namespace GLFD::Systems {
  /// 1 刻みの衝突の検出の結果 (ECS 2-12)。**観測は数で行う**(イベントを配らなくても、検出が動いていることが見える)
  struct CollisionStepResult {
    std::uint32_t contacts  = 0;   ///< 検出した重なり。1 体の側から 1 件(1 組は最大 2 件。16 件の打ち切りで片側だけのこともある)
    std::size_t   colliders = 0;   ///< 衝突体(Position + Velocity + Collider)の数
  };

  class CollisionSystem {
  public:
    /// 1 体あたりの費用 [us]。CollisionSystem(20000 体、30-300 フレーム目。群れの密度で変わる)をメインだけで回して測った値
    /// (ECS 2-6 手順2、i7-12700KF、Release)。粒度 = 1 本あたりの目標の仕事 / これ
    /// (Threading/ParallelFor.h の GrainFor)。**機械が変わったら測り直す値**
    static constexpr double kCostPerEntityUs = 0.093;
    static constexpr size_t kGrain = Thread::GrainFor(kCostPerEntityUs);

    // メイン処理
    static CollisionStepResult Update(GameContext& context) {

      auto& eventBus = *context.eventBus;
      auto& grid = *context.grid;

      // グリッドの基準は Position 単独の View (1-5 論点3)。BoidSystem が
      // 同じ基準で組んでいる
      auto gridView = context.registry->View<Components::Position>();
      const size_t gridCount = gridView.BaseSize();
      const Components::Position* const gridPositions = gridView.BaseComponents();
      const ECS::Entity* const gridOwners = gridView.BaseEntities();

      // 自分側は 3 成分の**組** (R-32)
      auto view = context.registry->View<Components::Position,
                                         Components::Velocity,
                                         Components::Collider>();
      const size_t count = view.BaseSize();
      CollisionStepResult result;
      result.colliders = count;
      if (count == 0 || gridCount == 0 || gridPositions == nullptr) return result;

      // **1-7: `Clear()` を消した。これが衝突検出の修復そのものである。**
      //
      // 以前はここで `Clear()` してから `Query` しており、**空のバケットを
      // 20,000 x 27 セルぶん辿るだけで衝突は 1 件も出なかった** (ECS-0 1-1)。
      // 組んだ人(`GridBuildSystem`)と読む人(ここ)が別なのに、読む側が
      // 消していた。**このシステムは読むだけである。**

      // グリッドを使って近傍を検索し、衝突応答を行う

      // 並列ループへ入る前に 1 回だけ突き合わせる (1-5 / R-24)
      assert(grid.BuildStamp() == context.registry->StructureVersion()
             && "CollisionSystem: the registry changed shape after the grid was built. "
                "The dense indices stored in the grid no longer mean what they meant (1-5).");

      // 分け方は ParallelFor.h の 1 か所 (ECS 2-6)
      // **購読者が居るときだけ配る** (ECS 2-12 の B')。刻みの頭でメインから 1 回だけ聞き、並列の本体には
      // 読んだ値を渡す(購読の足し外しは刻みの間のメインでしか起きない)。2-11 までは購読者が数えるだけの
      // ために配っていて、密な群れで 1 刻み最大 12 万件を積み、1 件 43～136 ns 払って溢れていた。
      // **購読者が居れば今までどおり 1 件ずつ配る。** ただし、その規模で使えることの保証ではない(2-12 の記録)
      const bool publish = eventBus.SubscriberCountOf<Events::CollisionEvent>() != 0u;
      // 検出した数。**塊ごとに手元で数え、塊の終わりに 1 回だけ足す**(1 件ごとの原子的な操作は持たない)
      std::atomic<std::uint32_t> contacts{ 0u };

      Thread::ParallelForChunks(*context.jobSystem, count, kGrain, [&](size_t start, size_t end) {
          std::uint32_t found = 0;
          for (auto entry : view.Slice(start, end)) {
            const ECS::Entity     self = std::get<0>(entry);
            Components::Position& posA = std::get<1>(entry);
            Components::Velocity& velA = std::get<2>(entry);
            const float           rA   = std::get<3>(entry).radius;

            int checkCount = 0;
            const int MAX_CHECKS = 16;

            // 近傍探索
            grid.Query(posA, [&](uint32_t neighborId) -> bool {
              if (neighborId >= gridCount) return true;   // 純粋な防御

              // **自己スキップは `Entity` で行う** (1-5)。反復側の添字と
              // グリッドの添字は別の View のものなので、添字では比べられない
              const ECS::Entity other = gridOwners[neighborId];
              if (other == self) return true; // 自分自身は無視して継続

              // 上限チェック
              if (++checkCount > MAX_CHECKS) {
                return false;
              }

              const Components::Position& posB = gridPositions[neighborId];

              // 半径は `Entity` から引く。Position の dense 添字では引けない
              const Components::Collider* const colliderB =
                  view.Find<Components::Collider>(other);
              if (colliderB == nullptr) return true;
              const float rB = colliderB->radius;

              // 距離チェック
              float dx = posA.x - posB.x;
              float dy = posA.y - posB.y;
              float dz = posA.z - posB.z;
              float distSq = dx * dx + dy * dy + dz * dz;
              float radSum = rA + rB;

              if (distSq < radSum * radSum && distSq > 0.0001f) {
                // 衝突！
                float dist = std::sqrt(distSq);
                float penetration = radSum - dist;

                // 反発ベクトル (正規化)
                float nx = dx / dist;
                float ny = dy / dist;
                float nz = dz / dist;

                // イベント発行 
                // 衝突の勢いを適当に計算
                float impact = std::abs(velA.vx) + std::abs(velA.vy);

                // **1-5 で正しいハンドルを入れた。** 1-4 までは dense 添字を
                // Entity として渡せなくなったため `Invalid()` を置いていた
                // (偽の identity より正直だという理由)。View が反復中の
                // `Entity` を返し、グリッドの添字は `BaseEntities()` で
                // `Entity` に戻せるようになったので、両方とも本物を渡せる。
                //
                // @note **この経路はまだ動いていない。** 上の Clear() の後に
                //       再挿入が無いので Query は何も返さない (ECS-0 1-1)。
                //       **1-7 で衝突検出を直せば、ここはそのまま動く。**
                ++found;
                if (publish) {
                  eventBus.Publish<Events::CollisionEvent>({
                    self,
                    other,
                    impact
                    });
                }

                float force = penetration * 0.5f; // 反発係数的なもの

                velA.vx += nx * force;
                velA.vy += ny * force;
                velA.vz += nz * force;
              }

              return true;
              });
          }
          if (found != 0u) { contacts.fetch_add(found, std::memory_order_relaxed); }
          });
      // ParallelForChunks は WaitFor の前に戻らないので、ここで全部の塊が足し終わっている
      result.contacts = contacts.load(std::memory_order_relaxed);
      return result;
    }
  };
}