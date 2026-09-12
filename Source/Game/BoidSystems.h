#pragma once

#include "../ECS/Components.h"
#include "../ECS/Registry.h"
#include "../ECS/View.h"
#include "../Threading/JobSystem.h"
#include "../Physics/SpatialHashGrid.h"
#include "BoidAgent.h"
#include "../Core/GameContext.h"
#include "../Core/HardwareConstants.h"
#include <cassert>
#include <cmath>
#include <tuple>

namespace GLFD::Systems {
  class BoidSystem {
  public:
    /// @param maxSpeed 設定由来の速度上限 (simulation.maxSpeed)
    static void Update(GameContext& context, float maxSpeed) {

      auto& grid = *context.grid;

      // **グリッドの基準は Position 単独の View** (1-5 論点3)。
      // グリッドに入るのはこの View の dense 添字で、引くときも同じ View から引く
      auto gridView = context.registry->View<Components::Position>();
      const size_t gridCount = gridView.BaseSize();
      const Components::Position* const gridPositions = gridView.BaseComponents();
      const ECS::Entity* const gridOwners = gridView.BaseEntities();

      // 自分側は 3 成分の**組**を回す (R-32)。以前は 3 本の dense 配列を
      // 同じ添字で触っていた
      auto view = context.registry->View<Components::Position,
                                         Components::Velocity,
                                         Components::BoidAgent>();
      const size_t count = view.BaseSize();
      if (count == 0 || gridCount == 0 || gridPositions == nullptr) return;
      
      size_t threadCount = System::WorkerThreadCount();   // 0 を返し得るので丸める (ECS-0 3-7)
      size_t batchSize = count / threadCount;
      Thread::JobCounter counter;
      auto handle = context.jobSystem->CreateHandle(counter);

      // **1-7: ここでは組まない。読むだけである。**
      //
      // 以前はこの位置で `Clear()` してから組み直していた。直前に
      // `GridBuildSystem` が組んだ内容を捨てて同じものを作り直す形で、
      // ECS-0 の「1 フレームに 2 回組まれる」の実体だった。
      // 実測では `Clear()` が 283μs、挿入が 103μs。**どちらも無駄だった。**
      //
      // 構築点は `GridBuildSystem` 1 箇所(実行順序の表の先頭)。
      // `SetBuildStamp` もあちらが行う。

      // **並列ループへ入る前に 1 回だけ突き合わせる。** グリッドに入っている
      // dense 添字が、まだ同じものを指しているか。1-4 で構造変更をフレーム境界へ
      // 集めたので成り立つはずだが、**成り立つはずを検査にする**
      assert(grid.BuildStamp() == context.registry->StructureVersion()
             && "BoidSystem: the registry changed shape after the grid was built. "
                "The dense indices stored in the grid no longer mean what they meant "
                "(1-5). Structural changes belong in a CommandBuffer.");

      for (size_t t = 0; t < threadCount; ++t) {
        size_t start = t * batchSize;
        size_t end = (t == threadCount - 1) ? count : start + batchSize;

        context.jobSystem->KickJob([=, &grid]() {
          for (auto entry : view.Slice(start, end)) {
            // 構造化束縛を内側のラムダで捕まえない形にしておく
            const ECS::Entity            self  = std::get<0>(entry);
            Components::Position&        myPos = std::get<1>(entry);
            Components::Velocity&        myVel = std::get<2>(entry);
            const Components::BoidAgent& agent = std::get<3>(entry);

            // 集計用変数
            float sepX = 0, sepY = 0; // 分離
            float aliX = 0, aliY = 0; // 整列
            float cohX = 0, cohY = 0; // 結合 (重心)

            int neighborCount = 0;
            const int MAX_NEIGHBORS = 20; // 打ち切り数（これ以上見ると重すぎる）

            // 近傍探索
            grid.Query(myPos, [&](uint32_t neighborId) -> bool {
                if (neighborId >= gridCount) return true;      // 純粋な防御

                // **自己スキップは `Entity` で行う。** 反復側の添字は 3 成分の
                // View の基準プールのもので、**グリッドの添字(Position 単独の
                // View)とは別物**である。添字で比べると、自分を飛ばし損ねて
                // 無関係な他人を飛ばす。しかも群れが少し変わるだけで落ちない
                const ECS::Entity other = gridOwners[neighborId];
                if (other == self) return true;

                if (++neighborCount > MAX_NEIGHBORS) return false; // 打ち切り

                // 位置は**直接添字**で引ける(グリッドと同じ View なので)
                const auto& otherPos = gridPositions[neighborId];

                // 速度は `Entity` から引く。**ここだけは疎配列を通る**。
                // プールごとに swap-and-pop が独立に起きるので、
                // 「Position の dense 添字で Velocity を引く」は成立しない
                const Components::Velocity* const otherVelPtr =
                    view.Find<Components::Velocity>(other);
                if (otherVelPtr == nullptr) return true;   // 速度が無い相手は数えない
                const auto& otherVel = *otherVelPtr;

                float dx = myPos.x - otherPos.x;
                float dy = myPos.y - otherPos.y;
                float distSq = dx * dx + dy * dy;

                // 視界内かチェック
                if (distSq < agent.viewRadius * agent.viewRadius && distSq > 0.0001f) {
                  // 1. Separation: 距離の逆数で重みづけして離れる
                  // 近ければ近いほど強く離れる
                  float dist = std::sqrt(distSq);
                  float sepFactor = 1.0f / dist;
                  sepX += (dx / dist) * sepFactor;
                  sepY += (dy / dist) * sepFactor;

                  // 2. Alignment: 相手の速度を足す
                  aliX += otherVel.vx;
                  aliY += otherVel.vy;

                  // 3. Cohesion: 相手の位置を足す
                  cohX += otherPos.x;
                  cohY += otherPos.y;
                }
                return true;
              });

            if (neighborCount > 0) {
              // Alignment: 平均速度を求める
              aliX /= neighborCount;
              aliY /= neighborCount;

              // 自分の速度との差分を力とする
              aliX -= myVel.vx;
              aliY -= myVel.vy;

              // Cohesion: 重心を求める
              cohX /= neighborCount;
              cohY /= neighborCount;

              // 重心へのベクトル
              cohX -= myPos.x;
              cohY -= myPos.y;

              // 力の合成
              float forceX = (sepX * agent.separationWeight) +
                (aliX * agent.alignmentWeight) +
                (cohX * agent.cohesionWeight);
              float forceY = (sepY * agent.separationWeight) +
                (aliY * agent.alignmentWeight) +
                (cohY * agent.cohesionWeight);

              // 速度更新 (加速度 * dt)
              // 質量は1.0と仮定
              myVel.vx += forceX * context.dt;
              myVel.vy += forceY * context.dt;

              // 速度制限 (爆速にならないように)
              float speedSq = myVel.vx * myVel.vx + myVel.vy * myVel.vy;
              if (speedSq > maxSpeed * maxSpeed) {
                float speed = std::sqrt(speedSq);
                myVel.vx = (myVel.vx / speed) * maxSpeed;
                myVel.vy = (myVel.vy / speed) * maxSpeed;
              }
            }
          }
          }, & handle);
      }
      context.jobSystem->WaitFor(handle);
    }
  };
}