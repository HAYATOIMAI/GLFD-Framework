#pragma once
#include "../ECS/Components.h"
#include "../ECS/View.h"
#include "../Core/GameContext.h"
#include "../Core/HardwareConstants.h"
#include <immintrin.h>

namespace GLFD::Systems {
  class MovementSystem {
  public:
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

      // 並列処理の設定
      size_t threadCount = System::WorkerThreadCount();   // 0 を返し得るので丸める (ECS-0 3-7)
      size_t batchSize = count / threadCount;

      // ジョブハンドルの作成
      Thread::JobCounter counter;
      auto handle = context.jobSystem->CreateHandle(counter);

      for (size_t t = 0; t < threadCount; ++t) {
        size_t start = t * batchSize;
        size_t end = (t == threadCount - 1) ? count : start + batchSize;

        // **View は値でコピーして投げる**(プールへのポインタしか持たない)
        context.jobSystem->KickJob([view, start, end, dt]() {
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
          }, &handle);
      }
      // 完了待ち
      context.jobSystem->WaitFor(handle);
    }
  };
}