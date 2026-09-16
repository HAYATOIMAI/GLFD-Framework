/**
 * @file  probe_grid_build_filtered.cpp
 * @brief `GridBuildSystem::Update<Filter...>` - the survivor path (1-8).
 *
 * @warning Dirty, and **not** for a reason you can see in `UpdateFiltered`.
 *          Its kick captures `[start, end, owners, grid, view]`, and `view` here
 *          is `View<Position, Health, Collider>`. That pushes the lambda past
 *          `std::function`'s inline buffer, so the conversion allocates.
 *          `probe_grid_build.cpp`, which differs only in the filter pack, is
 *          CLEAN. See `probe_root_job_dispatch.cpp`.
 */
// EXPECT: THROW
// WHY: the filtered kick captures a three-type View. See probe_root_job_dispatch.

#include "Core/GameContext.h"
#include "ECS/Components.h"
#include "Game/GridBulidSystem.h"
#include "Game/SurvivorComponents.h"
#include "Physics/CollisionComponents.h"

void BuildFiltered(GLFD::GameContext& ctx) {
  GLFD::Systems::GridBuildSystem::Update<GLFD::Components::Health,
                                         GLFD::Components::Collider>(ctx);
}
