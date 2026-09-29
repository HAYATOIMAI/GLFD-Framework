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
 *   - 43f: **隠れた → 確かめ直しで失敗**の順番で、黙って描画に戻らない。この順番は観察していない
 *          (【訂正】以前は「TDR で本物を観察した順番」と書いていた。17:14 に観察したのは、隠れた →
 *          確かめ直しで S_OK → 戻った、だけ)。起こり得る経路として塞ぐ
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
 *     【訂正】18:53 の dxcap -forcetdr で起きた。Present が DXGI_ERROR_DEVICE_REMOVED、理由
 *     DXGI_ERROR_DEVICE_RESET。このテストでは押さえず、実機の観察として記録した(T-ECS-44)
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
      CHECK(std::wcsstr(text, L"\u5931\u308f\u308c\u3066\u3044\u307e\u305b\u3093") != nullptr);   // "(デバイスは失われていません)"
      CHECK(std::wcsstr(text, L"\u30b2\u30fc\u30e0\u306e\u4e0d\u5177\u5408") != nullptr);   // "ゲームの不具合"
      CHECK(std::wcsstr(text, L"\u30ea\u30bb\u30c3\u30c8") == nullptr);   // "リセット" と書かない
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
    GLFD::Test::BeginCase("T-ECS-43f: OCCLUDED first and then a failing PRESENT_TEST (a possible order, not observed) stops, not resumes");

    // Present が OCCLUDED -> 止まっている間の確かめ直しで失敗(観察していない順番。起こり得る経路として塞ぐ)
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
    // 戻る経路(17:14 に dxcap を流したとき実際に起きたこと): OCCLUDED -> 確かめ直しで S_OK -> 描画に戻る
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
    CHECK(std::wcsstr(text, L"\u8d77\u52d5\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f") != nullptr);   // "起動できませんでした"
    CHECK(std::wcsstr(text, L"the graphics (DX11) could not be initialized") != nullptr);
    CHECK(std::wcsstr(text, L"\u4f5c\u696d\u30d5\u30a9\u30eb\u30c0: C:\\games\\x64\\Release") != nullptr);   // "作業フォルダ: "
    CHECK(std::wcsstr(text, L"\u30ed\u30b0: C:\\games\\x64\\Release\\Game.log") != nullptr);   // "ログ: "

    RenderFailure lost;
    (void)GLFD::Graphics::RecordFailure(lost, "Map", E_OUTOFMEMORY, DXGI_ERROR_DEVICE_REMOVED);
    CHECK(GLFD::Graphics::FormatExitMessage(text, ExitReason::RenderFailed, lost, nullptr, L"D:\\repo") > 0);
    CHECK(std::wcsstr(text, L"\u53d6\u308a\u5916") != nullptr);   // REMOVED: "取り外"
    CHECK(std::wcsstr(text, L"Map: E_OUTOFMEMORY (0x8007000E) / removed reason: DXGI_ERROR_DEVICE_REMOVED (0x887A0005)") != nullptr);
    CHECK(std::wcsstr(text, L"\u30ed\u30b0: D:\\repo\\Game.log") != nullptr);   // "ログ: "

    // 理由ごとに 1 行目を分ける(ユーザーは "device was lost" を「見つからない」と読んだ。18:53 の本物は RESET)
    struct Case { HRESULT reason; const wchar_t* must; const wchar_t* mustNot; };
    const Case cases[] = {
      { DXGI_ERROR_DEVICE_RESET,          L"\u30ea\u30bb\u30c3\u30c8\u3057\u305f", L"\u53d6\u308a\u5916" },   // "リセットした" / not "取り外"
      { DXGI_ERROR_DEVICE_HUNG,           L"\u5fdc\u7b54\u3057\u306a\u304f", L"\u53d6\u308a\u5916" },   // "応答しなく" / not "取り外"
      { DXGI_ERROR_DRIVER_INTERNAL_ERROR, L"\u5185\u90e8\u30a8\u30e9\u30fc", L"\u53d6\u308a\u5916" },   // "内部エラー" / not "取り外"
      { DXGI_ERROR_INVALID_CALL,          L"\u30b2\u30fc\u30e0\u306e\u4e0d\u5177\u5408", L"\u30ea\u30bb\u30c3\u30c8" },   // "ゲームの不具合" / not "リセット"
      { kUnknownDeviceRemoved,            L"\u4f7f\u3048\u306a\u3044\u72b6\u614b", L"\u30ea\u30bb\u30c3\u30c8" },   // "使えない状態" / not "リセット"
    };
    for (const Case& c : cases) {
      RenderFailure f;
      (void)GLFD::Graphics::RecordFailure(f, "Present", DXGI_ERROR_DEVICE_REMOVED, c.reason);
      CHECK(GLFD::Graphics::FormatExitMessage(text, ExitReason::RenderFailed, f, nullptr, L"D:\\repo") > 0);
      CHECK(std::wcsstr(text, c.must) != nullptr);
      CHECK(std::wcsstr(text, c.mustNot) == nullptr);
    }
    // RESET / HUNG / 内部エラーでは「GPU が無くなったわけではない」と書く。取り外し(REMOVED)では書かない
    RenderFailure reset;
    (void)GLFD::Graphics::RecordFailure(reset, "Present", DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET);
    CHECK(GLFD::Graphics::FormatExitMessage(text, ExitReason::RenderFailed, reset, nullptr, L"D:\\repo") > 0);
    CHECK(std::wcsstr(text, L"\u7121\u304f\u306a\u3063\u305f\u308f\u3051\u3067\u306f\u3042\u308a\u307e\u305b\u3093") != nullptr);
    CHECK(std::wcsstr(text, L"Present: DXGI_ERROR_DEVICE_REMOVED (0x887A0005) / removed reason: DXGI_ERROR_DEVICE_RESET (0x887A0007)") != nullptr);
    CHECK(GLFD::Graphics::FormatExitMessage(text, ExitReason::RenderFailed, lost, nullptr, L"D:\\repo") > 0);
    CHECK(std::wcsstr(text, L"\u7121\u304f\u306a\u3063\u305f\u308f\u3051\u3067\u306f\u3042\u308a\u307e\u305b\u3093") == nullptr);
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
