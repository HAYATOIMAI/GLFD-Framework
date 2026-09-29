/**
 * @file  probe_render_health.cpp
 * @brief The decisions after Present / Present(TEST) / Map added in ECS 2-9.
 *
 * @details Per-frame path: DX11Renderer::EndFrame / IsOccluded / DrawPoints call these every frame.
 *          N-2 says it must not throw. DX11Renderer.cpp itself pulls in <iostream> and is not a row in
 *          translation_units.txt, so this probe is the only row that sees the decisions by themselves.
 *          FormatExitMessage (swprintf) is run once at exit, and is included on purpose: it must be clean too.
 *          The log lines (RenderHealthLog.h) go through LOG_* and are left out (probe_root_logger).
 */
// EXPECT: CLEAN
// WHY: 2-9: per-frame render result decisions. HRESULT compares, a fixed struct, swprintf into a fixed array.

#include "Graphics/RenderHealth.h"

namespace {
  struct Reason {
    HRESULT operator()() const noexcept { return DXGI_ERROR_DEVICE_HUNG; }
  };
}

int Frame(GLFD::Graphics::PresentState& state, HRESULT present, HRESULT test, HRESULT map,
          wchar_t (&text)[GLFD::Graphics::kExitMessageLength]) {
  GLFD::Graphics::AfterPresent(state, present, Reason{});
  const bool stopped = GLFD::Graphics::AfterPresentTest(state, test, Reason{});
  const GLFD::Graphics::MapOutcome mapped = GLFD::Graphics::AfterMap(state.failure, map, Reason{});
  const int written = GLFD::Graphics::FormatExitMessage(text, GLFD::Graphics::ExitReason::RenderFailed, state.failure,
                                                        nullptr, L"C:\\work");
  return (stopped ? 1 : 0) + static_cast<int>(mapped) + written + GLFD::Graphics::ExitCodeFor(GLFD::Graphics::ExitReason::Normal)
         + (GLFD::Graphics::HResultName(present) != nullptr ? 1 : 0);
}
