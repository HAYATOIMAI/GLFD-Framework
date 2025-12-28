#pragma once

#include "../ECS/Components.h"
#include "../ECS/Registry.h"
#include "../Threading/JobSystem.h"
#include "../Core/Profiler.h"
#include "../Physics/SpatialHashGrid.h"
#include "BoidAgent.h"
#include "../Core/GameContext.h"
#include <cmath>

namespace GLFD::Systems {
  class BoidSystem {
  public:
    static void Update(GameContext& context) {

      auto& registry = *context.registry;
      auto& eventBus = *context.eventBus;
      auto& grid = *context.grid;

      auto& positions = context.registry->View<Components::Position>();
      auto& velocities = context.registry->View<Components::Velocity>();
      auto& boidAgents = context.registry->View<Components::BoidAgent>();

      size_t count = positions.GetSize();
      if (count == 0) return;

      auto* pData = positions.GetData();
      auto* vData = velocities.GetData();
      auto* aData = boidAgents.GetData();
      
      size_t threadCount = std::thread::hardware_concurrency();
      size_t batchSize = count / threadCount;
      Thread::JobCounter counter;
      auto handle = context.jobSystem->CreateHandle(counter);

      grid.Clear();

      Thread::JobCounter buildCounter;
      Thread::JobHandle buildHandle = context.jobSystem->CreateHandle(buildCounter);

      for (size_t t = 0; t < threadCount; ++t) {
        size_t start = t * batchSize;
        size_t end = (t == threadCount - 1) ? count : start + batchSize;

        context.jobSystem->KickJob([start, end, pData, &grid]() {
          for (size_t i = start; i < end; ++i) {
            grid.Insert(static_cast<uint32_t>(i), pData[i]);
          }
          }, &buildHandle);
      }

      context.jobSystem->WaitFor(buildHandle);


      for (size_t i = 0; i < threadCount; ++i) {
        size_t start = i * batchSize;
        size_t end = (i == threadCount - 1) ? count : start + batchSize;

        context.jobSystem->KickJob([=, &grid]() {
          for (size_t i = start; i < end; ++i) {
            auto& myPos = pData[i];
            auto& myVel = vData[i];
            const auto& agent = aData[i];

            // 集計用変数
            float sepX = 0, sepY = 0; // 分離
            float aliX = 0, aliY = 0; // 整列
            float cohX = 0, cohY = 0; // 結合 (重心)

            int neighborCount = 0;
            const int MAX_NEIGHBORS = 20; // 打ち切り数（これ以上見ると重すぎる）

            // 近傍探索
            grid.Query(myPos, [&](uint32_t neighborId) -> bool {
                if (i == neighborId) return true;

                if (++neighborCount > MAX_NEIGHBORS) return false; // 打ち切り

                const auto& otherPos = pData[neighborId];
                const auto& otherVel = vData[neighborId];

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
              float maxSpeed = 2.0f;
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