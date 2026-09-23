#pragma once
#include "../ECS/Components.h"
#include "../ECS/View.h"
#include "../Core/GameContext.h"
#include "../Physics/SpatialHashGrid.h"
#include "../Core/HardwareConstants.h"
#include "../Threading/ParallelFor.h"
#include <thread>

namespace GLFD::Systems {
  class GridBuildSystem {
  public:
    /// 1 体あたりの費用 [us]。UpdateAll(Boid の 20000 体)をメインだけで回して測った値
    /// (ECS 2-6 手順2、i7-12700KF、Release)。粒度 = 1 本あたりの目標の仕事 / これ
    /// (Threading/ParallelFor.h の GrainFor)。**機械が変わったら測り直す値**
    static constexpr double kCostAllUs = 0.0064;
    static constexpr size_t kGrainAll = Thread::GrainFor(kCostAllUs);
    /// 1 体あたりの費用 [us]。UpdateFiltered(Survivor small / large の 2 点の傾き。Find を 2 回引く分だけ重い)をメインだけで回して測った値
    /// (ECS 2-6 手順2、i7-12700KF、Release)。粒度 = 1 本あたりの目標の仕事 / これ
    /// (Threading/ParallelFor.h の GrainFor)。**機械が変わったら測り直す値**
    static constexpr double kCostFilteredUs = 0.0075;
    static constexpr size_t kGrainFiltered = Thread::GrainFor(kCostFilteredUs);

    /**
     * @brief グリッドを組む
     *
     * @tparam Filter 空なら `Position` を持つ全員(Boid と 1-7 までの形)。
     *                指定すると `View<Position, Filter...>` の組だけで組む (1-8)
     *
     * @note **型引数なしの呼び出し `Update(ctx)` はそのまま通る**(引数から推論されない
     *       末尾のパックは空になる)。Boid / ベンチ / 1-7 のテストは書き換えていない
     */
    template <class... Filter>
    static void Update(GameContext& ctx) {
      if constexpr (sizeof...(Filter) == 0) {
        UpdateAll(ctx);
      }
      else {
        UpdateFiltered<Filter...>(ctx);
      }
    }

    /// `Position` を持つ全員で組む。**1-7 までの `Update` の本体をそのまま移した**
    static void UpdateAll(GameContext& ctx) {
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

      // 並列でインサート。分け方は ParallelFor.h の 1 か所 (ECS 2-6)
      Thread::ParallelForChunks(jobSystem, count, kGrainAll, [&](size_t start, size_t end) {
            // **入れているのは基準プールの dense 添字**である (1-5 論点3)。
            // 引く側も同じ View の dense 配列で引くので対応が取れている
            for (size_t i = start; i < end; ++i) {
              grid->Insert(static_cast<uint32_t>(i), pData[i]);
            }
          });
    }

    /**
     * @brief 組み合わせで絞ったグリッドを組む (1-8)
     *
     * @details
     *  **グリッドに入る番号は `View<Position, Filter...>` の基準プールの dense 添字**
     *  である。基準は最小のプールなので、`Position` の dense 配列を直接は使えない
     *  (`BaseComponents` は単一型の View にしか無い)。位置は持ち主の `Entity` から
     *  引く。
     *
     *  @warning **引く側は同じ型の並びの View で持ち主を引くこと。** 並びが違うと
     *           基準プールが変わり得る(1-5 の「2 つの View は基準の添字が違う」)。
     *           `HitSystem::BuildGrid` が組み方と引き方の対を 1 箇所に置いている
     */
    template <class... Filter>
    static void UpdateFiltered(GameContext& ctx) {
      auto view = ctx.registry->View<Components::Position, Filter...>();
      const size_t count = view.BaseSize();

      void* buf = ctx.frameResource->Allocate(sizeof(Physics::SpatialHashGrid), alignof(Physics::SpatialHashGrid));
      if (buf == nullptr) {
        ctx.grid = nullptr;         // 前フレームのものを残さない (R-47)
        return;
      }

      auto* grid = new(buf) Physics::SpatialHashGrid(ctx.frameResource, count);
      if (!grid->IsReady()) {
        ctx.grid = nullptr;
        return;
      }

      grid->SetBuildStamp(view.StructureVersion());
      ctx.grid = grid;

      if (count == 0) { return; }

      const ECS::Entity* const owners = view.BaseEntities();
      auto& jobSystem = *ctx.jobSystem;

      // 分け方は ParallelFor.h の 1 か所 (ECS 2-6)
      Thread::ParallelForChunks(jobSystem, count, kGrainFiltered, [&](size_t start, size_t end) {
            for (size_t i = start; i < end; ++i) {
              const ECS::Entity e = owners[i];
              // 基準プールにいても組が揃っているとは限らない (R-22)
              const Components::Position* const pos =
                  view.template Find<Components::Position>(e);
              if (pos == nullptr) { continue; }
              if (((view.template Find<Filter>(e) == nullptr) || ...)) { continue; }
              grid->Insert(static_cast<uint32_t>(i), *pos);
            }
          });
    }
  };
}