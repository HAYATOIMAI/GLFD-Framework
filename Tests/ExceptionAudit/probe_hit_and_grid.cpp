/**
 * @file  probe_hit_and_grid.cpp
 * @brief Collision detection: `HitSystem` (1-8) and `CollisionSystem` (1-7).
 *
 * @note Was dirty until ECS 2-6 because both kicked jobs with a large capture
 *       (`HitSystem::Update` took `[=, &grid, &bus]`, two Views and more).
 *       Nothing in the overlap test itself throws.
 *  **ECS 2-6 3a:** the kick now goes through `Thread::ParallelForChunks`, whose job
 *  captures only `&body` and the range (24 bytes). That fits `std::function`'s
 *  inline buffer, so the conversion no longer allocates. The body itself is
 *  unchanged. `probe_root_job_dispatch.cpp` keeps the tooth (it still THROWs).
 */
// EXPECT: CLEAN
// WHY: 2-6 3a - HitSystem / CollisionSystem kick through ParallelForChunks (&body + range). was THROW.

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
