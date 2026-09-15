#pragma once

/**
 * @file  SurvivorComponents.h
 * @brief ECS 1-8: VS 型の最小ループ(敵 / 弾 / 経験値)で使う成分
 *
 * @details
 *  ## 組み合わせ
 *  | エンティティ | 成分 |
 *  |---|---|
 *  | 敵     | Position / Velocity / Health / Collider |
 *  | 弾     | Position / Velocity / Damage / Lifetime / Collider |
 *  | 経験値 | Position / Collider / Pickup / **Lifetime** |
 *
 *  **経験値に `Lifetime` を足したのは表からの唯一の逸脱である。** プレイヤーが
 *  動かないので、回収点に届かない経験値は永久に残り、いずれ上限に張り付く。
 *  型は増やしていない(組み合わせが違うだけ)。
 *
 *  ## 「死んだ」「使い切った」を値で表す
 *  専用のフラグを持たない。**破棄を積むのは、`Health::current` または
 *  `Lifetime::remaining` が正から 0 以下へ変わったときに限る。** 積んだあと
 *  適用までの間(同じフレームの残り)は、0 以下であることを見て各システムが
 *  素通りする。これで**同じエンティティへ `Destroy` が 2 回積まれることが
 *  構造的に起きない**(1-8 論点3 の c)。
 *
 *  ## 制約 (R-17)
 *  `CommandBuffer::Add` に渡すので trivially copyable でなければならない。
 *  **検査を `Add` の呼び出し側だけに置かず、型の定義に置く。** 呼ばれるまで
 *  壊れていることが分からない形にしない。
 *
 *  `alignas(16)` は付けない。`Position` / `Velocity` と違い SIMD でロードしない。
 *  `CommandBuffer` が要求するのは「アラインが 16 以下」だけである。
 */

#include <type_traits>

namespace GLFD::Components {

  /// 敵の体力。**0 以下になった瞬間に破棄を積む**
  struct Health {
    float current;
  };

  /// 弾が与える量
  struct Damage {
    float amount;
  };

  /// 残り秒数。**正から 0 以下へ変わった瞬間に破棄を積む**。命中した弾は 0 にされる
  struct Lifetime {
    float remaining;
  };

  /// 回収したときに得る経験値
  struct Pickup {
    float value;
  };

  namespace Detail {
    template <class T>
    inline constexpr bool kQueueable = std::is_trivially_copyable_v<T>
                                    && sizeof(T) <= 16u
                                    && alignof(T) <= 16u;
  }

  static_assert(Detail::kQueueable<Health>,
                "Health must stay trivially copyable and fit one 16-byte payload slot: "
                "the loop queues it through CommandBuffer::Add (R-17)");
  static_assert(Detail::kQueueable<Damage>,   "Damage: see the Health assertion (R-17)");
  static_assert(Detail::kQueueable<Lifetime>, "Lifetime: see the Health assertion (R-17)");
  static_assert(Detail::kQueueable<Pickup>,   "Pickup: see the Health assertion (R-17)");

}
