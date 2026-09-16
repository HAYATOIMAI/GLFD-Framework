/**
 * @file  probe_survivor_loop.cpp
 * @brief The whole 1-8 loop, through the same entry points the scene uses.
 *
 * @warning **Dirty, and not because of anything 1-8 wrote.** The loop reaches
 *          `HitSystem::Update`, which kicks jobs, and the kick builds a
 *          `std::function` big enough to allocate. See
 *          `probe_root_job_dispatch.cpp`. Nothing in `SurvivorLoop.h` itself
 *          throws: `probe_survivor_loop_steps.cpp` covers that part and is CLEAN.
 */
// EXPECT: THROW
// WHY: reaches HitSystem -> JobSystem::KickJob. See probe_root_job_dispatch.

#include "Core/GameContext.h"
#include "Game/SurvivorLoop.h"

void RunLoop(GLFD::Game::SurvivorState& s, GLFD::GameContext& ctx,
             GLFD::Core::FrameReport& report, GLFD::Core::FailureGate& gate) {
  GLFD::Game::AttachSurvivor(s, *ctx.registry, *ctx.commands, *ctx.eventBus);
  (void)GLFD::Game::SpawnEnemy(s, 1.0f, 2.0f, 0.0f, 0.0f);
  (void)GLFD::Game::FireBullet(s, 1.0f, 2.0f, 0.0f, 0.0f);
  GLFD::Game::BeginSurvivorFrame(s);
  GLFD::Game::RunSurvivorFrame(s, ctx, report);
  GLFD::Game::EndSurvivorFrame(s);
  (void)GLFD::Game::ObserveCreationFailures(s.thisFrame, gate);
}
