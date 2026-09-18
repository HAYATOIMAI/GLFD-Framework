/**
 * @file  probe_registry_destroy_all.cpp
 * @brief `Registry::DestroyAll` - what a scene transition runs (ECS 2-1).
 *
 * @details
 *  It allocates: the mark array that separates live indices from free ones has
 *  to come from somewhere. That makes it worth a row of its own, because the
 *  allocation is on a `Try*` path and must stay that way. A transition is not a
 *  per-frame event, but it happens while the player is holding a key down, and
 *  a throw there is the same crash as a throw in a frame.
 */
// EXPECT: CLEAN
// WHY: DestroyAll allocates its mark array and must report failure, not throw.

#include "Core/GameContext.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"

void Clear(GLFD::ECS::Registry& registry) {
  (void)registry.DestroyAll();
  (void)registry.AliveCount();
}
