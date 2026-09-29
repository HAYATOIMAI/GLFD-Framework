#include "Engine.h"
#include "Core/GameConfig.h"
#include <iostream>

#ifdef  _WIN32
#include <windows.h>
#endif //  _WIN32

int main() {
  // **GameEngine を壊してから知らせる** (ECS 2-9)。デストラクタが join → ログ → "Engine Shutdown" を
  // 済ませてから、メッセージボックスを出す。ボックスを閉じるまで待っても、ログは書き終わっている。
  // 知らせの文は、壊す前に写しておく
  GLFD::Graphics::ExitReason reason = GLFD::Graphics::ExitReason::Normal;
  wchar_t message[GLFD::Graphics::kExitMessageLength] = {};
  {
    GLFD::GameEngine engine;
    engine.Initialize();
    engine.Run();
    reason = engine.ExitReason();
    wcsncpy_s(message, engine.ExitMessage(), _TRUNCATE);
  }
  if (reason != GLFD::Graphics::ExitReason::Normal) {
    ::MessageBoxW(nullptr, message, L"GLFD", MB_OK | MB_ICONERROR);
  }
  // × で閉じた普通の終了 (0) と見分けられるように、0 以外で終わる
  return GLFD::Graphics::ExitCodeFor(reason);
}
