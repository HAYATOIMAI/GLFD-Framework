/**
 * @file  probe_grid_build_filtered.cpp
 * @brief `GridBuildSystem::Update<Filter...>` - the survivor path (1-8).
 *
 * @note Was dirty until ECS 2-6 for a reason you could not see in `UpdateFiltered`:
 *       its kick captured `[start, end, owners, grid, view]` with a three-type View,
 *       which pushed the lambda past `std::function`'s inline buffer.
 *  **ECS 2-6 3a:** the kick now goes through `Thread::ParallelForChunks`, whose job
 *  captures only `&body` and the range (24 bytes). That fits `std::function`'s
 *  inline buffer, so the conversion no longer allocates. The body itself is
 *  unchanged. `probe_root_job_dispatch.cpp` keeps the tooth (it still THROWs).
 */
// EXPECT: CLEAN
// WHY: 2-6 3a - the kick captures &body + range (24 B) via ParallelForChunks. was THROW (three-type View capture).

#include "Core/GameContext.h"
#include "ECS/Components.h"
#include "Game/GridBulidSystem.h"
#include "Game/SurvivorComponents.h"
#include "Physics/CollisionComponents.h"

void BuildFiltered(GLFD::GameContext& ctx) {
  GLFD::Systems::GridBuildSystem::Update<GLFD::Components::Health,
                                         GLFD::Components::Collider>(ctx);
}
