/**
 * @file  probe_grid_build.cpp
 * @brief `GridBuildSystem::Update` with an empty filter pack - the boid path.
 *
 * @details
 *  Kept apart from the filtered build on purpose. The two go through the same
 *  `if constexpr` in the same function template, and **only one of them is
 *  dirty**: the unfiltered branch captures a single-type `View`, which fits in
 *  `std::function`'s inline buffer, while the filtered branch captures a
 *  three-type one and does not.
 *
 *  That difference is invisible when reading the source, and it is why this row
 *  must not be merged into `probe_grid_build_filtered.cpp`: a CLEAN row here is
 *  a real statement about the boid path.
 */
// EXPECT: CLEAN
// WHY: the unfiltered grid build captures a small View and stays within N-2.

#include "Core/GameContext.h"
#include "ECS/Components.h"
#include "Game/GridBulidSystem.h"

void BuildAll(GLFD::GameContext& ctx) {
  GLFD::Systems::GridBuildSystem::Update(ctx);
}
