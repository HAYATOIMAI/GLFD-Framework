/**
 * @file  probe_ecs_core.cpp
 * @brief `Registry` / `CommandBuffer` / `View` on the per-frame path.
 *
 * @note  **The templates are instantiated on purpose.** A probe that only
 *        includes the header proves nothing about a template: it is not type
 *        checked, let alone code generated, until something uses it (E-1).
 */
// EXPECT: CLEAN
// WHY: R-10 / R-25 return Invalid() or nullptr, N-2 says the per-frame path never throws.

#include "Core/GameContext.h"
#include "ECS/CommandBuffer.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/View.h"

void RunEcs(GLFD::ECS::Registry& reg, GLFD::ECS::CommandBuffer& cmd) {
  const GLFD::ECS::Entity e = reg.CreateEntity();
  (void)reg.AddComponent<GLFD::Components::Position>(e, 1.0f, 2.0f, 0.0f, 0.0f);
  (void)reg.AddComponent<GLFD::Components::Velocity>(e, 1.0f, 2.0f, 0.0f, 0.0f);
  (void)reg.GetComponent<GLFD::Components::Position>(e);
  (void)reg.HasComponent<GLFD::Components::Velocity>(e);
  reg.RemoveComponent<GLFD::Components::Velocity>(e);

  reg.ApplyCommands(cmd);

  for (auto [entity, pos, vel] :
       reg.View<GLFD::Components::Position, GLFD::Components::Velocity>()) {
    (void)entity; (void)pos; (void)vel;
  }
  reg.DestroyEntity(e);
}
