/**
 * @file  EcsBenchmark.cpp
 * @brief ECS 1-5 の性能基準線 (§1-D)
 *
 * @details
 *  **書き換える前に取る。** `View` は最小セットを基準に反復して他のセットで
 *  存在を確認するため、**全エンティティが同じ4型を持つ Boid では遅くなり得る**。
 *  比較する相手が無いと、速くなったのか遅くなったのかが永久に分からない。
 *
 *  ## 何を測っているか
 *  `BoidDemoScene::OnUpdate` の連続する6行のうち、**ヘッドレスで呼べるもの**を
 *  同じ順序で回している。ウィンドウも DX11 も作らない。
 *
 *   1. `GridBuildSystem::Update`
 *   2. `InteractionSystem::Update` — **測っていない**。`InputSystem` と
 *      `SimpleWindow` の実体が要り、かつ左クリックしていなければ即 return する
 *   3. `BoidSystem::Update`
 *   4. `MovementSystem::Update`
 *   5. `ApplyWorldBounds` — シーンのメンバ関数なので**同じ処理を写してある**
 *      (下の @warning を参照)
 *   6. `CollisionSystem::Update`
 *   7. `Registry::ApplyCommands` (1-4)
 *
 *  @warning **`ApplyWorldBounds` はここにある写しである。** 1-5 で本体を書き
 *           換えたら、**この写しも同じ形に書き換えること。** 片方だけ直すと
 *           前後の比較が嘘になる。
 *
 *  ## 測り方
 *  `steady_clock` で 1 フレームずつ各段を挟む。先頭の数フレームは捨てる
 *  (プールの初期確保・キャッシュ・スレッドの起床が乗るため)。
 *  残りから**中央値・平均・最小・p95** を出す。**中央値を基準に読むこと。**
 *  平均は 1 回の外れ値で動く。
 *
 *  ## 絶対値の目標は無い
 *  JSON の N-6 と同じ位置づけで、**最適化の前後で比較できればよい**。
 *  値は機械と負荷で変わる。
 *
 *  usage: EcsBenchmark.exe [frames] [warmup] [--fail=...]     (既定: 300 30)
 *         **リポジトリのルートから実行すること**(Resource/GameConfig.jsonc を読む)
 *
 *  ## 定常状態は無い (2-5)
 *  群れは時間とともに固まり、衝突イベントが増え続ける(捨てる分 30 で約 680 件 /
 *  フレーム、3000 で 2200〜4000 件)。**数字は「何フレーム目を測ったか」で決まる。**
 *  比べるときは区間(warmup〜frames)をそろえること。
 *
 *  ## 基準線のための出力 (2-5)
 *  `@` で始まる行と、計測区間だけの平均使用コア数を出す(BenchMeasure.h)。
 *  `--fail=...` は `run_baseline.ps1` の歯の確認に使う。
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>

#include "Core/DoubleStackAllocator.h"
#include "Core/GameConfig.h"
#include "Core/GameConfigLoad.h"
#include "Core/GameContext.h"
#include "Core/StackAllocator.h"
#include "Core/StackResource.h"

#include "ECS/CommandBuffer.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/View.h"

#include "Events/EventBus.h"
#include "Events/Events.h"

#include "Game/BoidAgent.h"
#include "Game/BoidSystems.h"
#include "Game/GridBulidSystem.h"
#include "Game/SimdSystem.h"

#include "Physics/CollisionComponents.h"
#include "Physics/CollisionSystem.h"

#include "Threading/JobSystem.h"

#include "BenchMeasure.h"

namespace {

  using Clock = std::chrono::steady_clock;

  constexpr int kMaxFrames = 4096;
  constexpr int kStageCount = 6;

  const char* const kStageNames[kStageCount] = {
    "GridBuildSystem",
    "BoidSystem",
    "MovementSystem",
    "ApplyWorldBounds",
    "CollisionSystem",
    "ApplyCommands",
  };

  double g_samples[kStageCount][kMaxFrames];
  double g_frameTotal[kMaxFrames];
  double g_published[kMaxFrames];   ///< 1-7: 1 フレームの発行数
  double g_dropped[kMaxFrames];     ///< 1-7: 1 フレームの取りこぼし

  void SortAscending(double* values, int count) {
    // 挿入ソート。数百件なので十分で、<algorithm> を持ち込まずに済む
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
    s.median = values[count / 2];
    s.p95    = values[(count * 95) / 100];
    return s;
  }

  /**
   * @brief `BoidDemoScene::ApplyWorldBounds` の写し
   * @warning **本体を書き換えたらここも書き換えること**(ファイル冒頭の警告)
   */
  void ApplyWorldBounds(GLFD::ECS::Registry& registry, float bx, float by) {
    // **1-5 で本体と同じ形にした。** 片方だけ直すと前後の比較が嘘になる
    for (auto [entity, pos, vel] : registry.View<GLFD::Components::Position,
                                                 GLFD::Components::Velocity>()) {
      (void)entity;
      if (pos.x < -bx) { pos.x = -bx; vel.vx =  std::abs(vel.vx); }
      if (pos.x >  bx) { pos.x =  bx; vel.vx = -std::abs(vel.vx); }
      if (pos.y < -by) { pos.y = -by; vel.vy =  std::abs(vel.vy); }
      if (pos.y >  by) { pos.y =  by; vel.vy = -std::abs(vel.vy); }
    }
  }

  /// `BoidDemoScene::OnEnter` と**同じ手順**で作る(同じ乱数・同じ順序)
  size_t SpawnLikeTheScene(GLFD::ECS::Registry& registry, const GLFD::GameConfig& config) {
    std::mt19937 gen(12345);
    std::uniform_real_distribution<float> posDist(config.world.spawnRange[0],
                                                 config.world.spawnRange[1]);
    std::uniform_real_distribution<float> velDist(config.world.velocityRange[0],
                                                  config.world.velocityRange[1]);

    const size_t entityCount = (config.simulation.entityCount > 0)
                             ? static_cast<size_t>(config.simulation.entityCount) : 0u;
    const size_t profileCount = config.boidProfiles.GetSize();

    size_t created = 0;
    for (size_t i = 0; i < entityCount; ++i) {
      const GLFD::BoidProfile& profile = (profileCount != 0)
                                       ? config.boidProfiles[i % profileCount]
                                       : config.defaultBoid;

      const GLFD::ECS::Entity e = registry.CreateEntity();
      if (!e.IsValid()) { break; }

      const bool built =
             registry.AddComponent<GLFD::Components::Position>(
                 e, posDist(gen), posDist(gen), 0.0f, 0.0f) != nullptr
          && registry.AddComponent<GLFD::Components::Velocity>(
                 e, velDist(gen), velDist(gen), 0.0f, 0.0f) != nullptr
          && registry.AddComponent<GLFD::Components::Collider>(
                 e, config.world.colliderRadius) != nullptr
          && registry.AddComponent<GLFD::Components::BoidAgent>(
                 e, profile.viewRadius, profile.separationWeight,
                 profile.alignmentWeight, profile.cohesionWeight) != nullptr;

      if (!built) {
        registry.DestroyEntity(e);
        break;
      }
      ++created;
    }
    return created;
  }

}

