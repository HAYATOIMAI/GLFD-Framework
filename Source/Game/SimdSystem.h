#pragma once
#include "../ECS/Components.h"
#include "../ECS/View.h"
#include "../Core/GameContext.h"
#include "../Core/HardwareConstants.h"
#include "../Threading/ParallelFor.h"
#include <immintrin.h>

namespace GLFD::Systems {
  class MovementSystem {
  public:
    /// 1 体あたりの費用 [us]。Movement(Survivor small / large の 2 点の傾き。Boid の 20000 体でも同じ値)をメインだけで回して測った値
    /// (ECS 2-6 手順2、i7-12700KF、Release)。粒度 = 1 本あたりの目標の仕事 / これ
    /// (Threading/ParallelFor.h の GrainFor)。**機械が変わったら測り直す値**
    static constexpr double kCostPerEntityUs = 0.0023;
    static constexpr size_t kGrain = Thread::GrainFor(kCostPerEntityUs);

    /**
     * @brief 全エンティティの位置をSIMDで高速更新
     * @param registry ECSレジストリ
     * @param jobSystem ジョブシステム
     * @param dt デルタタイム
     */
    static void Update(GameContext& context) {
      // **添字が揃っている前提を捨てた** (1-5 / R-32)。以前は Position と
      // Velocity の dense 配列を同じ i で触っていたが、プールごとに
      // swap-and-pop が独立に起きるので**サイズが同じでも並びは一致しない**。
      // View が組を保証する
      auto view = context.registry->View<Components::Position, Components::Velocity>();
      const size_t count = view.BaseSize();
      if (count == 0) { return; }

      // dt (デルタタイム) をSIMDレジスタの全レーンにセット: [dt, dt, dt, dt]
      const float dt = context.dt;

      // 分け方は ParallelFor.h の 1 か所 (ECS 2-6)。塊ごとに body(start, end) が呼ばれる
      Thread::ParallelForChunks(*context.jobSystem, count, kGrain, [&](size_t start, size_t end) {
            const __m128 dtVec = _mm_set1_ps(dt);

            // **SIMD は残っている。** 各成分が個別に alignas(16) なので、
            // 参照からそのまま _mm_load_ps できる。失ったのは
            // 「2 本の配列を同じ添字で舐める」形だけで、命令列は変わらない
            for (auto [entity, pos, vel] : view.Slice(start, end)) {
              (void)entity;
              __m128 p = _mm_load_ps(reinterpret_cast<const float*>(&pos));
              __m128 v = _mm_load_ps(reinterpret_cast<const float*>(&vel));

              // 計算: P = P + V * dt
              __m128 result = _mm_add_ps(p, _mm_mul_ps(v, dtVec));

              _mm_store_ps(reinterpret_cast<float*>(&pos), result);
            }
          });
    }
  };
}