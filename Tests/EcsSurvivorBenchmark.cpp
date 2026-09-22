/**
 * @file  EcsSurvivorBenchmark.cpp
 * @brief ECS 1-8 の性能基準線(1-8 のループ、定常状態)— フェーズ2以降の出発点
 *
 * @details
 *  ## 基準線は 2 本 (1-8 §1-D)
 *   1. **Boid の非回帰線** — 既存の `EcsBenchmark`。1-7 の数字(中央値 1105us)と比べる
 *   2. **1-8 の線** — これ。`RunSurvivorFrame` と**同じ順序表 `kSurvivorOrder` を**
 *      1 段ずつ時間を挟んで回す。表を写していないので、順序を変えればここにも出る
 *
 *  ## 定常状態に入ってから測る
 *  既定の助走は 900 フレーム(15 秒)。敵は最長 10 秒で消えるので、それより長く回すと
 *  **生成数と破棄数が釣り合い、空き番号の再利用で dense の並びがかき混ぜられる**。
 *  1-5 の実測で、並びが恒等写像のままだと Entity -> dense の費用 (+44%) が見えなかった。
 *  測り始めに並びが実際に崩れていたかを出力する。
 *
 *  ## 件数と費用を並べる (1-7)
 *  段ごとの us に加えて、1 フレームあたりの生成数・破棄数・適用コマンド数・命中数・
 *  生存数を出す。**費用だけでは、軽くなったのか仕事が減ったのか区別できない。**
 *
 *  ## ビルド
 *  `run_survivor_benchmark.bat`。**ゲーム本体と同じフラグ (/W3 /sdl)** で建てる
 *  (`run_ecs_benchmark.bat` の @note と同じ理由)。
 *
 *  usage: EcsSurvivorBenchmark.exe [frames] [warmup] [small|large]   (既定: 600 900 small)
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Core/DoubleStackAllocator.h"
#include "Core/GameContext.h"
#include "Core/StackAllocator.h"
#include "Core/StackResource.h"
#include "Core/SystemSchedule.h"

#include "ECS/CommandBuffer.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/View.h"

#include "Events/EventBus.h"

#include "Game/SurvivorComponents.h"
#include "Game/SurvivorLoop.h"

#include "Threading/JobSystem.h"

namespace {

  using Clock = std::chrono::steady_clock;

  constexpr int         kMaxFrames = 4096;
  constexpr std::size_t kSteps     = sizeof(GLFD::Game::kSurvivorOrder)
                                   / sizeof(GLFD::Game::kSurvivorOrder[0]);

  enum Metric { kCreated, kDestroyed, kApplied, kHits, kAlive, kMetricCount };
  const char* const kMetricNames[kMetricCount] = {
    "created", "destroyed", "commands applied", "hits delivered", "alive",
  };

  double g_steps[kSteps][kMaxFrames];
  double g_frame[kMaxFrames];
  double g_metrics[kMetricCount][kMaxFrames];

  void SortAscending(double* values, int count) {
    for (int i = 1; i < count; ++i) {
      const double key = values[i];
      int j = i - 1;
      while (j >= 0 && values[j] > key) {
        values[j + 1] = values[j];
        --j;
      }
      values[j + 1] = key;
    }
  }

  struct Summary {
    double median = 0.0;
    double mean   = 0.0;
    double min    = 0.0;
    double max    = 0.0;
    double p95    = 0.0;
  };

  Summary Summarize(double* values, int count) {
    Summary s;
    if (count <= 0) { return s; }
    double total = 0.0;
    for (int i = 0; i < count; ++i) { total += values[i]; }
    s.mean = total / count;
    SortAscending(values, count);
    s.min    = values[0];
    s.max    = values[count - 1];
    s.median = values[count / 2];
    s.p95    = values[(count * 95) / 100];
    return s;
  }

  /// dense の並びが index の並びから崩れているか(崩れていないと R-33 の費用が見えない)
  bool DenseOrderScrambled(GLFD::ECS::Registry& registry) {
    const auto         view   = registry.View<GLFD::Components::Position>();
    const GLFD::ECS::Entity* const owners = view.BaseEntities();
    for (std::size_t i = 0; i < view.BaseSize(); ++i) {
      if (owners[i].Index() != i) { return true; }
    }
    return false;
  }

}

int main(int argc, char** argv) {
  int frames = (argc > 1) ? std::atoi(argv[1]) : 600;
  int warmup = (argc > 2) ? std::atoi(argv[2]) : 900;
  const char* const preset = (argc > 3) ? argv[3] : "small";
  if (frames < 1)          { frames = 1; }
  if (frames > kMaxFrames) { frames = kMaxFrames; }
  if (warmup < 0)          { warmup = 0; }

  const bool large = (std::strcmp(preset, "large") == 0);
  const GLFD::Game::SurvivorParams params =
      large ? GLFD::Game::LargeSurvivorParams() : GLFD::Game::SmallSurvivorParams();

  GLFD::Memory::StackAllocator       mainStack(512u * 1024u * 1024u);
  GLFD::Memory::StackResource        globalResource(mainStack);
  GLFD::Memory::DoubleStackAllocator frameAllocator(64u * 1024u * 1024u);

  GLFD::Thread::JobSystem  jobSystem;
  GLFD::ECS::Registry      registry(&globalResource);
  GLFD::ECS::CommandBuffer commands(&globalResource);
  GLFD::Events::EventBus   eventBus(&globalResource);

  GLFD::Game::SurvivorState state;
  state.params = params;
  (void)GLFD::Game::AttachSurvivor(state, registry, commands, eventBus);

  std::printf("=== ECS survivor benchmark (baseline for phase 2) ===\n");
#ifdef NDEBUG
  std::printf("(configuration: Release / NDEBUG)\n");
#else
  std::printf("(configuration: Debug)\n");
#endif
  std::printf("preset: %s  frames: %d  warmup: %d  workers: %u\n",
              large ? "large" : "small", frames, warmup,
              static_cast<unsigned>(GLFD::System::WorkerThreadCount()));
  std::fflush(stdout);

  const auto us = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
  };

  bool          scrambledAtStart = false;
  std::uint32_t notRan           = 0;
  std::uint32_t droppedEvents    = 0;
  std::size_t   maxEnemies = 0, maxBullets = 0, maxPickups = 0;

  const int total = warmup + frames;
  for (int f = 0; f < total; ++f) {
    frameAllocator.SwapAndReset();
    GLFD::Memory::StackResource frameResource(frameAllocator.GetCurrent());

    GLFD::GameContext ctx{
        &globalResource, &frameResource, &jobSystem, &registry, &commands, &eventBus,
        nullptr,          // grid: GridBuild が差し込む
        nullptr, nullptr, nullptr,
        0.016f,
        nullptr, static_cast<float>(f) * 0.016f, nullptr, nullptr };

    const bool measuring = (f >= warmup);
    const int  m         = f - warmup;

    // **`RunSurvivorFrame` と同じ表を 1 段ずつ回す**(表を写さない)
    GLFD::Game::BeginSurvivorFrame(state);
    const auto frameStart = Clock::now();
    for (std::size_t i = 0; i < kSteps; ++i) {
      const auto t0 = Clock::now();
      const GLFD::Core::StepResult result = GLFD::Game::kSurvivorOrder[i].run(state, ctx);
      const auto t1 = Clock::now();
      if (measuring) {
        g_steps[i][m] = us(t0, t1);
        if (result != GLFD::Core::StepResult::Ran) { ++notRan; }
      }
    }
    const auto frameEnd = Clock::now();
    GLFD::Game::EndSurvivorFrame(state);

    const GLFD::Events::BusCounters bus = eventBus.Counters();
    eventBus.ResetCounters();

    if (!measuring) { continue; }

    const GLFD::Game::SurvivorCounts& c = state.thisFrame;
    g_frame[m] = us(frameStart, frameEnd);
    g_metrics[kCreated][m]   = c.enemiesSpawned + c.bulletsFired + c.pickupsCreated;
    g_metrics[kDestroyed][m] = c.kills + c.enemiesReached + c.bulletsSpent + c.bulletsExpired
                             + c.pickupsExpired + c.pickupsCollected;
    g_metrics[kApplied][m]   = commands.Report().Applied();
    g_metrics[kHits][m]      = c.hitsDelivered;
    g_metrics[kAlive][m]     = registry.AliveCount();
    droppedEvents += bus.dropped;

    const std::size_t e = registry.View<GLFD::Components::Health>().BaseSize();
    const std::size_t b = registry.View<GLFD::Components::Damage>().BaseSize();
    const std::size_t p = registry.View<GLFD::Components::Pickup>().BaseSize();
    if (e > maxEnemies) { maxEnemies = e; }
    if (b > maxBullets) { maxBullets = b; }
    if (p > maxPickups) { maxPickups = p; }
    if (m == 0) { scrambledAtStart = DenseOrderScrambled(registry); }
  }

  std::printf("\n%-18s %10s %10s %10s %10s\n", "step", "median", "mean", "min", "p95");
  std::printf("------------------------------------------------------------\n");
  double medianTotal = 0.0;
  for (std::size_t i = 0; i < kSteps; ++i) {
    const Summary s = Summarize(g_steps[i], frames);
    medianTotal += s.median;
    std::printf("%-18s %9.1fus %9.1fus %9.1fus %9.1fus\n",
                GLFD::Game::kSurvivorOrder[i].name, s.median, s.mean, s.min, s.p95);
  }
  const Summary frame = Summarize(g_frame, frames);
  std::printf("------------------------------------------------------------\n");
  std::printf("%-18s %9.1fus %9.1fus %9.1fus %9.1fus\n",
              "frame (measured)", frame.median, frame.mean, frame.min, frame.p95);
  std::printf("(sum of the per-step medians: %.1fus)\n", medianTotal);

  std::printf("\n%-18s %8s %8s %8s\n", "per frame", "median", "min", "max");
  for (int k = 0; k < kMetricCount; ++k) {
    const Summary s = Summarize(g_metrics[k], frames);
    std::printf("%-18s %8.0f %8.0f %8.0f\n", kMetricNames[k], s.median, s.min, s.max);
  }

  const GLFD::Game::SurvivorCounts& t = state.total;
  std::printf("\nalive peaks while measuring: enemies %zu / %u, bullets %zu / %u, experience %zu / %u\n",
              maxEnemies, params.maxEnemies, maxBullets, params.maxBullets, maxPickups, params.maxPickups);
  std::printf("dense order scrambled when measuring started: %s\n", scrambledAtStart ? "yes" : "NO");
  std::printf("totals (whole run): spawned %u fired %u hits %u kills %u collected %u "
              "xpExpired %u bulletsExpired %u reached %u\n",
              t.enemiesSpawned, t.bulletsFired, t.hitsDelivered, t.kills, t.pickupsCollected,
              t.pickupsExpired, t.bulletsExpired, t.enemiesReached);
  std::printf("problems while measuring: steps not ran %u, events dropped %u; whole run: "
              "grid mismatches %u, create failures %u, queue failures %u\n",
              notRan, droppedEvents, t.gridMismatches, t.createFailures, t.queueFailures);
  std::fflush(stdout);

  // 通常の終了で抜ける (2-4 で `JobSystem` の停止経路を直し、`std::_Exit` の回避を外した)
  return 0;
}
