/**
 * @file  ConsoleCloseTests.cpp
 * @brief T-ECS-46(単体の部分): コンソールの信号の処理が、決めた順番と待ち方で動く (ECS 2-10)
 *
 * @details
 *  本番の `main` の処理と同じ `HandleConsoleSignal` を呼ぶ(手順を写さない。開発手法 §4.7)。
 *  書き込みを止める・窓に投げる、は記録する関数に差し替え、「終わった」の知らせは別のスレッドから出す。
 *
 *  ## 確かめること
 *   - 46a: 信号の分け方。Ctrl+C / Ctrl+Break / 閉じる は頼む。それ以外(ログオフ・シャットダウン)は
 *          既定の処理に任せる(FALSE、印を立てない)
 *   - 46b: 閉じる操作の順番: **書き込みを止める → 印を立てる → 窓に投げる**。止める時点で印はまだ無い
 *          (メインが印を見て書く 1 行が、閉じかけのコンソールへ行かないように)
 *   - 46c: 閉じる操作は、「終わった」の知らせが来るまで返さない。来たらすぐ返す
 *   - 46d: 知らせが来なければ、上限で返す(猶予より短く)。上限は `SPI_GETHUNGAPPTIMEOUT` から引く
 *   - 46e: Ctrl+C / Ctrl+Break は待たずに返し、書き込みも止めない(コンソールは残る)
 *   - 46f: 窓がまだ無い(nullptr)ときも、印は立つ(窓に投げるのは合図にすぎない)
 *   - 46g: 知らせが先に出ていれば(2-9 のボックスを出している間など)、閉じる操作もすぐ返す
 *
 *  ## 押さえないもの
 *   - 本物のコンソール・OS のスレッド・`Run` の印の確かめ。`Tests/console_close_check.ps1` で確かめる
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <chrono>
#include <cstdint>
#include <thread>

#include "Core/ConsoleClose.h"

#include "TestHarness.h"

using GLFD::Core::CloseRequest;
using GLFD::Core::ConsoleSignal;

namespace {

  using Clock = std::chrono::steady_clock;

  /// 処理が呼んだものを、呼ばれた順に記録する
  struct Calls {
    int  order        = 0;
    int  silencedAt   = -1;   ///< 何番目に書き込みを止めたか
    int  postedAt     = -1;
    bool requestedWhenSilenced = true;   ///< 止めた時点で、印がもう立っていたか
    bool requestedWhenPosted   = false;  ///< 投げた時点で、印がもう立っていたか
    HWND postedTo     = nullptr;
    int  posts        = 0;
  };

  BOOL Handle(DWORD type, CloseRequest& request, DWORD waitMs, Calls& calls) {
    return GLFD::Core::HandleConsoleSignal(
        type, request, waitMs,
        [&] { calls.silencedAt = calls.order++; calls.requestedWhenSilenced = request.Requested(); },
        [&](HWND window) {
          calls.postedAt = calls.order++; calls.postedTo = window; ++calls.posts;
          calls.requestedWhenPosted = request.Requested();
        });
  }

  long long MillisecondsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  }

  // ===========================================================================
  void TestClassification() {
    GLFD::Test::BeginCase("T-ECS-46a: Ctrl+C, Ctrl+Break and close ask to stop; logoff and shutdown are left to the default");

    CHECK(GLFD::Core::ClassifyConsoleSignal(CTRL_C_EVENT) == ConsoleSignal::CtrlC);
    CHECK(GLFD::Core::ClassifyConsoleSignal(CTRL_BREAK_EVENT) == ConsoleSignal::CtrlBreak);
    CHECK(GLFD::Core::ClassifyConsoleSignal(CTRL_CLOSE_EVENT) == ConsoleSignal::Close);
    CHECK(GLFD::Core::ClassifyConsoleSignal(CTRL_LOGOFF_EVENT) == ConsoleSignal::Other);
    CHECK(GLFD::Core::ClassifyConsoleSignal(CTRL_SHUTDOWN_EVENT) == ConsoleSignal::Other);

    for (DWORD type : { static_cast<DWORD>(CTRL_LOGOFF_EVENT), static_cast<DWORD>(CTRL_SHUTDOWN_EVENT) }) {
      CloseRequest request;
      CHECK(request.Init());
      Calls calls;
      CHECK(Handle(type, request, 50, calls) == FALSE);   // 次の処理 (既定の ExitProcess) へ
      CHECK(!request.Requested());
      CHECK(calls.posts == 0);
      CHECK(calls.silencedAt < 0);
    }
  }

  // ===========================================================================
  void TestCloseOrderAndWait() {
    GLFD::Test::BeginCase("T-ECS-46b/c: a close silences the console, then sets the flag, then posts, and returns only after the shutdown");

    CloseRequest request;
    CHECK(request.Init());
    const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1234));
    request.PublishWindow(window);
    Calls calls;

    // メインの役: 印が立つのを待ち、200 ms かけて終わった後に知らせる
    std::thread main([&] {
      const auto until = Clock::now() + std::chrono::seconds(5);
      while (!request.Requested() && Clock::now() < until) { std::this_thread::yield(); }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      request.MarkShutdownComplete();
    });
    const auto t0 = Clock::now();
    const BOOL handled = Handle(CTRL_CLOSE_EVENT, request, 3000, calls);
    const long long waited = MillisecondsSince(t0);
    main.join();

    CHECK(handled == TRUE);
    CHECK(request.Requested());
    CHECK(request.Signal() == ConsoleSignal::Close);
    // 46b: 止める -> (印) -> 投げる。止めた時点で印はまだ無い
    CHECK(calls.silencedAt == 0);
    CHECK(calls.postedAt == 1);
    CHECK(!calls.requestedWhenSilenced);
    CHECK(calls.requestedWhenPosted);       // 投げる時点で印はもう立っている(合図を取りこぼしても印が残る)
    CHECK(calls.postedTo == window);
    // 46c: 知らせ (約 200 ms 後) まで返さず、来たら上限 (3000 ms) を待たずに返す
    CHECK(waited >= 150);
    CHECK(waited < 2500);
  }

  // ===========================================================================
  void TestCloseTimeout() {
    GLFD::Test::BeginCase("T-ECS-46d: without the notice a close returns at the limit, which stays below the OS grace period");

    CloseRequest request;
    CHECK(request.Init());
    Calls calls;
    const auto t0 = Clock::now();
    CHECK(Handle(CTRL_CLOSE_EVENT, request, 300, calls) == TRUE);
    const long long waited = MillisecondsSince(t0);
    CHECK(waited >= 250);
    CHECK(waited < 2000);

    // 上限: 猶予から 1000 ms 引く。読めなければ文書の既定 5000 ms とみなす。短すぎる猶予でも 500 ms は待つ
    CHECK(GLFD::Core::CloseWaitLimit(5000) == 4000u);
    CHECK(GLFD::Core::CloseWaitLimit(0) == 4000u);
    CHECK(GLFD::Core::CloseWaitLimit(8000) == 7000u);
    CHECK(GLFD::Core::CloseWaitLimit(1200) == 500u);
    CHECK(GLFD::Core::CloseWaitLimit(5000) < 5000u);
  }

  // ===========================================================================
  void TestCtrlCDoesNotWait() {
    GLFD::Test::BeginCase("T-ECS-46e: Ctrl+C and Ctrl+Break ask to stop and return at once; the console keeps being written");

    for (DWORD type : { static_cast<DWORD>(CTRL_C_EVENT), static_cast<DWORD>(CTRL_BREAK_EVENT) }) {
      CloseRequest request;
      CHECK(request.Init());
      Calls calls;
      const auto t0 = Clock::now();
      CHECK(Handle(type, request, 3000, calls) == TRUE);   // TRUE: プロセスは終わらされない。メインが 0 で終わる
      CHECK(MillisecondsSince(t0) < 1000);                  // 知らせは出ていないが待たない
      CHECK(request.Requested());
      CHECK(calls.silencedAt < 0);
      CHECK(calls.posts == 1);
    }
  }

  // ===========================================================================
  void TestNoWindowYet() {
    GLFD::Test::BeginCase("T-ECS-46f: before the window exists the flag is still set (posting is only a wake-up)");

    CloseRequest request;
    CHECK(request.Init());
    Calls calls;
    CHECK(Handle(CTRL_C_EVENT, request, 100, calls) == TRUE);
    CHECK(calls.postedTo == nullptr);
    CHECK(request.Requested());
    CHECK(request.Signal() == ConsoleSignal::CtrlC);
  }

  // ===========================================================================
  void TestNoticeAlreadyGiven() {
    GLFD::Test::BeginCase("T-ECS-46g: when the shutdown already finished (the 2-9 box is up), a close returns at once");

    CloseRequest request;
    CHECK(request.Init());
    request.MarkShutdownComplete();
    Calls calls;
    const auto t0 = Clock::now();
    CHECK(Handle(CTRL_CLOSE_EVENT, request, 3000, calls) == TRUE);
    CHECK(MillisecondsSince(t0) < 1000);
  }

}

int main() {
  GLFD::Test::BeginSuite("ConsoleClose (ECS 2-10)");
  TestClassification();
  TestCloseOrderAndWait();
  TestCloseTimeout();
  TestCtrlCDoesNotWait();
  TestNoWindowYet();
  TestNoticeAlreadyGiven();
  return GLFD::Test::Summarize();
}
