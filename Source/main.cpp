#include "Engine.h"
#include "Core/GameConfig.h"
#include "Core/ConsoleClose.h"
#include "Core/Logger.h"
#include <iostream>

#ifdef  _WIN32
#include <windows.h>
#endif //  _WIN32

namespace {
  // **コンソールの信号の処理と共有する状態** (ECS 2-10)。処理は OS が作るスレッドで動き、main が返して
  // 静的な変数が壊されている最中も動き得るので、自明に壊せる型を constinit で置く (ConsoleClose.h)
  constinit GLFD::Core::CloseRequest g_closeRequest;
  constinit std::atomic<DWORD>       g_closeWaitMs{ 4000 };

  BOOL WINAPI OnConsoleSignal(DWORD ctrlType) {
    // ここでは Logger を呼ばない (書き込みで待たされた Logger の鍵を待ってしまう)
    return GLFD::Core::HandleConsoleSignal(
        ctrlType, g_closeRequest, g_closeWaitMs.load(std::memory_order_relaxed),
        [] { GLFD::Core::gLogToConsole.store(false, std::memory_order_relaxed); },
        [](HWND window) { if (window != nullptr) { (void)::PostMessageW(window, WM_CLOSE, 0, 0); } });
  }
}

int main() {
  // コンソールを閉じた / Ctrl+C / Ctrl+Break でも、窓の × と同じ終了の経路を通す (ECS 2-10)。
  // 以前はどれも既定の処理 (ExitProcess) で 25〜240 ms で切られ、join も Engine Shutdown も無かった
  UINT hungAppTimeout = 0;
  if (!::SystemParametersInfoW(SPI_GETHUNGAPPTIMEOUT, 0, &hungAppTimeout, 0)) { hungAppTimeout = 0; }
  g_closeWaitMs.store(GLFD::Core::CloseWaitLimit(hungAppTimeout), std::memory_order_relaxed);
  const bool closeReady = g_closeRequest.Init() && ::SetConsoleCtrlHandler(OnConsoleSignal, TRUE) != FALSE;

  // **GameEngine を壊してから知らせる** (ECS 2-9)。デストラクタが join → ログ → "Engine Shutdown" を
  // 済ませてから、メッセージボックスを出す。ボックスを閉じるまで待っても、ログは書き終わっている。
  // 知らせの文は、壊す前に写しておく
  GLFD::Graphics::ExitReason reason = GLFD::Graphics::ExitReason::Normal;
  wchar_t message[GLFD::Graphics::kExitMessageLength] = {};
  {
    GLFD::GameEngine engine;
    engine.SetCloseRequest(&g_closeRequest);
    engine.Initialize();
    LOG_INFO("console: %s. a close waits up to %lu ms for the shutdown (SPI_GETHUNGAPPTIMEOUT %u ms)",
             closeReady ? "close / Ctrl+C / Ctrl+Break go through the normal shutdown" : "COULD NOT install the handler",
             static_cast<unsigned long>(g_closeWaitMs.load(std::memory_order_relaxed)), hungAppTimeout);
    g_closeRequest.PublishWindow(static_cast<HWND>(engine.NativeWindow()));
    engine.Run();
    g_closeRequest.PublishWindow(nullptr);      // 壊れる窓に投げない
    reason = engine.ExitReason();
    wcsncpy_s(message, engine.ExitMessage(), _TRUNCATE);
  }
  // join -> ログ -> Engine Shutdown が済んだ。閉じる操作を待っている処理を放す (どの経路でも、ボックスより先に)。
  // 2-9 のボックスを出している間に閉じられたら、処理はすぐ TRUE を返し、OS がボックスごと終わらせる
  // (終了コードは 2 ではなくなる。ログは書き終わっている)
  g_closeRequest.MarkShutdownComplete();
  if (reason != GLFD::Graphics::ExitReason::Normal) {
    ::MessageBoxW(nullptr, message, L"GLFD", MB_OK | MB_ICONERROR);
  }
  // × で閉じた普通の終了 (0) と見分けられるように、0 以外で終わる
  return GLFD::Graphics::ExitCodeFor(reason);
}
