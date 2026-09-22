/**
 * @file  JobWakeBenchmark.cpp
 * @brief ジョブを積んで起こす往復の費用 (ECS 2-5)
 *
 * @details
 *  2-4 で「`WaitFor` のたびにワーカーを起こす費用」は未計測だった(要件書 §24.9)。
 *  **本番の `JobSystem.cpp` をそのまま建てて**、空のジョブで往復だけを測る。エンジンは変えない。
 *
 *  ## 測るもの
 *   - **A** 1 本積み、メインは手伝わずに待つ。積んでからワーカーで走り始めるまでの時間も出す
 *   - **B** 1 本積んで `WaitFor`。メインが自分で実行した割合も出す
 *   - **C** 20 本積んで `WaitFor`。**積む部分(`KickJob` 20 回)と合計を分けて出す**
 *     (並列の段は実体の数に関係なく `WorkerThreadCount()` 本に分けて積むので、その形)
 *
 *  各回の前にメインが gap µs だけ空回りし、ワーカーが眠りに戻ってから測る。
 *
 *  ## 見立て(未確認)
 *  2-5 の試作では C の時間のほぼすべてが `KickJob` の 20 回だった。`KickJob` ごとの
 *  `notify_one` が原因だと見ているが、**確かめていない【推測】**。
 *
 *  usage: JobWakeBenchmark.exe [iterations] [gapUs] [--fail=...]   (既定: 2000 200)
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "Core/HardwareConstants.h"
#include "Threading/JobSystem.h"

#include "BenchMeasure.h"

namespace {

  using Clock = std::chrono::steady_clock;

  double Us(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

  void Spin(double us) {
    const auto end = Clock::now()
                   + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::micro>(us));
    while (Clock::now() < end) {}
  }

  void Report(const char* name, std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    double total = 0.0;
    for (double x : v) { total += x; }
    const double median = v[n / 2], mean = total / static_cast<double>(n);
    const double min = v[0], p95 = v[(n * 95) / 100];
    std::printf("%-30s median %7.2fus  mean %7.2fus  min %7.2fus  p95 %7.2fus  max %8.2fus\n",
                name, median, mean, min, p95, v[n - 1]);
    GLFD::Bench::PrintRow("wake", name, median, mean, min, p95);
  }

  void Ratio(const char* name, long long part, long long whole) {
    std::printf("%-30s %lld / %lld\n", name, part, whole);
    std::printf("@ratio name=%s part=%lld whole=%lld\n", name, part, whole);
  }

}

int main(int argc, char** argv) {
  const GLFD::Bench::Args args = GLFD::Bench::ParseArgs(argc, argv);
  if (args.bad) { return 2; }
  int iterations = GLFD::Bench::IntArg(args, 0, 2000);
  const int gapUs = GLFD::Bench::IntArg(args, 1, 200);
  if (iterations < 20) { iterations = 20; }
  constexpr int kChunks = 20;

  GLFD::Thread::JobSystem jobSystem;
  const auto mainId = std::this_thread::get_id();

  std::printf("=== job wake benchmark (ECS 2-5) ===\n");
  std::printf("workers: %u  iterations: %d  gap: %dus  chunks in C: %d\n",
              static_cast<unsigned>(GLFD::System::WorkerThreadCount()), iterations, gapUs, kChunks);
  std::printf("@meta bench=wake iterations=%d gap_us=%d chunks=%d workers=%u\n",
              iterations, gapUs, kChunks, static_cast<unsigned>(GLFD::System::WorkerThreadCount()));
  GLFD::Bench::PrintCommonMeta(args);
  std::fflush(stdout);

  // スレッドとキューを温める
  for (int i = 0; i < 200; ++i) {
    GLFD::Thread::JobCounter c;
    auto h = jobSystem.CreateHandle(c);
    jobSystem.KickJob([] {}, &h);
    jobSystem.WaitFor(h);
  }

  {  // A: 1 本積み、手伝わずに待つ
    std::vector<double> total, toStart;
    long long onMain = 0;
    for (int i = 0; i < iterations; ++i) {
      GLFD::Bench::MaybeFail(args, i);
      Spin(gapUs);
      GLFD::Thread::JobCounter c;
      auto h = jobSystem.CreateHandle(c);
      std::atomic<long long> started{ 0 };
      std::atomic<bool> ranOnMain{ false };
      const auto t0 = Clock::now();
      jobSystem.KickJob([&] {
        started.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        ranOnMain.store(std::this_thread::get_id() == mainId, std::memory_order_relaxed);
      }, &h);
      while (h.IsBusy()) {}
      const auto t1 = Clock::now();
      total.push_back(Us(t1 - t0));
      toStart.push_back(Us(Clock::duration(started.load()) - t0.time_since_epoch()));
      if (ranOnMain.load()) { ++onMain; }
    }
    Report("A_kick1_poll_total", total);
    Report("A_kick1_poll_to_start", toStart);
    Ratio("A_ran_on_main", onMain, iterations);
  }

  {  // B: 1 本積んで WaitFor
    std::vector<double> total;
    long long onMain = 0;
    for (int i = 0; i < iterations; ++i) {
      Spin(gapUs);
      GLFD::Thread::JobCounter c;
      auto h = jobSystem.CreateHandle(c);
      std::atomic<bool> ranOnMain{ false };
      const auto t0 = Clock::now();
      jobSystem.KickJob([&] {
        ranOnMain.store(std::this_thread::get_id() == mainId, std::memory_order_relaxed);
      }, &h);
      jobSystem.WaitFor(h);
      total.push_back(Us(Clock::now() - t0));
      if (ranOnMain.load()) { ++onMain; }
    }
    Report("B_kick1_waitfor", total);
    Ratio("B_ran_on_main", onMain, iterations);
  }

  {  // C: 20 本積んで WaitFor。積む部分を分ける
    std::vector<double> total, kicks;
    long long onMain = 0;
    for (int i = 0; i < iterations; ++i) {
      Spin(gapUs);
      GLFD::Thread::JobCounter c;
      auto h = jobSystem.CreateHandle(c);
      std::atomic<int> mainRan{ 0 };
      const auto t0 = Clock::now();
      for (int k = 0; k < kChunks; ++k) {
        jobSystem.KickJob([&] {
          if (std::this_thread::get_id() == mainId) { mainRan.fetch_add(1, std::memory_order_relaxed); }
        }, &h);
      }
      const auto t1 = Clock::now();
      jobSystem.WaitFor(h);
      const auto t2 = Clock::now();
      kicks.push_back(Us(t1 - t0));
      total.push_back(Us(t2 - t0));
      onMain += mainRan.load();
    }
    Report("C_kick20_waitfor_total", total);
    Report("C_kick20_kick_part", kicks);
    Ratio("C_ran_on_main", onMain, static_cast<long long>(iterations) * kChunks);
  }

  jobSystem.Stop();
  GLFD::Bench::PrintEnd();
  return GLFD::Bench::ExitCode(args);
}
