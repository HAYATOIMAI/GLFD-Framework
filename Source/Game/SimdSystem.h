#pragma once
#include "../ECS/Components.h"
#include "../Core/GameContext.h"
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
      // データへの生ポインタを取得 (Viewを使わず直接Dense配列を触るのが最速)
      // ※今回は「全員がPos/Velを持っている」前提のデモです
      auto& positions = context.registry->View<Components::Position>();
      auto& velocities = context.registry->View<Components::Velocity>();

      size_t count = positions.GetSize();
      Components::Position* pData = positions.GetData();
      const Components::Velocity* vData = velocities.GetData();

      // dt (デルタタイム) をSIMDレジスタの全レーンにセット: [dt, dt, dt, dt]
      __m128 dtVec = _mm_set1_ps(context.dt);

      // 並列処理の設定
      size_t threadCount = std::thread::hardware_concurrency();
      size_t batchSize = count / threadCount;

      // ジョブハンドルの作成
      Thread::JobCounter counter;
      auto handle = context.jobSystem->CreateHandle(counter);

      for (size_t t = 0; t < threadCount; ++t) {
        size_t start = t * batchSize;
        size_t end = (t == threadCount - 1) ? count : start + batchSize;

        context.jobSystem->KickJob([=]() {
            // ループアンローリングなどの最適化はコンパイラに任せるか、
            // さらに手動で 4要素ずつ処理することも可能だが、
            // ここでは「1エンティティ = 1SIMD演算」として記述する。
            // 構造体が16バイト(float*4)なので、配列アクセスも綺麗にアラインされる。

            for (size_t i = start; i < end; ++i) {
              // 1. メモリからレジスタへロード (Aligned Load)
              // pData[i] は alignas(16) なので _mm_load_ps が使える（最速）
              // もしアライメントが保証されない場合は _mm_loadu_ps を使う必要がある
              __m128 p = _mm_load_ps(reinterpret_cast<const float*>(&pData[i]));
              __m128 v = _mm_load_ps(reinterpret_cast<const float*>(&vData[i]));

              // 2. 計算: P = P + V * dt
              // _mm_mul_ps: V * dt (4要素同時掛け算)
              // _mm_add_ps: P + result (4要素同時足し算)
              __m128 result = _mm_add_ps(p, _mm_mul_ps(v, dtVec));

              // 3. レジスタからメモリへストア (Aligned Store)
              _mm_store_ps(reinterpret_cast<float*>(&pData[i]), result);
            }
          }, &handle);
      }
      // 完了待ち
      context.jobSystem->WaitFor(handle);
    }
  };
}