#pragma once

/**
 * @file  ConsoleClose.h
 * @brief コンソールを閉じても・Ctrl+C でも、正しい終了の経路で終わる (ECS 2-10 A)
 *
 * @details
 *  ## なぜ要るか
 *  2-10 の手順1 で確かめると、ゲームの窓の × 以外(コンソールの窓を閉じる、Ctrl+C、Ctrl+Break)では、
 *  Windows Terminal でも conhost でも 25〜240 ms で `0xC000013A` で終わり、join の行も
 *  `=== Engine Shutdown ===` も出なかった。既定の処理が `ExitProcess` を呼ぶため。
 *  ユーザーは 2-9 の 18:44 の回で、コンソールを閉じてこれを踏んだ。
 *
 *  ## 形(2-10 の判断 A1)
 *  信号の処理は **OS がプロセスの中に作るスレッド**で動く(Learn: HandlerRoutine)。そこでは終了の処理を
 *  しない。次の順で頼むだけにする:
 *   1. 閉じる操作なら、コンソールへの書き込みを止める(閉じかけのコンソールで待たされると、猶予を
 *      食いつぶす)。**印より先に**止める。メインは印を見るとログに 1 行書くので
 *   2. **印を立てる**(原子的)。これが頼みの本体。メインは `Run` のループの頭でこれを見て抜ける
 *   3. ゲームの窓に `WM_CLOSE` を投げる。**早く起こすための合図にすぎない**。窓がまだ無い(起動中)、
 *      もう無い、で失敗しても印が残る(2-4 で合図と状態を分けたのと同じ)
 *   4. 閉じる操作(`CTRL_CLOSE_EVENT`)なら、メインが `GameEngine` を壊し終えた(join → ログ →
 *      `Engine Shutdown`)という知らせを、猶予より短い上限まで待ってから `TRUE` を返す。
 *      **`TRUE` を返した時点で OS がプロセスを終わらせる**(Learn)ので、先に返さない。
 *      Ctrl+C / Ctrl+Break は時間の制限が無く、`TRUE` ならプロセスは終わらされないので、待たずに返し、
 *      メインが普通に 0 で終わる
 *  ログオフ・シャットダウンは、窓を持つ(user32.dll を読み込む)プロセスには届かない(Learn)。
 *  `WM_QUERYENDSESSION` で受け取る形は今回入れていない(負債)。
 *
 *  ## プロセスの終わりまで生きていること
 *  処理のスレッドは、メインが `main` を返して静的な変数を壊している最中も動き得る。だから
 *  `CloseRequest` は**自明に壊せる型**にし(原子的な変数とハンドルだけ)、`constinit` で置く。
 *  イベントのハンドルは閉じない。処理は `Logger` を呼ばない(書き込みで待たされた `Logger` の鍵を
 *  待ってしまう)。
 *
 *  判断は `HandleConsoleSignal` に置き、`main` の処理とテスト (T-ECS-46) が同じ関数を呼ぶ。
 */

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace GLFD::Core {

  /// コンソールの信号の種類
  enum class ConsoleSignal : std::uint8_t {
    None,
    CtrlC,
    CtrlBreak,
    Close,   ///< コンソールの窓 / Windows Terminal のタブを閉じた
    Other,   ///< ログオフ・シャットダウン(窓を持つプロセスには届かない)
  };

  [[nodiscard]] constexpr ConsoleSignal ClassifyConsoleSignal(DWORD ctrlType) noexcept {
    switch (ctrlType) {
      case CTRL_C_EVENT:     return ConsoleSignal::CtrlC;
      case CTRL_BREAK_EVENT: return ConsoleSignal::CtrlBreak;
      case CTRL_CLOSE_EVENT: return ConsoleSignal::Close;
      default:               return ConsoleSignal::Other;
    }
  }

  [[nodiscard]] constexpr const char* ConsoleSignalText(ConsoleSignal s) noexcept {
    switch (s) {
      case ConsoleSignal::CtrlC:     return "Ctrl+C";
      case ConsoleSignal::CtrlBreak: return "Ctrl+Break";
      case ConsoleSignal::Close:     return "the console was closed";
      case ConsoleSignal::Other:     return "a console signal";
      case ConsoleSignal::None:      return "nothing";
    }
    return "?";
  }

  /// 信号ごとに何をするか
  struct ConsoleSignalAction {
    bool requestClose    = false;   ///< 印を立て、窓に WM_CLOSE を投げる
    bool silenceConsole  = false;   ///< コンソールへの書き込みを止める(印より先に)
    bool waitForShutdown = false;   ///< 終了の経路が済むまで待ってから返す
    bool handled         = false;   ///< TRUE を返す(FALSE なら次の処理 = 既定の ExitProcess へ)
  };

  [[nodiscard]] constexpr ConsoleSignalAction ActionFor(ConsoleSignal s) noexcept {
    switch (s) {
      case ConsoleSignal::CtrlC:
      case ConsoleSignal::CtrlBreak: return ConsoleSignalAction{ true, false, false, true };
      case ConsoleSignal::Close:     return ConsoleSignalAction{ true, true, true, true };
      default:                       return ConsoleSignalAction{};   // 既定の処理に任せる
    }
  }

  /**
   * @brief 処理のスレッドとメインのスレッドが共有する「閉じるよう頼まれた」の状態
   * @note  **自明に壊せる型にする**(プロセスの終わりに静的な変数が壊されている最中も、処理の
   *        スレッドが触り得るため)。ハンドルは閉じない
   */
  class CloseRequest {
  public:
    constexpr CloseRequest() noexcept = default;

    /// `main` の最初に 1 回。「終わった」の知らせ(手動で戻すイベント)を作る
    bool Init() noexcept {
      m_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      return m_done != nullptr;
    }

    // --- 処理のスレッド ---
    void Request(ConsoleSignal s) noexcept {
      m_signal.store(s, std::memory_order_relaxed);
      m_requested.store(true, std::memory_order_release);
    }
    /// 知らせを最大 `milliseconds` 待つ。知らせが来たら true
    [[nodiscard]] bool WaitShutdownComplete(DWORD milliseconds) const noexcept {
      return m_done != nullptr && ::WaitForSingleObject(m_done, milliseconds) == WAIT_OBJECT_0;
    }

    // --- メインのスレッド ---
    [[nodiscard]] bool Requested() const noexcept { return m_requested.load(std::memory_order_acquire); }
    [[nodiscard]] ConsoleSignal Signal() const noexcept { return m_signal.load(std::memory_order_relaxed); }
    /// 窓ができたら出す。`Run` を抜けたら nullptr に戻す(壊れた窓に投げないため)
    void PublishWindow(HWND window) noexcept { m_window.store(window, std::memory_order_release); }
    /// `GameEngine` を壊し終えたら(どの経路でも)1 回
    void MarkShutdownComplete() noexcept {
      if (m_done != nullptr) { ::SetEvent(m_done); }
    }

    [[nodiscard]] HWND Window() const noexcept { return m_window.load(std::memory_order_acquire); }

  private:
    std::atomic<bool>          m_requested{ false };
    std::atomic<ConsoleSignal> m_signal{ ConsoleSignal::None };
    std::atomic<HWND>          m_window{ nullptr };
    HANDLE                     m_done = nullptr;
  };

  static_assert(std::is_trivially_destructible_v<CloseRequest>,
                "CloseRequest: the console signal thread may still use it while static objects are being "
                "destroyed at exit (ECS 2-10). Keep it to atomics and handles; see ConsoleClose.h");

  /**
   * @brief 信号の処理の本体。**OS のスレッドで動く**。`Logger` を呼ばない
   * @param silence  コンソールへの書き込みを止める
   * @param post     窓(nullptr のこともある)に WM_CLOSE を投げる。失敗してよい
   * @param waitMs   閉じる操作のとき、終了の経路を待つ上限(猶予 `SPI_GETHUNGAPPTIMEOUT` より短く)
   * @return 処理したら TRUE
   */
  template <class SilenceFn, class PostFn>
  BOOL HandleConsoleSignal(DWORD ctrlType, CloseRequest& request, DWORD waitMs, SilenceFn&& silence, PostFn&& post) {
    const ConsoleSignal       signal = ClassifyConsoleSignal(ctrlType);
    const ConsoleSignalAction action = ActionFor(signal);
    if (!action.requestClose) { return FALSE; }
    if (action.silenceConsole) { silence(); }          // 1. 印より先に
    request.Request(signal);                           // 2. 頼みの本体
    post(request.Window());                            // 3. 早く起こすための合図(失敗してよい)
    if (action.waitForShutdown) {                      // 4. TRUE を返すと OS が終わらせるので、先に待つ
      (void)request.WaitShutdownComplete(waitMs);
    }
    return action.handled ? TRUE : FALSE;
  }

  /**
   * @brief 待つ上限。猶予(`SPI_GETHUNGAPPTIMEOUT`、既定 5000 ms)から余裕を引く
   * @param hungAppTimeoutMs 読んだ猶予。読めなければ 0 を渡す(文書の既定の 5000 ms とみなす)
   */
  [[nodiscard]] constexpr DWORD CloseWaitLimit(DWORD hungAppTimeoutMs) noexcept {
    constexpr DWORD kDocumentedDefault = 5000;
    constexpr DWORD kMargin            = 1000;   // 返してから OS が終わらせるまでと、測りの揺れの余裕
    constexpr DWORD kFloor             = 500;
    const DWORD grace = (hungAppTimeoutMs == 0) ? kDocumentedDefault : hungAppTimeoutMs;
    return (grace > kMargin + kFloor) ? grace - kMargin : kFloor;
  }

}
