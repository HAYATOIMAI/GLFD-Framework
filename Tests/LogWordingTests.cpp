/**
 * @file  LogWordingTests.cpp
 * @brief T-ECS-45: ログの言葉が数えているものと一致する (ECS 2-10)
 *
 * @details
 *  2-8 でシミュレーションを固定の刻みにしてから、シーンの `OnUpdate` は刻みごとに呼ばれる。
 *  そこで数える行が「frame」と書いたままで、2-9 のレビューが「10 フレームで止んだ」と読み違えた
 *  (実際は 10 刻み = 0.16 秒)。
 *
 *  **言葉の決まり**: 「step」はシミュレーションの刻みだけに使う。フレーム(描画・`Run` の 1 周)は
 *  「frame」。1 回の更新の中のシステムの並びは「update stages」/「stage」。
 *
 *  本番と同じ関数(`EcsDiagnosticsLog.h` / `SimulationClockLog.h` / `RenderHealthLog.h`)を本物の
 *  `Logger` で呼び、書かれた行を読み戻して照らす(手順を写さない。開発手法 §4.7)。
 *
 *  ## 確かめること
 *   - 45a: 刻みを数える行(段・衝突・溢れ・Survivor・コマンドバッファ・初期値)が「step」を使い、
 *          「frame」を使わない
 *   - 45b: フレームを数える行(`Run` の刻みの遅れ、`frame rate`、描画の失敗 2 種)は「frame」のまま
 *   - 45c: 段の行の見出しは「update stages」。「step」を段の意味で使わない
 *
 *  ## 押さえないもの
 *   - この行を読む道具(`render_fault_check.ps1` など)が新しい行を読めること。流して確かめる
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Core/FailureGate.h"
#include "Core/Logger.h"
#include "Core/SimulationClock.h"
#include "Core/SimulationClockLog.h"
#include "ECS/CommandBuffer.h"
#include "ECS/Registry.h"
#include "Game/EcsDiagnosticsLog.h"
#include "Graphics/RenderHealthLog.h"
#include "Graphics/RenderStatus.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::Core::FailureGate;
using GLFD::Test::MockMemoryResource;

namespace {

  constexpr int kMaxLines   = 64;
  constexpr int kLineLength = 512;

  struct Lines {
    char text[kMaxLines][kLineLength]{};
    int  count = 0;
  };

  void TempPath(char (&path)[512], const char* name) {
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\%s", hasTemp ? tempDir : ".", name);
  }

  void ReadLines(const char* path, Lines& out) {
    FILE* f = nullptr;
    out.count = 0;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) { out.count = -1; return; }
    char buf[kLineLength];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
      if (out.count < kMaxLines) { strcpy_s(out.text[out.count], buf); }
      ++out.count;
    }
    std::fclose(f);
  }

  bool Contains(const char* line, const char* part) { return std::strstr(line, part) != nullptr; }

  /// 行の中に `part` を含む行が 1 つだけあれば、その添字。無い / 2 つ以上なら -1
  int FindOne(const Lines& lines, const char* part) {
    int found = -1;
    for (int i = 0; i < lines.count && i < kMaxLines; ++i) {
      if (Contains(lines.text[i], part)) {
        if (found >= 0) { return -1; }
        found = i;
      }
    }
    return found;
  }

  /// `head` で始まる行が 1 つだけあり、`must` を含み、"frame" を含まないこと
  void CheckStepLine(const Lines& lines, const char* head, const char* must) {
    const int i = FindOne(lines, head);
    CHECK(i >= 0);
    if (i < 0) {
      std::printf("      no single line with: %s\n", head);
      return;
    }
    CHECK(Contains(lines.text[i], must));
    CHECK(!Contains(lines.text[i], "frame"));
    if (!Contains(lines.text[i], must) || Contains(lines.text[i], "frame")) {
      std::printf("      line: %s", lines.text[i]);
    }
  }

  // ===========================================================================
  // 45a / 45c 刻みを数える行
  // ===========================================================================
  void TestStepLines() {
    GLFD::Test::BeginCase("T-ECS-45a/c: lines that count simulation steps say step, never frame; stages are stages");

    char path[512]{};
    TempPath(path, "glfd_log_wording_steps.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    // 段(1 回の更新の中のシステム): 失敗が 3 刻み続いて戻る
    {
      FailureGate gate;
      GLFD::Core::StageReport bad;
      bad.Record("Boid", GLFD::Core::StageResult::Failed);
      bad.Record("Collision", GLFD::Core::StageResult::Skipped);
      GLFD::Core::StageReport good;
      good.Record("Boid", GLFD::Core::StageResult::Ran);
      for (int i = 0; i < 3; ++i) { GLFD::Game::ReportUpdateStages(bad, gate); }
      GLFD::Game::ReportUpdateStages(good, gate);
    }
    // 衝突(Boid): 初めて届いた刻み、溢れ 2 刻み -> 戻る
    {
      FailureGate gate;
      bool first = false;
      GLFD::Game::ReportCollisionObservation(1500u, 0u, 1400u, first, gate);
      GLFD::Game::ReportCollisionObservation(18001u, 1617u, 16384u, first, gate);
      GLFD::Game::ReportCollisionObservation(17000u, 616u, 16384u, first, gate);
      GLFD::Game::ReportCollisionObservation(1600u, 0u, 1600u, first, gate);
    }
    // Survivor: 段の初回、全段がそろった、作れない 2 刻み -> 戻る、溢れ 3 刻み -> 戻る、要約、初期値
    {
      GLFD::Game::SurvivorState s;
      s.step = 212u;
      s.thisStep.enemiesSpawned = 2u;  s.thisStep.bulletsFired = 1u;  s.thisStep.hitsDelivered = 1u;
      s.thisStep.kills = 1u;           s.thisStep.pickupsCreated = 1u; s.thisStep.pickupsCollected = 1u;
      s.thisStep.pickupsExpired = 1u;  s.thisStep.enemiesReached = 1u;
      s.thisStep.createFailures = 3u;
      GLFD::Game::SurvivorLapLog lap;
      GLFD::Game::ReportSurvivorFirstLap(s, lap);

      FailureGate creation;
      GLFD::Game::ReportSurvivorCreation(s.thisStep, creation.Observe(true), creation);
      GLFD::Game::ReportSurvivorCreation(s.thisStep, creation.Observe(true), creation);
      GLFD::Game::ReportSurvivorCreation(s.thisStep, creation.Observe(false), creation);

      FailureGate queue;
      for (int i = 0; i < 3; ++i) { GLFD::Game::ReportEventQueue(GLFD::Events::BusCounters{ 20000u, 3616u }, queue); }
      GLFD::Game::ReportEventQueue(GLFD::Events::BusCounters{ 900u, 0u }, queue);

      s.step = 600u;
      GLFD::Game::ReportSurvivorSummary(s, 137u, 42u, 31u, 210u);
      GLFD::Game::ReportSurvivorPreset(GLFD::Game::SmallSurvivorParams());
    }
    // コマンドバッファ: 死んだハンドルへの破棄で取りこぼし 1 刻み -> 戻る
    {
      MockMemoryResource mock;
      GLFD::ECS::Registry      registry(&mock);
      GLFD::ECS::CommandBuffer commands(&mock);
      bool firstApply = false;
      FailureGate drops;
      const GLFD::ECS::Entity victim = registry.CreateEntity();
      registry.DestroyEntity(victim);
      CHECK(commands.Destroy(victim));
      registry.ApplyCommands(commands);
      GLFD::Game::ReportAppliedCommands(commands.Report(), firstApply, drops);
      registry.ApplyCommands(commands);                     // 空: 取りこぼしが止む
      GLFD::Game::ReportAppliedCommands(commands.Report(), firstApply, drops);
    }
    GLFD::Core::Logger::Get().Shutdown();

    Lines lines;
    ReadLines(path, lines);
    std::remove(path);
    CHECK(lines.count > 0);

    // 45c: 段の見出しは update stages。"step" を段の意味で使わない
    CheckStepLine(lines, "update stages: 1 failed, 1 skipped of 2", "update stages:");
    CheckStepLine(lines, "update stages: all systems ran again", "after 3 step(s) (3 bad step(s) total)");
    CHECK(FindOne(lines, "frame steps") < 0);
    // 45a: 刻みの行
    CheckStepLine(lines, "collision: first", "collision: first step with hits");
    CheckStepLine(lines, "published this step. the channel holds", "dropped 1617 of 18001 published this step");
    CheckStepLine(lines, "event queue: no longer overflowing after 2", "after 2 step(s) (2 bad step(s) total)");
    CheckStepLine(lines, "survivor: first enemy spawned", "at step 211 (2 in that step)");
    CheckStepLine(lines, "survivor: every stage of the loop", "(by step 211)");
    CheckStepLine(lines, "survivor: could not create entities", "this step");
    CheckStepLine(lines, "survivor: creating entities again", "after 2 step(s) (2 bad step(s) total)");
    CheckStepLine(lines, "published this step. each channel holds", "dropped 3616 of 20000 published this step");
    CheckStepLine(lines, "event queue: no longer overflowing after 3", "after 3 step(s) (3 bad step(s) total)");
    CheckStepLine(lines, "survivor: step 600 alive 210", "survivor: step 600");
    CheckStepLine(lines, "SurvivorScene: small preset.", "every 6 steps at radius");
    CheckStepLine(lines, "command buffer: no longer dropping", "after 1 step(s) (1 bad step(s) total)");
  }

  // ===========================================================================
  // 45b フレームを数える行は frame のまま
  // ===========================================================================
  void TestFrameLines() {
    GLFD::Test::BeginCase("T-ECS-45b: lines that count frames (the Run loop, drawing) still say frame");

    char path[512]{};
    TempPath(path, "glfd_log_wording_frames.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    // Run の 1 周(フレーム)ごとの刻みの遅れ
    {
      GLFD::Core::FixedStepAccumulator clock(4);
      GLFD::Core::StepDropReport report;
      GLFD::Core::ReportStepDrops(clock.Advance(1003000000LL), clock.MaxStepsPerFrame(), report);
      GLFD::Core::ReportStepDrops(clock.Advance(6944444LL), clock.MaxStepsPerFrame(), report);
      GLFD::Core::StartupRateProbe::Result measured;
      measured.framesPerSecond = 144.0; measured.seconds = 2.0; measured.stepsPerSecond = 62.5; measured.simulatedPerReal = 1.0;
      GLFD::Core::ReportStartupRate(measured, 144);
    }
    // 描画(フレームごと)
    {
      FailureGate gate;
      const GLFD::Systems::RenderStatus unavailable{ GLFD::Systems::RenderStatus::Outcome::VertexBufferUnavailable, 12u };
      const GLFD::Systems::RenderStatus drawn{ GLFD::Systems::RenderStatus::Outcome::Drawn, 12u };
      GLFD::Game::ReportRenderStep(unavailable, gate);
      GLFD::Game::ReportRenderStep(drawn, gate);
      GLFD::Graphics::SkippedDrawReport skipped;
      GLFD::Graphics::ReportSkippedDraws(true, E_OUTOFMEMORY, skipped);
      GLFD::Graphics::ReportSkippedDraws(false, S_OK, skipped);
    }
    GLFD::Core::Logger::Get().Shutdown();

    Lines lines;
    ReadLines(path, lines);
    std::remove(path);

    const int behind = FindOne(lines, "simulation: falling behind real time.");
    CHECK(behind >= 0);
    if (behind >= 0) {
      CHECK(Contains(lines.text[behind], "this frame; at most 4 step(s) run per frame"));
    }
    const int keeping = FindOne(lines, "simulation: keeping up again after 1 frame(s)");
    CHECK(keeping >= 0);
    CHECK(FindOne(lines, "frame rate: 144.0 fps") >= 0);
    CHECK(FindOne(lines, "RenderSystem: not drawing this frame.") >= 0);
    CHECK(FindOne(lines, "RenderSystem: drawing again after 1 frame(s) (1 dropped frame(s) total)") >= 0);
    CHECK(FindOne(lines, "render: not drawing this frame.") >= 0);
    CHECK(FindOne(lines, "render: drawing again after 1 frame(s) (1 frame(s) not drawn in total)") >= 0);
  }

}

int main() {
  GLFD::Test::BeginSuite("LogWording (ECS 2-10)");
  TestStepLines();
  TestFrameLines();
  return GLFD::Test::Summarize();
}
