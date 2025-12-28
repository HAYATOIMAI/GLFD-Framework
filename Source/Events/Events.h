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
}
