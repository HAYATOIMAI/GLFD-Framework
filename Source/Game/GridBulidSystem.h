#pragma once
#include "../ECS/Components.h"
#include "../Core/GameContext.h"
#include "../Physics/SpatialHashGrid.h"
#include <thread>

namespace GLFD::Systems {
  class GridBuildSystem {
  public:
    static void Update(GameContext& ctx) {
      // 1. フレームメモリから Grid のメモリを確保！
      // Placement New を使用して構築
      void* buf = ctx.frameResource->Allocate(sizeof(Physics::SpatialHashGrid), alignof(Physics::SpatialHashGrid));

      // エンティティ数 (Registryから取得してもいいし、Configから取ってもいい)
      size_t entityCount = ctx.registry->View<Components::Position>().GetSize();

      // フレームリソースを使って Grid を構築
      // Grid内部の配列も frameResource から確保されるため、
      // フレーム終了時に全自動で消滅する。デストラクタ呼び出しすら不要。
      auto* grid = new(buf) Physics::SpatialHashGrid(ctx.frameResource, entityCount);

      // Contextにセットして、後続のシステムが使えるようにする
      ctx.grid = grid;

      auto& positions = ctx.registry->View<Components::Position>();
      size_t count = positions.GetSize();
      if (count == 0) return;

      auto* pData = positions.GetData();
      auto& jobSystem = *ctx.jobSystem;

      // 並列でインサート
      size_t threadCount = std::thread::hardware_concurrency();
      size_t batchSize = count / threadCount;
      Thread::JobCounter counter;
      auto handle = jobSystem.CreateHandle(counter);

      for (size_t t = 0; t < threadCount; ++t) {
        size_t start = t * batchSize;
        size_t end = (t == threadCount - 1) ? count : start + batchSize;

        jobSystem.KickJob([start, end, pData, &grid]() {
            // Viewの並び順(Dense Index)とEntityIDが一致している前提の高速化
            // ※エンティティ削除が入るとズレるので、その場合は修正が必要
            for (size_t i = start; i < end; ++i) {
              grid->Insert(static_cast<uint32_t>(i), pData[i]);
            }
          }, &handle);
      }
      jobSystem.WaitFor(handle);
    }
  };
}