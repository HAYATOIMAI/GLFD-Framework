/**
 * @file  SimulationClockTests.cpp
 * @brief T-ECS-41: 刻みの数え方 (ECS 2-8)
 *
 * @details
 *  **実機の時計に頼らない。** 前のフレームからの経過(ナノ秒)を差し込み、本番の `Run` と
 *  同じ `Core::RunFixedSteps` / `KeyEdgeLatch` / `ReportStepDrops` を呼ぶ(手順を写さない。§4.7)。
 *
 *  ## 確かめること
 *   - 41a: 決まった経過の列で、進む刻みの数が期待どおり(144 Hz と 60 Hz 相当)
 *   - 41b: **長く回しても、シミュレーションの時間と実時間がずれていかない**(1,000 万フレーム)
 *   - 41c: 実時間が大きく飛んだとき、**上限で止まり**、超えた分を捨て、端数は残す
 *   - 41d: 実時間がほとんど進まないフレームでは刻みが 0 回。0 以下の経過は何もしない
 *   - 41e: 押された瞬間は**ちょうど 1 回の刻みに**届く。刻みが 0 回のフレームの瞬間は
 *          次のフレームの最初の刻みへ持ち越され、2 回のフレームでも 1 回だけ届く
 *   - 41f: 起動後に 1 回だけ、fps とシミュレーションの速さを測る
 *   - 41g: 刻みを捨て始めたとき / 止んだときだけ、`Logger` が実際に 1 行ずつ書く
 *   - 41h: 窓が隠れて止まっている間の経過は数えない。戻った最初のフレームも数えない。
 *          端数と、刻みにまだ届いていない押下は、止まる前のまま残る
 *   - 41i: 止まったとき / 戻ったときだけ、`Logger` が実際に 1 行ずつ書く
 *
 *  ## 差の現れない値を避ける (§4.2)
 *   - 経過は刻み (16,000,000 ns) の倍数ちょうどにしない(6,944,444 / 16,666,667 / 49,999 ns など)
 *   - 大きく飛んだ量も、刻みの倍数 + 端数にする(端数を捨てる変異が見えるように)
 *
 *  ## 押さえないもの
 *   - `GameEngine::Run` そのもの(DX11 を要求する)。`Run` は `RunFixedSteps` と
 *     `ReportStepDrops` / `ReportStartupRate` を呼ぶだけにしてある。実機の速さは T-ECS-42
 *   - `InputSystem` の `GetKeyboardState`(Win32)。読んだ後の扱いは `KeyEdgeLatch` に切り出した
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Core/FailureGate.h"
#include "Core/KeyEdgeLatch.h"
#include "Core/Logger.h"
#include "Core/SimulationClock.h"
#include "Core/SimulationClockLog.h"

#include "TestHarness.h"

using GLFD::Core::FixedStepAccumulator;
using GLFD::Core::KeyEdgeLatch;
using GLFD::Core::StepPlan;
using GLFD::Core::kSimulationStepNanoseconds;

namespace {

  constexpr std::int64_t kFrame144 = 6944444;    ///< 144 Hz のフレーム(刻みの倍数ではない)
  constexpr std::int64_t kFrame60  = 16666667;   ///< 60 Hz のフレーム(刻みより少し長い)

  /// 押された瞬間を数える入力の代わり。本番の `InputSystem` と同じく `BeginStep` を持つ
  struct NullInput {
    void BeginStep() noexcept {}
  };

  // ===========================================================================
  // 41a 決まった経過の列
  // ===========================================================================
  void TestStepCountsFollowRealTime() {
    GLFD::Test::BeginCase("T-ECS-41a: the number of steps per frame follows real time (144 Hz and 60 Hz frames)");

    // 144 Hz: 1 フレーム 6.94 ms。刻み 16 ms は 2〜3 フレームに 1 回
    {
      FixedStepAccumulator clock;
      std::int64_t  real  = 0;
      std::uint64_t steps = 0;
      bool eachFrameExact = true;
      int  zeroFrames = 0, oneFrames = 0, moreFrames = 0;
      for (int f = 0; f < 1000; ++f) {
        const StepPlan plan = clock.Advance(kFrame144);
        real  += kFrame144;
        steps += plan.steps;
        // 毎フレーム: 進んだ刻み = 実時間 / 刻み(切り捨て)、残り = 端数
        eachFrameExact = eachFrameExact
                      && steps == static_cast<std::uint64_t>(real / kSimulationStepNanoseconds)
                      && clock.PendingNanoseconds() == real % kSimulationStepNanoseconds
                      && plan.droppedSteps == 0u;
        if (plan.steps == 0u) { ++zeroFrames; } else if (plan.steps == 1u) { ++oneFrames; } else { ++moreFrames; }
      }
      CHECK(eachFrameExact);
      CHECK(steps == 434u);          // 6.944444 s / 0.016 s = 434.02
      CHECK(zeroFrames > 0);         // 刻みの無いフレームがある
      CHECK(oneFrames > 0);
      CHECK(moreFrames == 0);        // 144 Hz では 1 フレームに 2 回は来ない
    }

    // 60 Hz: 1 フレーム 16.67 ms。ほとんど 1 回、ときどき 2 回
    {
      FixedStepAccumulator clock;
      std::int64_t  real  = 0;
      std::uint64_t steps = 0;
      int twoFrames = 0;
      bool eachFrameExact = true;
      for (int f = 0; f < 600; ++f) {
        const StepPlan plan = clock.Advance(kFrame60);
        real  += kFrame60;
        steps += plan.steps;
        eachFrameExact = eachFrameExact && steps == static_cast<std::uint64_t>(real / kSimulationStepNanoseconds);
        if (plan.steps == 2u) { ++twoFrames; }
      }
      CHECK(eachFrameExact);
      CHECK(steps == 625u);          // 10.0000002 s / 0.016 s = 625.00001
      CHECK(twoFrames > 0);
    }
  }

  // ===========================================================================
  // 41b 長く回してもずれない
  // ===========================================================================
  void TestNoDriftOverLongRuns() {
    GLFD::Test::BeginCase("T-ECS-41b: over ten million frames the simulated time stays within one step of real time");

    FixedStepAccumulator clock;
    std::uint64_t steps = 0;
    const std::int64_t frames = 10000000;          // 144 Hz で 19 時間ぶん
    for (std::int64_t f = 0; f < frames; ++f) {
      steps += clock.Advance(kFrame144).steps;
    }
    const std::int64_t real = frames * kFrame144;
    CHECK(steps == static_cast<std::uint64_t>(real / kSimulationStepNanoseconds));
    CHECK(clock.PendingNanoseconds() == real % kSimulationStepNanoseconds);
    // シミュレーションの時間と実時間の差は 1 刻み未満
    const std::int64_t simulated = static_cast<std::int64_t>(steps) * kSimulationStepNanoseconds;
    CHECK(real - simulated >= 0 && real - simulated < kSimulationStepNanoseconds);
  }

  // ===========================================================================
  // 41c 大きく飛んだとき
  // ===========================================================================
  void TestLargeJumpStopsAtTheCap() {
    GLFD::Test::BeginCase("T-ECS-41c: a large jump runs at most the cap, drops whole steps beyond it, keeps the remainder");

    // 5.007 s 止まっていた(ブレークポイント、窓のドラッグ)。312 刻みぶん + 端数 15 ms
    {
      FixedStepAccumulator clock(4);
      const StepPlan plan = clock.Advance(5007000000LL);
      CHECK(plan.steps == 4u);
      CHECK(plan.droppedSteps == 308u);
      CHECK(clock.PendingNanoseconds() == 15000000);   // 端数は捨てない
      // 次の普通のフレームは、残った端数から続く: 15 ms + 6.94 ms で 1 刻み
      const StepPlan next = clock.Advance(kFrame144);
      CHECK(next.steps == 1u);
      CHECK(next.droppedSteps == 0u);
      CHECK(clock.PendingNanoseconds() == 15000000 + kFrame144 - kSimulationStepNanoseconds);
    }

    // 上限ちょうど(4 刻み + 1 ns)なら捨てない。上限 + 1 刻みなら 1 つ捨てる
    {
      FixedStepAccumulator clock(4);
      const StepPlan atCap = clock.Advance(4 * kSimulationStepNanoseconds + 1);
      CHECK(atCap.steps == 4u && atCap.droppedSteps == 0u && clock.PendingNanoseconds() == 1);
      const StepPlan overCap = clock.Advance(5 * kSimulationStepNanoseconds + 3);
      CHECK(overCap.steps == 4u && overCap.droppedSteps == 1u && clock.PendingNanoseconds() == 4);
    }

    // 上限は構築時の値に従う(1 にすれば 1 回ずつ)
    {
      FixedStepAccumulator clock(1);
      const StepPlan plan = clock.Advance(3 * kSimulationStepNanoseconds + 7);
      CHECK(plan.steps == 1u && plan.droppedSteps == 2u && clock.PendingNanoseconds() == 7);
    }

    // 2 時間止まっていても桁あふれしない(64 ビットのナノ秒があふれるのは約 292 年ぶんから)
    {
      FixedStepAccumulator clock(4);
      const std::int64_t twoHours = 7200LL * 1000000000LL + 12345;
      const StepPlan plan = clock.Advance(twoHours);
      CHECK(plan.steps == 4u);
      CHECK(plan.droppedSteps == static_cast<std::uint64_t>(twoHours / kSimulationStepNanoseconds) - 4u);
      CHECK(clock.PendingNanoseconds() >= 0 && clock.PendingNanoseconds() < kSimulationStepNanoseconds);
    }
  }

  // ===========================================================================
  // 41d ほとんど進まないフレーム
  // ===========================================================================
  void TestTinyFramesRunNoStep() {
    GLFD::Test::BeginCase("T-ECS-41d: frames that barely advance run no step until a whole step has built up");

    // 最小化中の Present は待たず、1 フレームが 0.05 ms 程度(2-8 手順1)。49,999 ns を 320 回で
    // 15,999,680 ns(刻みに 320 ns 足りない)、321 回目で初めて 1 刻み
    FixedStepAccumulator clock;
    bool zeroUntil320 = true;
    for (int f = 0; f < 320; ++f) { zeroUntil320 = zeroUntil320 && clock.Advance(49999).steps == 0u; }
    CHECK(zeroUntil320);
    CHECK(clock.PendingNanoseconds() == 320LL * 49999);
    CHECK(clock.Advance(49999).steps == 1u);
    CHECK(clock.PendingNanoseconds() == 321LL * 49999 - kSimulationStepNanoseconds);

    // 0 や負の経過(同じ時刻・時計が戻った)は何も足さない
    const std::int64_t before = clock.PendingNanoseconds();
    const StepPlan zero = clock.Advance(0);
    const StepPlan back = clock.Advance(-5000000);
    CHECK(zero.steps == 0u && zero.droppedSteps == 0u);
    CHECK(back.steps == 0u && back.droppedSteps == 0u);
    CHECK(clock.PendingNanoseconds() == before);
  }

  // ===========================================================================
  // 41e 押された瞬間
  // ===========================================================================

  /// 1 フレームぶん: 状態を読み、本番と同じ RunFixedSteps で刻みを回し、刻みごとに見えたものを数える
  struct InputFrameResult {
    int  steps = 0;
    int  triggeredSteps = 0;   ///< その刻みで IsTriggered(key) が真だった数
    int  releasedSteps  = 0;
    bool triggeredInFirstStepOnly = true;
  };

  InputFrameResult RunInputFrame(FixedStepAccumulator& clock, KeyEdgeLatch& latch,
                                 const KeyEdgeLatch::KeyStates& state, std::int64_t elapsed, int key) {
    latch.Sample(state);   // 本番の InputSystem::Update がフレームに 1 回これを呼ぶ
    InputFrameResult r;
    const StepPlan plan = GLFD::Core::RunFixedSteps(clock, latch, elapsed, [&] {
      const bool t = latch.IsTriggered(key);
      if (t) { ++r.triggeredSteps; if (r.steps != 0) { r.triggeredInFirstStepOnly = false; } }
      if (latch.IsReleased(key)) { ++r.releasedSteps; }
      ++r.steps;
    });
    CHECK_QUIET(static_cast<int>(plan.steps) == r.steps);
    return r;
  }

  void TestInputEdgesReachExactlyOneStep() {
    GLFD::Test::BeginCase("T-ECS-41e: a press reaches exactly one step: carried over a frame with no step, once in a frame with two");

    const int key = 'A';
    KeyEdgeLatch::KeyStates up{};
    KeyEdgeLatch::KeyStates down{};
    down[static_cast<std::size_t>(key)] = 0x80;

    // (1) 刻みが 0 回のフレームで押す -> 次のフレームの最初の刻みに 1 回だけ届く
    {
      FixedStepAccumulator clock;
      KeyEdgeLatch latch;
      const InputFrameResult f0 = RunInputFrame(clock, latch, down, 5000000, key);   // 5 ms: 0 刻み
      CHECK(f0.steps == 0 && f0.triggeredSteps == 0);
      const InputFrameResult f1 = RunInputFrame(clock, latch, down, 13000000, key);  // 計 18 ms: 1 刻み
      CHECK(f1.steps == 1);
      CHECK(f1.triggeredSteps == 1);
      // 押し続けても、もう届かない(瞬間ではない)
      const InputFrameResult f2 = RunInputFrame(clock, latch, down, 17000000, key);
      CHECK(f2.steps == 1 && f2.triggeredSteps == 0);
      CHECK(latch.IsPressed(key));
    }

    // (2) 刻みが 2 回のフレームで押す -> 最初の刻みだけに届く
    {
      FixedStepAccumulator clock;
      KeyEdgeLatch latch;
      const InputFrameResult f0 = RunInputFrame(clock, latch, down, 2 * kSimulationStepNanoseconds + 1234567, key);
      CHECK(f0.steps == 2);
      CHECK(f0.triggeredSteps == 1);
      CHECK(f0.triggeredInFirstStepOnly);
    }

    // (3) 刻みの無いフレームが 3 つ続いてから 3 刻み進む -> 1 回だけ
    {
      FixedStepAccumulator clock;
      KeyEdgeLatch latch;
      int triggered = 0;
      triggered += RunInputFrame(clock, latch, up,   3000000, key).triggeredSteps;
      triggered += RunInputFrame(clock, latch, down, 3000000, key).triggeredSteps;   // ここで押す
      triggered += RunInputFrame(clock, latch, down, 3000000, key).triggeredSteps;
      const InputFrameResult last = RunInputFrame(clock, latch, down, 3 * kSimulationStepNanoseconds, key);
      triggered += last.triggeredSteps;
      CHECK(last.steps == 3);
      CHECK(triggered == 1);
      CHECK(last.triggeredInFirstStepOnly);
    }

    // (4) 刻みの無いフレームで押し、次の刻みの無いフレームで離す -> 次の刻みに両方 1 回ずつ
    {
      FixedStepAccumulator clock;
      KeyEdgeLatch latch;
      const InputFrameResult a = RunInputFrame(clock, latch, down, 4000000, key);
      const InputFrameResult b = RunInputFrame(clock, latch, up,   4000000, key);
      const InputFrameResult c = RunInputFrame(clock, latch, up,   9000000, key);   // 計 17 ms: 1 刻み
      CHECK(a.steps == 0 && b.steps == 0 && c.steps == 1);
      CHECK(c.triggeredSteps == 1);
      CHECK(c.releasedSteps == 1);
      CHECK(!latch.IsPressed(key));
    }

    // (5) 別のキーは混ざらない。範囲外のキーは偽
    {
      KeyEdgeLatch latch;
      latch.Sample(down);
      latch.BeginStep();
      CHECK(latch.IsTriggered(key));
      CHECK(!latch.IsTriggered(key + 1));
      CHECK(!latch.IsTriggered(-1) && !latch.IsTriggered(256) && !latch.IsPressed(300));
    }
  }

  // ===========================================================================
  // 41f 起動後に 1 回だけ測る
  // ===========================================================================
  void TestStartupRateIsMeasuredOnce() {
    GLFD::Test::BeginCase("T-ECS-41f: the startup rate is measured once, after the settle time, from the steps actually run");

    FixedStepAccumulator clock;
    GLFD::Core::StartupRateProbe probe(0);
    NullInput input;
    std::int64_t now = 0;
    int reported = 0;
    for (int f = 0; f < 144 * 6; ++f) {   // 6 秒
      const StepPlan plan = GLFD::Core::RunFixedSteps(clock, input, kFrame144, [] {});
      now += kFrame144;
      if (probe.OnFrame(now, plan.steps)) { ++reported; }
    }
    CHECK(reported == 1);
    const GLFD::Core::StartupRateProbe::Result& m = probe.Measured();
    CHECK(std::fabs(m.framesPerSecond - 144.0) < 0.6);
    CHECK(std::fabs(m.stepsPerSecond - 62.5) < 1.0);
    CHECK(std::fabs(m.simulatedPerReal - 1.0) < 0.02);   // 2 秒で 1 刻み (0.016 s) の端数ぶんまで
    CHECK(m.seconds >= 2.0 && m.seconds < 2.01);

    // 2-7 までの形(毎フレーム 1 刻み)なら 2.30 倍と出る。**観測点がそれを見分けられる**こと
    GLFD::Core::StartupRateProbe oldWay(0);
    now = 0;
    for (int f = 0; f < 144 * 6; ++f) {
      now += kFrame144;
      (void)oldWay.OnFrame(now, 1u);
    }
    CHECK(std::fabs(oldWay.Measured().simulatedPerReal - 2.304) < 0.01);
  }

  // ===========================================================================
  // 41g 診断の行
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

  void TestDropDiagnosticsOnlyOnChanges() {
    GLFD::Test::BeginCase("T-ECS-41g: dropping steps is logged once when it starts and once when it stops; the startup line once");

    char path[512]{};
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\glfd_simulation_clock_test.log", hasTemp ? tempDir : ".");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    FixedStepAccumulator clock(4);
    GLFD::Core::StepDropReport report;
    NullInput input;
    // 普通 20 フレーム -> 1.003 s 止まる -> 80 ms のフレームが 2 つ(まだ遅れる)-> 普通 20 フレーム
    //                -> もう一度 0.5 s 止まる -> 普通 5 フレーム
    // 期待の数は scratchpad の独立したモデル (model_41g.py) で数えた。20 フレームの端数 10.9 ms が
    // 次のフレームに乗る(手で数えたときにこれを落とした)
    const std::int64_t script[] = { 1003000000LL, 80000000LL, 80000000LL };
    std::uint64_t dropped = 0;
    int stepsRun = 0;
    auto frame = [&](std::int64_t elapsed) {
      const StepPlan plan = GLFD::Core::RunFixedSteps(clock, input, elapsed, [&] { ++stepsRun; });
      dropped += plan.droppedSteps;
      GLFD::Core::ReportStepDrops(plan, clock.MaxStepsPerFrame(), report);
    };
    for (int f = 0; f < 20; ++f) { frame(kFrame144); }
    for (std::int64_t e : script) { frame(e); }
    for (int f = 0; f < 20; ++f) { frame(kFrame144); }
    frame(500000000LL);
    for (int f = 0; f < 5; ++f) { frame(kFrame144); }

    GLFD::Core::StartupRateProbe::Result measured;
    measured.framesPerSecond = 143.97; measured.seconds = 2.004; measured.stepsPerSecond = 62.4; measured.simulatedPerReal = 0.998;
    GLFD::Core::ReportStartupRate(measured, 144);
    GLFD::Core::Logger::Get().Shutdown();

    char lines[kMaxLines][kLineLength]{};
    const int n = ReadLines(path, lines);
    std::remove(path);

    CHECK(n == 5);   // 捨て始め・止み が 2 回ずつ + 起動の 1 行
    if (n >= 5) {
      // 端数 10.9 ms + 1.003 s = 63 刻み + 5.9 ms。4 回走らせ 59 捨てる
      CHECK(Contains(lines[0], "[WARN]"));
      CHECK(Contains(lines[0], "simulation: falling behind real time. dropped 59 step(s) (0.94 s) this frame"));
      CHECK(Contains(lines[0], "at most 4 step(s) run per frame"));
      // 80 ms のフレームは端数 5.9 ms と合わせて 5 刻みぶん -> 1 つずつ捨てる。3 フレーム続いて 61 刻み
      CHECK(Contains(lines[1], "[INFO]"));
      CHECK(Contains(lines[1], "simulation: keeping up again after 3 frame(s) (61 step(s) = 0.98 s dropped in that stretch, 61 in total)"));
      // 2 回目: 0.5 s。1 フレームだけ、27 刻み。合計は 1 回目との和
      CHECK(Contains(lines[2], "[WARN]"));
      CHECK(Contains(lines[2], "dropped 27 step(s) (0.43 s) this frame"));
      CHECK(Contains(lines[3], "simulation: keeping up again after 1 frame(s) (27 step(s) = 0.43 s dropped in that stretch, 88 in total)"));
      CHECK(Contains(lines[4], "frame rate: 144.0 fps over 2.0 s (monitor 144 Hz). simulation: 62.4 step(s)/s of 0.016 s = 1.00 x real time"));
    }
    CHECK(report.droppedTotal == dropped);
    CHECK(dropped == 88u);
  }


  // ===========================================================================
  // 41h 止まっている間
  // ===========================================================================
  void TestPausedTimeIsNotCounted() {
    GLFD::Test::BeginCase("T-ECS-41h: paused time is not counted, the resuming frame runs no step, remainder and a pending press survive");

    const int key = '2';
    KeyEdgeLatch::KeyStates down{};
    down[key] = 0x80;
    FixedStepAccumulator clock;
    KeyEdgeLatch latch;

    // 10 ms のフレームで押す。刻みは 0 回なので押下は持ち越し
    const InputFrameResult before = RunInputFrame(clock, latch, down, 10000000, key);
    CHECK(before.steps == 0);
    CHECK(clock.PendingNanoseconds() == 10000000);

    // 隠れて 3 秒あまり。Run と同じく、入力は読まず Advance(経過, true) だけ
    std::uint64_t pausedSteps = 0, pausedDropped = 0;
    for (int f = 0; f < 60; ++f) {
      const StepPlan plan = clock.Advance(50001234, /*paused=*/true);
      pausedSteps   += plan.steps;
      pausedDropped += plan.droppedSteps;
    }
    CHECK(pausedSteps == 0u);
    CHECK(pausedDropped == 0u);                       // 止まっているのは「遅れて捨てた」ではない
    CHECK(clock.PendingNanoseconds() == 10000000);    // 端数は止まる前のまま

    // 戻った最初のフレーム: 51 ms は隠れていた時間を含むので捨てる(数えれば 10 + 51 = 3 刻み)
    const InputFrameResult resume = RunInputFrame(clock, latch, down, 51000000, key);
    CHECK(resume.steps == 0);
    CHECK(clock.PendingNanoseconds() == 10000000);

    // 次のフレームからは数える: 10 + 7 = 17 ms -> 1 刻み。止まる前の押下はここにちょうど 1 回届く
    const InputFrameResult first = RunInputFrame(clock, latch, down, 7000000, key);
    CHECK(first.steps == 1);
    CHECK(first.triggeredSteps == 1);
    CHECK(clock.PendingNanoseconds() == 1000000);
    const InputFrameResult held = RunInputFrame(clock, latch, down, 16000000, key);
    CHECK(held.steps == 1);
    CHECK(held.triggeredSteps == 0);                  // 押し続けても 2 回目は届かない

    // 1 フレームだけ隠れても同じ: そのフレームと、戻ったフレームの経過を数えない
    (void)clock.Advance(20000000, /*paused=*/true);
    CHECK(clock.Advance(20000000).steps == 0u);
    CHECK(clock.Advance(15000000).steps == 1u);       // 1 + 15 = 16 ms ちょうど
    CHECK(clock.PendingNanoseconds() == 0);
  }

  // ===========================================================================
  // 41i 止まった / 戻ったの行
  // ===========================================================================
  void TestPauseDiagnosticsOnlyOnChanges() {
    GLFD::Test::BeginCase("T-ECS-41i: pausing is logged once when the window hides and once when it is back, with the paused time");

    char path[512]{};
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\glfd_simulation_pause_test.log", hasTemp ? tempDir : ".");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    GLFD::Core::PauseReport report;
    std::int64_t now = 0;
    auto frames = [&](bool paused, const char* why, int count, std::int64_t each) {
      for (int f = 0; f < count; ++f) {
        now += each;
        GLFD::Core::ReportPause(paused, why, now, report);
      }
    };
    frames(false, "unused", 10, kFrame144);
    frames(true, "window minimized", 50, 50000000);          // 最初の 1 フレームで止まる。2.45 s 後に戻る
    frames(false, "unused", 10, kFrame144);
    frames(true, "window occluded", 6, 50000000);            // 0.25 s 後 + 1 フレームで戻る
    frames(false, "unused", 5, kFrame144);
    GLFD::Core::Logger::Get().Shutdown();

    char lines[kMaxLines][kLineLength]{};
    const int n = ReadLines(path, lines);
    std::remove(path);

    CHECK(n == 4);
    if (n >= 4) {
      CHECK(Contains(lines[0], "[INFO]"));
      CHECK(Contains(lines[0], "simulation: paused (window minimized). no steps and no drawing until the window is visible again"));
      // 止まったのは 50 ms のフレームの 1 つ目の終わり、戻ったのは 144 Hz のフレームの 1 つ目の終わり:
      // 49 x 50 ms + 6.94 ms = 2.457 s
      CHECK(Contains(lines[1], "simulation: resumed after 2.5 s paused"));
      CHECK(Contains(lines[2], "simulation: paused (window occluded)."));
      // 5 x 50 ms + 6.94 ms = 0.257 s
      CHECK(Contains(lines[3], "simulation: resumed after 0.3 s paused"));
    }
    CHECK(report.pauses == 2u);
    CHECK(!report.paused);
  }

}

int main() {
  GLFD::Test::BeginSuite("SimulationClock (ECS 2-8)");
  TestStepCountsFollowRealTime();
  TestNoDriftOverLongRuns();
  TestLargeJumpStopsAtTheCap();
  TestTinyFramesRunNoStep();
  TestInputEdgesReachExactlyOneStep();
  TestStartupRateIsMeasuredOnce();
  TestDropDiagnosticsOnlyOnChanges();
  TestPausedTimeIsNotCounted();
  TestPauseDiagnosticsOnlyOnChanges();
  return GLFD::Test::Summarize();
}
