/**
 * @file  probe_diagnostics.cpp
 * @brief The reporting side: gates, the schedule report, and the Logger calls.
 *
 * @warning Dirty through `Logger::LogFmt` only - see `probe_root_logger.cpp`.
 *          The decision logic was split out of the Logger calls in 1-8 exactly
 *          so it could be tested on its own; `probe_command_gate.cpp` audits
 *          that half and must stay CLEAN.
 */
// EXPECT: THROW
// WHY: every Report* function ends in LOG_*. See probe_root_logger.

#include "Core/GameContext.h"
#include "Game/CommandReportGate.h"
#include "Game/EcsDiagnosticsLog.h"

void Report(const GLFD::ECS::ApplyReport& applied, bool& loggedFirst,
            GLFD::Core::FailureGate& gate, const GLFD::Core::FrameReport& frame,
            const GLFD::Events::BusCounters& bus, const GLFD::Systems::RenderStatus& render,
            const GLFD::Game::SurvivorCounts& counts, GLFD::Game::SurvivorLapLog& lap,
            GLFD::Game::SurvivorState& state) {
  GLFD::Game::ReportAppliedCommands(applied, loggedFirst, gate);
  GLFD::Game::ReportFrameSteps(frame, gate);
  GLFD::Game::ReportEventQueue(bus, gate);
  GLFD::Game::ReportRenderStep(render, gate);
  GLFD::Game::ReportSurvivorCreation(counts, GLFD::Core::FailureGate::Change{}, gate);
  GLFD::Game::ReportSurvivorFirstLap(state, lap);
  GLFD::Game::ReportSurvivorSummary(state, 1u, 2u, 3u, 4u);
}
