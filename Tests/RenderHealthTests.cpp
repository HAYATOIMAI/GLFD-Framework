/**
 * @file  RenderHealthTests.cpp
 * @brief T-ECS-43: 描画の呼び出しの戻り値の分け方と、失敗の行 (ECS 2-9)
 *
 * @details
 *  **DX11 を使わない。** `Present` / `Map` の戻り値を差し込み、本番の `DX11Renderer` と同じ
 *  `AfterPresent` / `AfterPresentTest` / `AfterMap` / `ReportRenderFailure` / `ReportSkippedDraws` /
 *  `FormatExitMessage` を呼ぶ(手順を写さない。開発手法 §4.7)。
 *
 *  ## 確かめること
 *   - 43a: `Present` の値の分け方。失敗の値は 1 つだけにしない(`== DXGI_ERROR_DEVICE_REMOVED` と書く
 *          変異を落とすため)。失われていない失敗(`INVALID_CALL`、理由 `S_OK`)は「失われた」と書かない
 *   - 43b: `Map` の失敗を、`GetDeviceRemovedReason` で「消失」と「このフレームは描かない」に分ける
 *   - 43c: 最初の失敗だけを覚える(後から来たもので上書きしない)
 *   - 43d: 失敗の行。名前と 16 進。失われたか / 失われていないかで文を分ける。表に無い値は 16 進だけ
 *   - 43e: 描かなかったフレームの行は、始まり / 止みだけ
 *   - 43f: **隠れた → 確かめ直しで失敗**の順番(TDR で本物を観察した順番)で、黙って描画に戻らない
 *   - 43g: 終了コードと、利用者への知らせの文
 *
 *  ## 差の現れない値を避ける (§4.2)
 *   - 返った値と理由の値は、いつも別の値にする(取り違えが見えるように)
 *   - 描かなかったフレームの続きは長さ 1 にしない
 *
 *  ## 押さえないもの
 *   - `DX11Renderer` と `Run` そのもの(DX11 と窓が要る)。確認用のビルド (`GLFD_RENDER_FAULT_PROBE`)
 *     で本物の DX11 に差し込んで確かめる(H1〜H6)
 *   - 本物のデバイスの消失で何が返るか。この機械では起こせなかった(dxcap -forcetdr では失われなかった)
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "Core/Logger.h"
#include "Graphics/RenderHealth.h"
#include "Graphics/RenderHealthLog.h"

#include "TestHarness.h"

using GLFD::Graphics::MapOutcome;
using GLFD::Graphics::PresentOutcome;
using GLFD::Graphics::PresentState;
using GLFD::Graphics::RenderFailure;

namespace {

  constexpr HRESULT kUnknownDeviceRemoved = static_cast<HRESULT>(0x88760870L);   ///< D3DDDIERR_DEVICEREMOVED の候補。表に無い値として使う

  /// `where` が空でも落ちない比べ方。記録が無いのを crash ではなく失敗した CHECK にする
  /// (変異 R4 / R6 で、strcmp が空の指し先を読んで suite ごと落ちた)
  bool WhereIs(const RenderFailure& failure, const char* expected) {
    return failure.where != nullptr && std::strcmp(failure.where, expected) == 0;
  }

  /// 失敗したときだけ呼ばれるはずの理由。呼ばれた回数を数える
  struct Reason {
    HRESULT value = S_OK;
    int     calls = 0;
    HRESULT operator()() { ++calls; return value; }
  };

  // ===========================================================================
  // 43a Present の値
  // ===========================================================================
  void TestPresentOutcomes() {
    GLFD::Test::BeginCase("T-ECS-43a: every failing Present value is a failure, OCCLUDED is not, other successes are shown");

    using GLFD::Graphics::ClassifyPresent;
    CHECK(ClassifyPresent(S_OK) == PresentOutcome::Shown);
    CHECK(ClassifyPresent(DXGI_STATUS_OCCLUDED) == PresentOutcome::Occluded);
    // OCCLUDED 以外の成功の値は、見えている扱い
    CHECK(ClassifyPresent(S_FALSE) == PresentOutcome::Shown);
    CHECK(ClassifyPresent(DXGI_STATUS_MODE_CHANGED) == PresentOutcome::Shown);
    // 失敗の値はどれも Failed。REMOVED だけを見る形では、下の 5 つが通り抜ける
    CHECK(ClassifyPresent(DXGI_ERROR_DEVICE_REMOVED) == PresentOutcome::Failed);
    CHECK(ClassifyPresent(DXGI_ERROR_DEVICE_RESET) == PresentOutcome::Failed);
    CHECK(ClassifyPresent(DXGI_ERROR_DEVICE_HUNG) == PresentOutcome::Failed);
    CHECK(ClassifyPresent(DXGI_ERROR_DRIVER_INTERNAL_ERROR) == PresentOutcome::Failed);
    CHECK(ClassifyPresent(kUnknownDeviceRemoved) == PresentOutcome::Failed);
    CHECK(ClassifyPresent(DXGI_ERROR_INVALID_CALL) == PresentOutcome::Failed);

    // 本番と同じ AfterPresent を通す: 成功のたびに理由を読まない。失敗したときだけ読む
    {
      PresentState state;
      Reason reason{ DXGI_ERROR_DEVICE_HUNG, 0 };
      GLFD::Graphics::AfterPresent(state, S_OK, reason);
      GLFD::Graphics::AfterPresent(state, DXGI_STATUS_OCCLUDED, reason);
      CHECK(state.occluded);
      CHECK(!state.failure.failed);
      CHECK(reason.calls == 0);
      GLFD::Graphics::AfterPresent(state, DXGI_ERROR_DEVICE_RESET, reason);
      CHECK(state.failure.failed);
      CHECK(!state.occluded);             // 失敗は「隠れた」ではない(止めて眠る経路に入らない)
      CHECK(reason.calls == 1);
      CHECK(state.failure.returned == DXGI_ERROR_DEVICE_RESET);
      CHECK(state.failure.removedReason == DXGI_ERROR_DEVICE_HUNG);
      CHECK(GLFD::Graphics::IsDeviceRemoved(state.failure));
    }
    // 失われていない失敗: 呼び方の誤り(INVALID_CALL)で理由が S_OK。「失われた」と書かない
    {
      PresentState state;
      Reason reason{ S_OK, 0 };
      GLFD::Graphics::AfterPresent(state, DXGI_ERROR_INVALID_CALL, reason);
      CHECK(state.failure.failed);
      CHECK(!GLFD::Graphics::IsDeviceRemoved(state.failure));
      wchar_t text[GLFD::Graphics::kExitMessageLength];
      GLFD::Graphics::FormatExitMessage(text, GLFD::Graphics::ExitReason::RenderFailed, state.failure, nullptr, L"C:\\work");
      CHECK(std::wcsstr(text, L"the graphics device was not lost") != nullptr);
      CHECK(std::wcsstr(text, L"The graphics device was lost") == nullptr);
    }
  }

  // ===========================================================================
  // 43b Map の失敗
  // ===========================================================================
  void TestMapOutcomes() {
    GLFD::Test::BeginCase("T-ECS-43b: a failed Map is a lost device only when GetDeviceRemovedReason says so; otherwise the frame is not drawn");

    // 成功: 理由を読まない
    {
      RenderFailure failure;
      Reason reason{ DXGI_ERROR_DEVICE_REMOVED, 0 };
      CHECK(GLFD::Graphics::AfterMap(failure, S_OK, reason) == MapOutcome::Written);
      CHECK(reason.calls == 0);
      CHECK(!failure.failed);
    }
    // 失敗 + 理由 REMOVED: 消失。Map の値と理由の値は別の値にする
    {
      RenderFailure failure;
      Reason reason{ DXGI_ERROR_DEVICE_REMOVED, 0 };
      CHECK(GLFD::Graphics::AfterMap(failure, E_OUTOFMEMORY, reason) == MapOutcome::DeviceLost);
      CHECK(reason.calls == 1);
      CHECK(failure.failed);
      CHECK(WhereIs(failure, "Map"));
      CHECK(failure.returned == E_OUTOFMEMORY);
      CHECK(failure.removedReason == DXGI_ERROR_DEVICE_REMOVED);
    }
    // 失敗 + 理由 S_OK: このフレームは描かない。失敗としては記録しない(ゲームは続く)
    {
      RenderFailure failure;
      Reason reason{ S_OK, 0 };
      CHECK(GLFD::Graphics::AfterMap(failure, E_OUTOFMEMORY, reason) == MapOutcome::SkipFrame);
      CHECK(reason.calls == 1);
      CHECK(!failure.failed);
    }
  }

  // ===========================================================================
  // 43c 最初の失敗だけ
  // ===========================================================================
  void TestFirstFailureWins() {
    GLFD::Test::BeginCase("T-ECS-43c: only the first failure is kept; a later one does not overwrite it");

    PresentState state;
    Reason first{ DXGI_ERROR_DEVICE_HUNG, 0 };
    GLFD::Graphics::AfterPresent(state, DXGI_ERROR_DEVICE_REMOVED, first);
    Reason later{ DXGI_ERROR_DRIVER_INTERNAL_ERROR, 0 };
    CHECK(GLFD::Graphics::AfterMap(state.failure, E_OUTOFMEMORY, later) == MapOutcome::DeviceLost);
    CHECK(WhereIs(state.failure, "Present"));
    CHECK(state.failure.returned == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(state.failure.removedReason == DXGI_ERROR_DEVICE_HUNG);
    CHECK(!GLFD::Graphics::RecordFailure(state.failure, "Map", E_FAIL, DXGI_ERROR_DEVICE_RESET));
    CHECK(state.failure.returned == DXGI_ERROR_DEVICE_REMOVED);
  }

  // ===========================================================================
  // 43d / 43e 行(本物の Logger で書き、読み戻す)
  // ===========================================================================
  constexpr int kMaxLines   = 16;
  constexpr int kLineLength = 512;

  int ReadLines(const char* path, char (&lines)[kMaxLines][kLineLength]) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) { return -1; }
    int n = 0;
    char buf[kLineLength];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
      if (n < kMaxLines) { strcpy_s(lines[n], buf); }
      ++n;
    }
    std::fclose(f);
    return n;
  }

  bool Contains(const char* line, const char* part) { return std::strstr(line, part) != nullptr; }

  void TempPath(char (&path)[512], const char* name) {
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\%s", hasTemp ? tempDir : ".", name);
  }

  void TestFailureLines() {
    GLFD::Test::BeginCase("T-ECS-43d: the failure line names the value and the reason, and says whether the device was lost");

    char path[512]{};
    TempPath(path, "glfd_render_failure_test.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    RenderFailure lost;
    (void)GLFD::Graphics::RecordFailure(lost, "Present", DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_HUNG);
    GLFD::Graphics::ReportRenderFailure(lost);
    RenderFailure notLost;
    (void)GLFD::Graphics::RecordFailure(notLost, "Present", DXGI_ERROR_INVALID_CALL, S_OK);
    GLFD::Graphics::ReportRenderFailure(notLost);
    RenderFailure unknown;
    (void)GLFD::Graphics::RecordFailure(unknown, "Present(TEST)", kUnknownDeviceRemoved, DXGI_ERROR_DEVICE_RESET);
    GLFD::Graphics::ReportRenderFailure(unknown);
    GLFD::Core::Logger::Get().Shutdown();

    char lines[kMaxLines][kLineLength]{};
    const int n = ReadLines(path, lines);
    std::remove(path);

    CHECK(n == 3);
    if (n >= 3) {
      CHECK(Contains(lines[0], "[ERR"));
      CHECK(Contains(lines[0], "render: the graphics device was lost at Present: DXGI_ERROR_DEVICE_REMOVED (0x887A0005), "
                               "removed reason DXGI_ERROR_DEVICE_HUNG (0x887A0006). stopping the game"));
      CHECK(Contains(lines[1], "render: Present failed: DXGI_ERROR_INVALID_CALL (0x887A0001). "
                               "the device was not lost (removed reason S_OK (0x00000000)). stopping the game"));
      CHECK(!Contains(lines[1], "was lost at"));
      CHECK(Contains(lines[2], "render: the graphics device was lost at Present(TEST): 0x88760870, "
                               "removed reason DXGI_ERROR_DEVICE_RESET (0x887A0007)"));
    }
  }

  void TestSkippedDrawLines() {
    GLFD::Test::BeginCase("T-ECS-43e: frames not drawn are logged once when they start and once when they stop");

    char path[512]{};
    TempPath(path, "glfd_skipped_draw_test.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    GLFD::Graphics::SkippedDrawReport report;
    auto frames = [&](bool skipped, HRESULT hr, int count) {
      for (int i = 0; i < count; ++i) { GLFD::Graphics::ReportSkippedDraws(skipped, hr, report); }
    };
    frames(false, S_OK, 5);
    frames(true, E_OUTOFMEMORY, 3);
    frames(false, S_OK, 4);
    frames(true, DXGI_ERROR_WAS_STILL_DRAWING, 2);
    frames(false, S_OK, 3);
    GLFD::Core::Logger::Get().Shutdown();

    char lines[kMaxLines][kLineLength]{};
    const int n = ReadLines(path, lines);
    std::remove(path);

    CHECK(n == 4);
    if (n >= 4) {
      CHECK(Contains(lines[0], "[WARN]"));
      CHECK(Contains(lines[0], "render: not drawing this frame. the vertex buffer could not be mapped: "
                               "E_OUTOFMEMORY (0x8007000E); the device was not lost"));
      CHECK(Contains(lines[1], "render: drawing again after 3 frame(s) (3 frame(s) not drawn in total)"));
      CHECK(Contains(lines[2], "DXGI_ERROR_WAS_STILL_DRAWING (0x887A000A)"));
      CHECK(Contains(lines[3], "render: drawing again after 2 frame(s) (5 frame(s) not drawn in total)"));
    }
    CHECK(report.skippedTotal == 5u);
  }

  // ===========================================================================
  // 43f 隠れた → 確かめ直しで失敗
  // ===========================================================================
  void TestOccludedThenFailed() {
    GLFD::Test::BeginCase("T-ECS-43f: OCCLUDED first and then a failing PRESENT_TEST (the order a real TDR showed) stops, not resumes");

    // TDR で観察した順番: Present が OCCLUDED -> 止まっている間の確かめ直し
    {
      PresentState state;
      Reason reason{ DXGI_ERROR_DEVICE_HUNG, 0 };
      GLFD::Graphics::AfterPresent(state, DXGI_STATUS_OCCLUDED, reason);
      CHECK(state.occluded);
      CHECK(GLFD::Graphics::AfterPresentTest(state, DXGI_STATUS_OCCLUDED, reason));   // まだ隠れている
      CHECK(!state.failure.failed);
      const bool stillStopped = GLFD::Graphics::AfterPresentTest(state, DXGI_ERROR_DEVICE_REMOVED, reason);
      CHECK(stillStopped);                     // 「見えた」と読んで描画に戻らない
      CHECK(state.failure.failed);             // Run はこれを見て終わる
      CHECK(WhereIs(state.failure, "Present(TEST)"));
      CHECK(state.failure.returned == DXGI_ERROR_DEVICE_REMOVED);
      CHECK(state.failure.removedReason == DXGI_ERROR_DEVICE_HUNG);
      CHECK(reason.calls == 1);
    }
    // 生き残る経路(TDR で実際に起きたこと): OCCLUDED -> 確かめ直しで S_OK -> 描画に戻る
    {
      PresentState state;
      Reason reason{ DXGI_ERROR_DEVICE_HUNG, 0 };
      GLFD::Graphics::AfterPresent(state, DXGI_STATUS_OCCLUDED, reason);
      CHECK(!GLFD::Graphics::AfterPresentTest(state, S_OK, reason));
      CHECK(!state.occluded);
      CHECK(!state.failure.failed);
      CHECK(reason.calls == 0);
    }
  }

  // ===========================================================================
  // 43g 終わり方
  // ===========================================================================
  void TestExitCodesAndMessages() {
    GLFD::Test::BeginCase("T-ECS-43g: exit codes tell a normal close from a failure, and the message says where to look");

    using GLFD::Graphics::ExitCodeFor;
    using GLFD::Graphics::ExitReason;
    CHECK(ExitCodeFor(ExitReason::Normal) == 0);
    CHECK(ExitCodeFor(ExitReason::StartupFailed) != 0);
    CHECK(ExitCodeFor(ExitReason::RenderFailed) != 0);
    CHECK(ExitCodeFor(ExitReason::StartupFailed) != ExitCodeFor(ExitReason::RenderFailed));

    wchar_t text[GLFD::Graphics::kExitMessageLength];
    const RenderFailure none{};
    CHECK(GLFD::Graphics::FormatExitMessage(text, ExitReason::Normal, none, nullptr, L"C:\\work") == 0);
    CHECK(text[0] == L'\0');

    CHECK(GLFD::Graphics::FormatExitMessage(text, ExitReason::StartupFailed, none,
                                            "the graphics (DX11) could not be initialized", L"C:\\games\\x64\\Release") > 0);
    CHECK(std::wcsstr(text, L"could not start: the graphics (DX11) could not be initialized") != nullptr);
    CHECK(std::wcsstr(text, L"Working folder: C:\\games\\x64\\Release") != nullptr);
    CHECK(std::wcsstr(text, L"Log: C:\\games\\x64\\Release\\Game.log") != nullptr);

    RenderFailure lost;
    (void)GLFD::Graphics::RecordFailure(lost, "Map", E_OUTOFMEMORY, DXGI_ERROR_DEVICE_REMOVED);
    CHECK(GLFD::Graphics::FormatExitMessage(text, ExitReason::RenderFailed, lost, nullptr, L"D:\\repo") > 0);
    CHECK(std::wcsstr(text, L"The graphics device was lost") != nullptr);
    CHECK(std::wcsstr(text, L"Map returned E_OUTOFMEMORY (0x8007000E)") != nullptr);
    CHECK(std::wcsstr(text, L"removed reason: DXGI_ERROR_DEVICE_REMOVED (0x887A0005)") != nullptr);
    CHECK(std::wcsstr(text, L"Log: D:\\repo\\Game.log") != nullptr);
  }

}

int main() {
  GLFD::Test::BeginSuite("RenderHealth (ECS 2-9)");
  TestPresentOutcomes();
  TestMapOutcomes();
  TestFirstFailureWins();
  TestFailureLines();
  TestSkippedDrawLines();
  TestOccludedThenFailed();
  TestExitCodesAndMessages();
  return GLFD::Test::Summarize();
}
