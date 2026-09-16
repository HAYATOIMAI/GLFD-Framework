/**
 * @file  probe_command_gate.cpp
 * @brief The Logger-free half of the diagnostics: the decision, not the output.
 *
 * @details
 *  6.4 - the layer that records and the layer that prints are different layers.
 *  This row is the mechanical version of that claim: it compiles the deciding
 *  code with no `Logger` anywhere in the TU, and must stay CLEAN.
 */
// EXPECT: CLEAN
// WHY: the deciding half must not throw, and must not depend on the Logger.

#include "Core/FailureGate.h"
#include "ECS/CommandBuffer.h"
#include "Game/CommandReportGate.h"

GLFD::Game::AppliedCommandsNotice Decide(const GLFD::ECS::ApplyReport& report,
                                         bool& loggedFirst,
                                         GLFD::Core::FailureGate& gate) {
  return GLFD::Game::DecideAppliedCommands(report, loggedFirst, gate);
}
