/**
 * @file  probe_survivor_loop_steps.cpp
 * @brief The 1-8 loop's **main-thread** steps: the part 1-8 actually wrote.
 *
 * @details
 *  Splitting this out is the point. If the whole loop were one dirty row, a new
 *  violation written into `SurvivorLoop.h` tomorrow would land on an
 *  already-red line and nobody would see it. Here every step that runs on the
 *  main thread is compiled, and the row must stay CLEAN.
 *
 *  Deliberately absent: `Movement`, `GridBuild` and `Hit`. All three dispatch to
 *  workers, and all three are dirty for the one reason in
 *  `probe_root_job_dispatch.cpp` - not for anything in the step itself.
 *  `probe_survivor_loop.cpp` covers the whole table including those.
 */
// EXPECT: CLEAN
// WHY: the main-thread steps of the 1-8 loop must not throw (N-2).

#include "Core/GameContext.h"
#include "Game/SurvivorLoop.h"

void RunSteps(GLFD::Game::SurvivorState& s, GLFD::GameContext& ctx) {
  (void)GLFD::Game::SpawnEnemiesStep(s, ctx);
  (void)GLFD::Game::FireBulletsStep(s, ctx);
  (void)GLFD::Game::DispatchEventsStep(s, ctx);
  (void)GLFD::Game::LifetimeStep(s, ctx);
  (void)GLFD::Game::CollectStep(s, ctx);
  (void)GLFD::Game::ReachStep(s, ctx);
  (void)GLFD::Game::ApplyCommandsStep(s, ctx);
}
