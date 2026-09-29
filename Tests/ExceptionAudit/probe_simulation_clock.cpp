/**
 * @file  probe_simulation_clock.cpp
 * @brief The fixed step and the input edge carry-over added in ECS 2-8.
 *
 * @details Per-frame path: GameEngine::Run calls RunFixedSteps and KeyEdgeLatch every frame.
 *          N-2 says it must not throw. Engine.cpp as a whole is C4530+THROW (translation_units.txt),
 *          so this probe is the only row that sees the step counting by itself. The log lines
 *          (SimulationClockLog.h) go through LOG_* and are left out on purpose (probe_root_logger).
 */
// EXPECT: CLEAN
// WHY: 2-8: the per-frame step counting and input carry-over. integers and fixed arrays only.

#include <cstdint>

#include "Core/KeyEdgeLatch.h"
#include "Core/SimulationClock.h"

std::uint32_t Frame(GLFD::Core::FixedStepAccumulator& clock, GLFD::Core::KeyEdgeLatch& latch,
                    const GLFD::Core::KeyEdgeLatch::KeyStates& keys, std::int64_t elapsed,
                    GLFD::Core::StartupRateProbe& probe, std::int64_t now, int& triggered) {
  if (elapsed > 1000000000LL) { (void)clock.Advance(elapsed, /*paused=*/true); return 0; }
  latch.Sample(keys);
  const GLFD::Core::StepPlan plan = GLFD::Core::RunFixedSteps(clock, latch, elapsed, [&] {
    if (latch.IsTriggered('2')) { ++triggered; }
  });
  (void)probe.OnFrame(now, plan.steps);
  return plan.steps + static_cast<std::uint32_t>(plan.droppedSteps);
}
