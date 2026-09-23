/**
 * @file  probe_survivor_loop.cpp
 * @brief The whole 1-8 loop, through the same entry points the scene uses.
 *
 * @note Was dirty until ECS 2-6, and not because of anything 1-8 wrote: the loop
 *       reaches `HitSystem::Update`, whose kick built a `std::function` big enough
 *       to allocate. Nothing in `SurvivorLoop.h` itself throws
 *       (`probe_survivor_loop_steps.cpp`).
 *  **ECS 2-6 3a:** the kick now goes through `Thread::ParallelForChunks`, whose job
 *  captures only `&body` and the range (24 bytes). That fits `std::function`'s
 *  inline buffer, so the conversion no longer allocates. The body itself is
 *  unchanged. `probe_root_job_dispatch.cpp` keeps the tooth (it still THROWs).
 */
// EXPECT: CLEAN
// WHY: 2-6 3a - every kick on this path goes through ParallelForChunks (&body + range). was THROW.

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
