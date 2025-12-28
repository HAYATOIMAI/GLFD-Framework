#pragma once

#include "Threading/JobSystem.h"
#include "ECS/Registry.h"
#include "ECS/Components.h"
#include "CollisionComponents.h"
#include "SpatialHashGrid.h"
#include "Events/EventBus.h"
#include "Events/Events.h"
#include "../Core/Profiler.h"
#include "../Core/GameContext.h"
#include <immintrin.h>

namespace GLFD::Systems {
  class CollisionSystem {
  public:
    // メイン処理
    static void Update(GameContext& context) {

      auto& registry = *context.registry;
      auto& eventBus = *context.eventBus;
      auto& grid = *context.grid;

      auto& positions = context.registry->View<Components::Position>();
      auto& velocities = context.registry->View<Components::Velocity>();
      auto& colliders = context.registry->View<Components::Collider>();

      size_t count = positions.GetSize();
      if (count == 0) return;

      // 生ポインタ取得
      auto* pData = positions.GetData();
      auto* vData = velocities.GetData();
      auto* cData = colliders.GetData();

      // 全エンティティをグリッドに登録する

      context.grid->Clear(); // バケット初期化

      // バッチ処理設定
      size_t threadCount = std::thread::hardware_concurrency();
      size_t batchSize = count / threadCount;
      Thread::JobCounter buildCounter;
      auto buildHandle = context.jobSystem->CreateHandle(buildCounter);

      // グリッドを使って近傍を検索し、衝突応答を行う

      Thread::JobCounter solveCounter;
      auto solveHandle = context.jobSystem->CreateHandle(solveCounter);

      for (size_t t = 0; t < threadCount; ++t) {
        size_t start = t * batchSize;
        size_t end = (t == threadCount - 1) ? count : start + batchSize;

        context.jobSystem->KickJob([start, end, pData, vData, cData, &grid, &eventBus]() {
          for (size_t i = start; i < end; ++i) {
            Components::Position& posA = pData[i];
            Components::Velocity& velA = vData[i];
            float rA = cData[i].radius;

            int checkCount = 0;
            const int MAX_CHECKS = 16;

            // 近傍探索
            grid.Query(posA, [&](uint32_t neighborId) -> bool {
              if (i == neighborId) return true; // 自分自身は無視して継続

              // 上限チェック
              if (++checkCount > MAX_CHECKS) {
                return false;
              }

              Components::Position& posB = pData[neighborId];
              float rB = cData[neighborId].radius;

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

                eventBus.Publish<Events::CollisionEvent>({
                  static_cast<ECS::Entity>(i),
                  static_cast<ECS::Entity>(neighborId),
                  impact
                  });

                float force = penetration * 0.5f; // 反発係数的なもの

                velA.vx += nx * force;
                velA.vy += ny * force;
                velA.vz += nz * force;
              }

              return true;
              });
          }
          }, &solveHandle);
      }
      context.jobSystem->WaitFor(solveHandle);
    }
  };
}