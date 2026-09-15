#pragma once

#include "../ECS/Entity.h"

namespace GLFD::Events {
  using Entity = GLFD::ECS::Entity;

  // 衝突イベント
  struct CollisionEvent {
    Entity entityA;
    Entity entityB;
    float velocityImpact; // 衝突の勢い（音量などに使用）
  };

  // デバッグ用ログイベント
  struct LogEvent {
    int code;
    // 文字列などはStackAllocatorとの相性が悪いため、
    // 固定長配列にするか、IDを渡すのが定石
    char message[64];
  };

  /**
   * @brief 命中イベント (ECS 1-8)。弾が敵に重なった
   * @note  並列の `HitSystem` から発行され、`DispatchEvents` でメインスレッドへ届く。
   *        1 発が複数の敵に重なれば複数件出る。どれを倒すかは購読者が値で決める
   */
  struct HitEvent {
    Entity bullet;
    Entity enemy;
  };
}
