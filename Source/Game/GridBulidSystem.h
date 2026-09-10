#pragma once
#include "../ECS/Components.h"
#include "../Core/GameContext.h"
#include "../Physics/SpatialHashGrid.h"
#include "../Core/HardwareConstants.h"
#include <thread>

namespace GLFD::Systems {
  class GridBuildSystem {
  public:
    static void Update(GameContext& ctx) {
      // **グリッドの基準は Position プール 1 本**である (1-5)。
      // グリッドに入れるのはこの View の dense 添字で、引くときも同じ View から
      // 引く。単一型なので飛ばされる要素が無く、添字がそのまま並びになる
      auto view = ctx.registry->View<Components::Position>();
      const size_t count = view.BaseSize();

      // 1. フレームメモリから Grid のメモリを確保！
      // Placement New を使用して構築
      void* buf = ctx.frameResource->Allocate(sizeof(Physics::SpatialHashGrid), alignof(Physics::SpatialHashGrid));

      // **確保できたか見る** (1-6)。以前は戻り値を見ずに placement new していた。
      // フレームメモリが尽きると nullptr へ構築して未定義動作になり、
      // その後 BoidSystem の `*context.grid` で null 参照していた
      if (buf == nullptr) {
        ctx.grid = nullptr;         // **前フレームのものを残さない**(下の @note)
        return;
      }

      // フレームリソースを使って Grid を構築
      // Grid内部の配列も frameResource から確保されるため、
      // フレーム終了時に全自動で消滅する。デストラクタ呼び出しすら不要。
      auto* grid = new(buf) Physics::SpatialHashGrid(ctx.frameResource, count);

      // 内部の配列も確保できたか (1-6)。コンストラクタは投げない契約になった
      if (!grid->IsReady()) {
        ctx.grid = nullptr;
        return;
      }

      // **組んだ時点の構造版を控える** (1-5 / R-24)。入っている dense 添字が
      // まだ有効かを、引く側が検査できるようにするため
      grid->SetBuildStamp(view.StructureVersion());

      // Contextにセットして、後続のシステムが使えるようにする
      //
      // @note **失敗したフレームで `ctx.grid` を前フレームのまま残さない。**
      //       残すと、構造版の照合 (1-5 / R-43) をすり抜けた古い dense 添字で
      //       近傍を引くことになる。`nullptr` は「無い」と正直に言える。
      //       依存するシステム (Boid / Collision) は実行順序の表で
      //       `Skipped` を返す
      ctx.grid = grid;

      if (count == 0) { return; }

      // @note **ここで組んだ内容は誰も読まない。** 直後に BoidSystem が
      //       `Clear()` して組み直す(ECS-0 の「1 フレームに 2 回組まれる」)。
      //       1-5 では消さない。消すと View 化の前後比較に「無駄を消した分」が
      //       混ざって、どちらの効果か読めなくなるためである。**整理は 1-7。**
      //
      // @note コンストラクタ直後のバケットは**番兵で埋まっていない**
      //       (`Resize` は 0 埋めで、番兵は 0xFFFFFFFF)。ここが `Clear()` を
      //       呼ばずに挿入しているのは ECS-0 1-4 の指摘そのものである。**是正は 1-7。**
      const Components::Position* const pData = view.BaseComponents();
      auto& jobSystem = *ctx.jobSystem;

      // 並列でインサート
      size_t threadCount = System::WorkerThreadCount();   // 0 を返し得るので丸める (ECS-0 3-7)
      size_t batchSize = count / threadCount;
      Thread::JobCounter counter;
      auto handle = jobSystem.CreateHandle(counter);

      for (size_t t = 0; t < threadCount; ++t) {
        size_t start = t * batchSize;
        size_t end = (t == threadCount - 1) ? count : start + batchSize;

        jobSystem.KickJob([start, end, pData, grid]() {
            // **入れているのは基準プールの dense 添字**である (1-5 論点3)。
            // 引く側も同じ View の dense 配列で引くので対応が取れている
            for (size_t i = start; i < end; ++i) {
              grid->Insert(static_cast<uint32_t>(i), pData[i]);
            }
          }, &handle);
      }
      jobSystem.WaitFor(handle);
    }
  };
}