int main(int argc, char** argv) {
  const GLFD::Bench::Args args = GLFD::Bench::ParseArgs(argc, argv);
  if (args.bad) { return 2; }
  int frames = GLFD::Bench::IntArg(args, 0, 300);
  int warmup = GLFD::Bench::IntArg(args, 1, 30);
  if (frames < 1)          { frames = 1; }
  if (frames > kMaxFrames) { frames = kMaxFrames; }
  if (warmup < 0)          { warmup = 0; }
  if (warmup >= frames)    { warmup = frames / 10; }

  GLFD::Memory::StackAllocator       mainStack(512u * 1024u * 1024u);
  GLFD::Memory::StackResource        globalResource(mainStack);
  GLFD::Memory::DoubleStackAllocator frameAllocator(64u * 1024u * 1024u);

  std::unique_ptr<GLFD::Json::Document> baseDoc;
  std::unique_ptr<GLFD::Json::Document> localDoc;
  std::unique_ptr<GLFD::GameConfig>     config;

  const bool loaded = GLFD::ReloadGameConfig(
      baseDoc, localDoc, config,
      GLFD::kGameConfigPath, GLFD::kGameConfigLocalPath,
      &globalResource,
      [](GLFD::ConfigLayer, const char*, const GLFD::Json::Document&,
         const GLFD::Json::ArchiveContext&) {});
  if (!loaded) {
    // **黙って既定値で走らない。** 基準線が別の設定で取られると比較が壊れる
    std::printf("[ERROR] could not load %s. run this from the repository root.\n",
                GLFD::kGameConfigPath);
    return 1;
  }

  GLFD::Thread::JobSystem  jobSystem;
  GLFD::ECS::Registry      registry(&globalResource);
  GLFD::ECS::CommandBuffer commands(&globalResource);
  GLFD::Events::EventBus   eventBus(&globalResource);
  eventBus.Register<GLFD::Events::CollisionEvent>();

  const size_t created = SpawnLikeTheScene(registry, *config);

  std::printf("=== ECS benchmark (baseline for 1-5) ===\n");
#ifdef NDEBUG
  std::printf("(configuration: Release / NDEBUG)\n");
#else
  std::printf("(configuration: Debug)\n");
#endif
  std::printf("entities: %zu (config asks for %d)  frames: %d  warmup: %d\n",
              created, config->simulation.entityCount, frames, warmup);
  std::printf("workers: %u\n",
              static_cast<unsigned>(GLFD::System::WorkerThreadCount()));
  std::printf("measured window: frames %d..%d (the flock keeps changing; there is no steady state)\n",
              warmup, frames - 1);

  std::printf("@meta bench=boid frames=%d warmup=%d measured=%d entities=%zu workers=%u\n",
              frames, warmup, frames - warmup, created,
              static_cast<unsigned>(GLFD::System::WorkerThreadCount()));
  GLFD::Bench::PrintCommonMeta(args);

  std::fflush(stdout);   // 出口で固まっても、ここまでは必ず見えるようにする

  const float bx = config->world.halfExtent[0];
  const float by = config->world.halfExtent[1];
  const float maxSpeed = config->simulation.maxSpeed;

  GLFD::Bench::CpuSample cpuBegin;
  for (int f = 0; f < frames; ++f) {
    // 2-5: **計測区間だけ**の CPU 時間を取る(助走と準備を含めない)
    if (f == warmup) { cpuBegin = GLFD::Bench::SampleCpu(); }
    if (f >= warmup) { GLFD::Bench::MaybeFail(args, f - warmup); }

    frameAllocator.SwapAndReset();
    GLFD::Memory::StackResource frameResource(frameAllocator.GetCurrent());

    GLFD::GameContext ctx{
        &globalResource,
        &frameResource,
        &jobSystem,
        &registry,
        &commands,
        &eventBus,
        nullptr,          // grid: GridBuildSystem が差し込む
        nullptr,          // window
        nullptr,          // input
        nullptr,          // fileManager
        0.016f,
        nullptr,          // renderer
        static_cast<float>(f) * 0.016f,
        nullptr,          // sceneManager
        nullptr           // resourceManager
    };

    const auto t0 = Clock::now();
    GLFD::Systems::GridBuildSystem::Update(ctx);
    const auto t1 = Clock::now();
    GLFD::Systems::BoidSystem::Update(ctx, maxSpeed);
    const auto t2 = Clock::now();
    GLFD::Systems::MovementSystem::Update(ctx);
    const auto t3 = Clock::now();
    ApplyWorldBounds(registry, bx, by);
    const auto t4 = Clock::now();
    GLFD::Systems::CollisionSystem::Update(ctx);
    const auto t5 = Clock::now();
    registry.ApplyCommands(commands);
    const auto t6 = Clock::now();

    eventBus.DispatchAll();

    // 1-7: **走査費用と発行数は別の話**なので分けて出せるようにする
    {
      const GLFD::Events::BusCounters c = eventBus.Counters();
      g_published[f] = static_cast<double>(c.published);
      g_dropped[f]   = static_cast<double>(c.dropped);
      eventBus.ResetCounters();
    }

    const auto us = [](Clock::time_point a, Clock::time_point b) {
      return std::chrono::duration<double, std::micro>(b - a).count();
    };

    g_samples[0][f] = us(t0, t1);
    g_samples[1][f] = us(t1, t2);
    g_samples[2][f] = us(t2, t3);
    g_samples[3][f] = us(t3, t4);
    g_samples[4][f] = us(t4, t5);
    g_samples[5][f] = us(t5, t6);
    g_frameTotal[f] = us(t0, t6);
  }
  const GLFD::Bench::CpuWindow cpu = GLFD::Bench::Diff(cpuBegin, GLFD::Bench::SampleCpu());

  const int measured = frames - warmup;

  std::printf("\n%-18s %10s %10s %10s %10s\n", "stage", "median", "mean", "min", "p95");
  std::printf("------------------------------------------------------------\n");

  double medianTotal = 0.0;
  for (int s = 0; s < kStageCount; ++s) {
    const Summary summary = Summarize(g_samples[s] + warmup, measured);
    medianTotal += summary.median;
    std::printf("%-18s %9.1fus %9.1fus %9.1fus %9.1fus\n",
                kStageNames[s], summary.median, summary.mean, summary.min, summary.p95);
    GLFD::Bench::PrintRow("stage", kStageNames[s],
                          summary.median, summary.mean, summary.min, summary.p95);
  }

  const Summary total = Summarize(g_frameTotal + warmup, measured);
  std::printf("------------------------------------------------------------\n");
  std::printf("%-18s %9.1fus %9.1fus %9.1fus %9.1fus\n",
              "frame (measured)", total.median, total.mean, total.min, total.p95);
  std::printf("(sum of the per-stage medians: %.1fus)\n", medianTotal);
  GLFD::Bench::PrintRow("frame", "frame", total.median, total.mean, total.min, total.p95);
  const Summary pub  = Summarize(g_published + warmup, measured);
  const Summary drop = Summarize(g_dropped + warmup, measured);
  std::printf("\ncollision events published per frame: median %.0f  min %.0f  p95 %.0f\n",
              pub.median, pub.min, pub.p95);
  std::printf("collision events dropped   per frame: median %.0f  p95 %.0f\n",
              drop.median, drop.p95);
  GLFD::Bench::PrintRow("count", "events_published", pub.median, pub.mean, pub.min, pub.p95);
  GLFD::Bench::PrintRow("count", "events_dropped", drop.median, drop.mean, drop.min, drop.p95);

  std::printf("\naverage cores used while measuring: %.2f (process times)  %.2f (cycles, approx)"
              "  over %.3fs\n", cpu.coresByTimes, cpu.coresByCycles, cpu.wallSec);
  GLFD::Bench::PrintCpu(cpu);

  std::printf("\nnote: rendering, input and config reloading are NOT included.\n");
  std::fflush(stdout);

  // 通常の終了で抜ける。1-5 から 2-3 までは、ここで `std::_Exit(0)` を使って
  // `~JobSystem` の停止のハングを避けていた。2-4 で停止経路を直したので回避を外した。
  // **このツールが自分で終わること自体が、本番の停止経路が直ったことの確認になる**
  GLFD::Bench::PrintEnd();
  return GLFD::Bench::ExitCode(args);
}
