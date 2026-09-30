/**
 * @file  probe_console_close.cpp
 * @brief The console signal handling added in ECS 2-10 (ConsoleClose.h).
 *
 * @details HandleConsoleSignal runs on the thread the OS creates for a console signal, and Run reads the flag
 *          at the top of every loop turn. Neither may throw (N-2): a throw on the OS's thread would terminate the
 *          process in the middle of the shutdown it is waiting for. main.cpp is not a row in
 *          translation_units.txt, so this probe is the only row that sees the handler by itself.
 */
// EXPECT: CLEAN
// WHY: 2-10: the console signal handler and the flag. atomics and Win32 event calls only.

#include "Core/ConsoleClose.h"

namespace {
  GLFD::Core::CloseRequest g_request;
}

BOOL Probe(DWORD ctrlType, bool& silenced, HWND& posted) {
  (void)g_request.Init();
  const BOOL handled = GLFD::Core::HandleConsoleSignal(
      ctrlType, g_request, GLFD::Core::CloseWaitLimit(5000),
      [&] { silenced = true; },
      [&](HWND window) { posted = window; });
  g_request.MarkShutdownComplete();
  return handled && g_request.Requested() ? TRUE : FALSE;
}
