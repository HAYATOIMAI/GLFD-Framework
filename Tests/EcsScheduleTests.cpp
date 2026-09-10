/**
 * @file  EcsScheduleTests.cpp
 * @brief ECS 1-6: 実行順序の表 — T-ECS-18
 *
 * @details
 *  **表のとおりに走ること**と、**順序を 1 箇所で変えられること**を固定する。
 *
 *  `SystemStep` が `Host` と `Ctx` の両方をテンプレートなのは、ここで実物の
 *  `GameContext` を持ち込まずに済ませるためである(`GameContext.h` は
 *  `d3d11.h` まで引き込むので、`/W4 /WX` のテストに入れたくない)。
 *  **その設計判断がこのファイルの存在で報われている。**
 *
 *  ## 押さえられないもの
 *  **本番の表(`BoidDemoScene::OnUpdate` の `kUpdateOrder`)そのものは、
 *  ここからは見えない。** シーンは DX11 とウィンドウを要求するため。
 *  ここで固定できるのは**仕組みの側**(表のとおりに走る / 順序を変えれば
 *  結果が変わる / 前提条件で飛ばせる)であって、**本番の表の中身が正しい順序か
 *  どうかではない**。それは実機での動作確認が受け持つ。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/SystemSchedule.h"

#include "TestHarness.h"

using GLFD::Core::FrameReport;
using GLFD::Core::RunSteps;
using GLFD::Core::StepResult;
using GLFD::Core::SystemStep;

namespace {

  /// 走った順を書き込むだけの偽の文脈
  struct FakeContext {
    static constexpr std::size_t kMaxTrace = 32;
    char        trace[kMaxTrace]{};
    std::size_t traceLength = 0;
    bool        gridReady   = true;   ///< 前提条件の代わり

    void Mark(char c) {
      if (traceLength < kMaxTrace - 1u) {
        trace[traceLength++] = c;
        trace[traceLength]   = '\0';
      }
    }
  };

  struct FakeHost {
    int spawned = 0;
  };

  using Step = SystemStep<FakeHost, FakeContext>;

  // ---------------------------------------------------------------------------
  // T-ECS-18 実行順序
  // ---------------------------------------------------------------------------

  void TestStepsRunInTableOrder() {
    GLFD::Test::BeginCase("T-ECS-18a: the steps run in the order the table lists them");

    static constexpr Step kOrder[] = {
      { "alpha", [](FakeHost&, FakeContext& c) { c.Mark('a'); return StepResult::Ran; } },
      { "beta",  [](FakeHost&, FakeContext& c) { c.Mark('b'); return StepResult::Ran; } },
      { "gamma", [](FakeHost&, FakeContext& c) { c.Mark('g'); return StepResult::Ran; } },
    };

    FakeHost    host;
    FakeContext ctx;
    FrameReport report;
    RunSteps(kOrder, host, ctx, report);

    CHECK(std::strcmp(ctx.trace, "abg") == 0);

    // 報告に**表と同じ順で**名前が並ぶこと
    CHECK(report.StepCount() == 3u);
    CHECK(std::strcmp(report.NameAt(0), "alpha") == 0);
    CHECK(std::strcmp(report.NameAt(1), "beta") == 0);
    CHECK(std::strcmp(report.NameAt(2), "gamma") == 0);
    CHECK(report.ResultAt(0) == StepResult::Ran);
    CHECK(report.AllRan());
    CHECK(!report.Truncated());
    CHECK(report.CountOf(StepResult::Ran) == 3u);
    CHECK(report.CountOf(StepResult::Skipped) == 0u);
    CHECK(report.CountOf(StepResult::Failed) == 0u);
  }

  void TestOrderIsChangedInExactlyOnePlace() {
    GLFD::Test::BeginCase("T-ECS-18b: reordering the table is the only thing that reorders the run");

    // **同じ 3 つのステップ**を、表の並びだけ変えて 2 通り用意する
    constexpr auto alpha = [](FakeHost&, FakeContext& c) { c.Mark('a'); return StepResult::Ran; };
    constexpr auto beta  = [](FakeHost&, FakeContext& c) { c.Mark('b'); return StepResult::Ran; };
    constexpr auto gamma = [](FakeHost&, FakeContext& c) { c.Mark('g'); return StepResult::Ran; };

    static constexpr Step kForward[]  = { { "alpha", alpha }, { "beta", beta }, { "gamma", gamma } };
    static constexpr Step kShuffled[] = { { "gamma", gamma }, { "alpha", alpha }, { "beta", beta } };

    FakeHost    host;
    FrameReport report;

    FakeContext a;
    RunSteps(kForward, host, a, report);
    CHECK(std::strcmp(a.trace, "abg") == 0);
    CHECK(std::strcmp(report.NameAt(0), "alpha") == 0);

    FakeContext b;
    RunSteps(kShuffled, host, b, report);
    CHECK(std::strcmp(b.trace, "gab") == 0);
    CHECK(std::strcmp(report.NameAt(0), "gamma") == 0);

    // **走る順が本当に変わっていること。** 同じなら表が効いていない
    CHECK(std::strcmp(a.trace, b.trace) != 0);
  }

  void TestAFailingStepDoesNotStopTheRest() {
    GLFD::Test::BeginCase("T-ECS-18c: a failed step does not abandon the steps behind it");

    static constexpr Step kOrder[] = {
      { "first",  [](FakeHost&, FakeContext& c) { c.Mark('1'); return StepResult::Ran; } },
      { "broken", [](FakeHost&, FakeContext& c) { c.Mark('X'); return StepResult::Failed; } },
      { "last",   [](FakeHost&, FakeContext& c) { c.Mark('3'); return StepResult::Ran; } },
    };

    FakeHost    host;
    FakeContext ctx;
    FrameReport report;
    RunSteps(kOrder, host, ctx, report);

    // **後ろのステップは走る** (1-4 の R-37 と同じ理由)
    CHECK(std::strcmp(ctx.trace, "1X3") == 0);
    CHECK(report.StepCount() == 3u);
    CHECK(report.ResultAt(1) == StepResult::Failed);
    CHECK(!report.AllRan());
    CHECK(report.CountOf(StepResult::Failed) == 1u);
    CHECK(report.CountOf(StepResult::Ran) == 2u);
  }

  void TestPreconditionsSkipWithoutRunning() {
    GLFD::Test::BeginCase("T-ECS-18d: a step whose precondition fails is skipped, not run");

    // 「グリッドが作れなかったフレーム」の縮小版。**1 つの原因と 2 つの結果**
    static constexpr Step kOrder[] = {
      { "GridBuild", [](FakeHost&, FakeContext& c) {
          c.Mark('G');
          return c.gridReady ? StepResult::Ran : StepResult::Failed; } },
      { "Boid", [](FakeHost&, FakeContext& c) {
          if (!c.gridReady) { return StepResult::Skipped; }
          c.Mark('B');
          return StepResult::Ran; } },
      { "Movement", [](FakeHost&, FakeContext& c) {
          c.Mark('M');
          return StepResult::Ran; } },
      { "Collision", [](FakeHost&, FakeContext& c) {
          if (!c.gridReady) { return StepResult::Skipped; }
          c.Mark('C');
          return StepResult::Ran; } },
    };

    FakeHost    host;
    FrameReport report;

    {   // 平常
      FakeContext ctx;
      RunSteps(kOrder, host, ctx, report);
      CHECK(std::strcmp(ctx.trace, "GBMC") == 0);
      CHECK(report.AllRan());
    }
    {   // グリッドが作れなかったフレーム
      FakeContext ctx;
      ctx.gridReady = false;
      RunSteps(kOrder, host, ctx, report);

      // **飛ばされたステップの本体は動いていない**
      CHECK(std::strcmp(ctx.trace, "GM") == 0);

      // **1 つの原因 (Failed) と 2 つの結果 (Skipped) が区別できること**
      CHECK(report.CountOf(StepResult::Failed) == 1u);
      CHECK(report.CountOf(StepResult::Skipped) == 2u);
      CHECK(report.CountOf(StepResult::Ran) == 1u);
      CHECK(std::strcmp(report.NameAt(0), "GridBuild") == 0);
      CHECK(report.ResultAt(0) == StepResult::Failed);
      CHECK(report.ResultAt(1) == StepResult::Skipped);
      CHECK(report.ResultAt(3) == StepResult::Skipped);
      CHECK(!report.AllRan());
    }
  }

  void TestTheHostIsReachableFromEveryStep() {
    GLFD::Test::BeginCase("T-ECS-18e: steps can reach the host (that is how config is read)");

    // 本番では `self.m_config->simulation.maxSpeed` を読むのがこの経路
    static constexpr Step kOrder[] = {
      { "spawn", [](FakeHost& h, FakeContext&) { h.spawned += 2; return StepResult::Ran; } },
      { "again", [](FakeHost& h, FakeContext&) { h.spawned *= 3; return StepResult::Ran; } },
    };

    FakeHost    host;
    FakeContext ctx;
    FrameReport report;
    RunSteps(kOrder, host, ctx, report);

    CHECK(host.spawned == 6);        // (0 + 2) * 3。順序が逆なら 0
  }

  void TestReportIsResetOnEveryRun() {
    GLFD::Test::BeginCase("T-ECS-18f: the report describes one frame, not every frame so far");

    static constexpr Step kOne[] = {
      { "only", [](FakeHost&, FakeContext&) { return StepResult::Failed; } },
    };
    static constexpr Step kTwo[] = {
      { "a", [](FakeHost&, FakeContext&) { return StepResult::Ran; } },
      { "b", [](FakeHost&, FakeContext&) { return StepResult::Ran; } },
    };

    FakeHost    host;
    FakeContext ctx;
    FrameReport report;

    RunSteps(kOne, host, ctx, report);
    CHECK(report.StepCount() == 1u);
    CHECK(report.CountOf(StepResult::Failed) == 1u);

    RunSteps(kTwo, host, ctx, report);
    CHECK(report.StepCount() == 2u);              // **前のフレームが混ざらない**
    CHECK(report.CountOf(StepResult::Failed) == 0u);
    CHECK(report.AllRan());
  }

  void TestReportSaysWhenItRanOutOfRoom() {
    GLFD::Test::BeginCase("T-ECS-18g: the report says so when it cannot record everything");

    // 実行順序の表が上限を越えたら `RunSteps` の static_assert が**ビルドで**止める。
    // ここで見るのは `Record` を直に叩いた場合の振る舞い
    FrameReport report;
    report.Reset();
    for (std::uint32_t i = 0; i < FrameReport::kMaxSteps; ++i) {
      report.Record("step", StepResult::Ran);
    }
    CHECK(report.StepCount() == FrameReport::kMaxSteps);
    CHECK(!report.Truncated());

    report.Record("one too many", StepResult::Failed);
    CHECK(report.StepCount() == FrameReport::kMaxSteps);   // 増えない
    CHECK(report.Truncated());                             // **黙って落とさない**
    CHECK(report.CountOf(StepResult::Failed) == 0u);       // 記録できなかったので数にも出ない
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsSchedule (ECS 1-6)");

  TestStepsRunInTableOrder();
  TestOrderIsChangedInExactlyOnePlace();
  TestAFailingStepDoesNotStopTheRest();
  TestPreconditionsSkipWithoutRunning();
  TestTheHostIsReachableFromEveryStep();
  TestReportIsResetOnEveryRun();
  TestReportSaysWhenItRanOutOfRoom();

  return GLFD::Test::Summarize();
}
