/**
 * @file  probe_hit_and_grid.cpp
 * @brief Collision detection: `HitSystem` (1-8) and `CollisionSystem` (1-7).
 *
 * @warning Dirty because both kick jobs with a large capture -
 *          `HitSystem::Update` takes `[=, &grid, &bus]`, which copies two Views,
 *          a pointer, a count and two indices. See `probe_root_job_dispatch.cpp`.
 *          Nothing in the overlap test itself throws.
 */
// EXPECT: THROW
// WHY: HitSystem / CollisionSystem kick jobs. See probe_root_job_dispatch.

#include "Core/GameContext.h"
#include "ECS/Components.h"
#include "Game/HitSystem.h"
#include "Game/SurvivorComponents.h"
#include "Physics/CollisionComponents.h"
#include "Physics/CollisionSystem.h"

void RunHit(GLFD::GameContext& ctx) {
  GLFD::Systems::HitSystem::BuildGrid(ctx);
  (void)GLFD::Systems::HitSystem::GridMatches(ctx);
  GLFD::Systems::HitSystem::Update(ctx);
  GLFD::Systems::CollisionSystem::Update(ctx);
}
