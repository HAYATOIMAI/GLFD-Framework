/**
 * @file  JobSystemStopTests.cpp
 * @brief ECS 2-4: JobSystem の停止経路 — T-ECS-30 / T-ECS-31
 *
 * @details
 *  ## このスイートは自分で期限を持つ
 *  **テストランナー (build_and_run.bat) には時間上限が無い。** 停止のハングをこの
 *  プロセスの中で起こすと、スイートごと止まって「終わらない」になり、失敗として
 *  数えられない (開発手法 4.4)。そこで**止まり得るものはすべて子プロセスで回す**:
 *    - T-ECS-30: JobSystemStopHarness.h の親子構成。子が試行ごとにウォッチドッグを持ち、
 *      期限切れを hang として記録してプロセスを終わらせる。子が terminate / abort で
 *      落ちたら crash として数える
 *    - T-ECS-31 と死亡テスト: `--case <name>` で子を起こし、終了コードで結果を受け取る。
 *      期限が来たら親が子を終わらせ、失敗として数える
 *  **修正を戻した変異では、このスイートはハングせずに失敗を返して終わる。**
 *
 *  ## 本番の経路を呼ぶ (4.7)
 *  JobSystem は本物 (Source/Threading/JobSystem.cpp) を、差し込み点を有効にして
 *  (/DGLFD_JOBSYSTEM_PROBE) リンクしている。差し込み点は観測と窓を広げるためだけに使う。
 *
 *  ## 固定すること
 *  - **~JobSystem は必ず Stop() を呼ぶ。** 試行はすべて Stop() を呼ばずに delete する。
 *    呼ばなければ、修正前はハング、修正後は terminate (std::thread) になり、どちらも失敗になる
 *  - 停止後の KickJob は積まずに数える / 停止時に残ったジョブは実行せずに数え、
 *    親ハンドルの件数を戻す (§6 論点5)
 *  - KickJob / Stop の呼び出し元の検査 (Debug の assert)
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include "JobSystemStopHarness.h"
#include "TestHarness.h"

namespace {
  using namespace GLFD::Test::JobStop;
  using GLFD::Thread::JobCounter;
  using GLFD::Thread::JobHandle;

  // 1000 回の比較は単体のハーネス (run_jobsystem_stop_harness.bat) で行う。ここは毎回回す分
  int g_trials = 200;
  constexpr int kTimeoutMs = 200;        // 対照で測った正常な停止の最大 1.975 ms の約 100 倍
  constexpr int kCaseTimeoutMs = 10000;

  // =========================================================================
  //  子プロセス側: `--case <name>`。戻り値は失敗した確認の件数 (0 = 合格)
  // =========================================================================
  int g_caseFailures = 0;

#define CASE_EXPECT(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      std::printf("    [child FAIL] %s(%d): %s\n", __FILE__, __LINE__, #expr);   \
      std::fflush(stdout);                                                       \
      ++g_caseFailures;                                                          \
    }                                                                            \
  } while (0)

  // 停止後の KickJob は積まない / Stop() の 2 回目は何もしない
  int CaseKickAfterStop() {
    ResetProbes();
    JobSystem js(3);
    js.Stop();
    js.Stop();                                              // 2 回目: 何もしない
    CASE_EXPECT(Hits(Site::StopAfterNotify) == 1);          // 起こしたのは 1 回だけ
    CASE_EXPECT(Hits(Site::WorkerExit) == 3);               // 1 回目で全員を join した

    std::atomic<int> ran{ 0 };
    JobCounter counter;
    JobHandle handle = js.CreateHandle(counter);
    for (int i = 0; i < 3; ++i) {                           // 1 件だと「数えた」と「偶然 1」が区別できない
      js.KickJob([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, &handle);
    }
    CASE_EXPECT(ran.load() == 0);                           // 実行されていない
    CASE_EXPECT(!handle.IsBusy());                          // 親ハンドルの件数を増やしていない
    CASE_EXPECT(js.Stats().rejectedAfterStop == 3);
    CASE_EXPECT(js.Stats().abandonedAtStop == 0);
    js.WaitFor(handle);                                     // 戻ること(戻らなければ親の期限切れ)
    return g_caseFailures;
  }

  // join の後にキューに残ったジョブは実行しない。親ハンドルの件数を戻して数える
  int CaseAbandonedAtStop() {
    ResetProbes();
    JobSystem js(1);                                        // 1 本: 残りが確実にキューに溜まる
    JobCounter counter;
    JobHandle handle = js.CreateHandle(counter);
    std::atomic<bool> blockerStarted{ false };
    std::atomic<bool> blockerTimedOut{ false };
    std::atomic<int>  othersRan{ 0 };

    // 唯一のワーカーを、Stop() がワーカーを起こし終えるまで塞ぐ。
    // 塞いでいる間に積んだジョブは、ワーカーが次に停止を見るので取り出されない
    js.KickJob([&] {
      blockerStarted.store(true, std::memory_order_release);
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      while (Hits(Site::StopAfterNotify) == 0) {
        if (Clock::now() > deadline) { blockerTimedOut.store(true); break; }
        std::this_thread::yield();
      }
    }, &handle);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!blockerStarted.load(std::memory_order_acquire) && Clock::now() < deadline) std::this_thread::yield();
    CASE_EXPECT(blockerStarted.load());

    constexpr int kQueued = 5;
    for (int i = 0; i < kQueued; ++i) {
      js.KickJob([&othersRan] { othersRan.fetch_add(1, std::memory_order_relaxed); }, &handle);
    }
    CASE_EXPECT(handle.IsBusy());

    js.Stop();
    CASE_EXPECT(!blockerTimedOut.load());
    CASE_EXPECT(othersRan.load() == 0);                     // 残ったジョブは実行していない
    CASE_EXPECT(js.Stats().abandonedAtStop == static_cast<std::uint32_t>(kQueued));
    CASE_EXPECT(js.Stats().rejectedAfterStop == 0);
    CASE_EXPECT(!handle.IsBusy());                          // 1 (実行) + 5 (回収) で 0 に戻った
    js.WaitFor(handle);                                     // 戻ること
    return g_caseFailures;
  }

  // キューが満杯で積めなかったジョブは捨てられ、数えられ、親ハンドルの件数に残らない。
  // 以前は std::cerr に 1 行書くだけで、Game.log にも件数にも残らなかった (手順5)
  int CaseQueueFullIsCounted() {
    JobSystem js(1);                                        // 1 本を塞げば、キューは容量ちょうどまで溜まる
    JobCounter counter;
    JobHandle handle = js.CreateHandle(counter);
    std::atomic<bool> blockerStarted{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> blockerTimedOut{ false };
    std::atomic<int>  ran{ 0 };

    js.KickJob([&] {
      blockerStarted.store(true, std::memory_order_release);
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      while (!release.load(std::memory_order_acquire)) {
        if (Clock::now() > deadline) { blockerTimedOut.store(true); break; }
        std::this_thread::yield();
      }
    }, &handle);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!blockerStarted.load(std::memory_order_acquire) && Clock::now() < deadline) std::this_thread::yield();
    CASE_EXPECT(blockerStarted.load());                     // 塞ぐジョブはキューから出ている

    // 容量を 3 件超えて積む(1 件だと「数えた」と「偶然 1」が区別できない)
    constexpr int kOver = 3;
    constexpr int kKicks = static_cast<int>(JobSystem::QUEUE_CAPACITY) + kOver;
    for (int i = 0; i < kKicks; ++i) {
      js.KickJob([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, &handle);
    }
    CASE_EXPECT(js.Stats().queueFullDrops == static_cast<std::uint32_t>(kOver));
    CASE_EXPECT(js.Stats().rejectedAfterStop == 0);

    release.store(true, std::memory_order_release);
    js.WaitFor(handle);                                     // 捨てた分が件数に残っていれば戻らない
    CASE_EXPECT(!blockerTimedOut.load());
    CASE_EXPECT(ran.load() == static_cast<int>(JobSystem::QUEUE_CAPACITY));
    js.Stop();
    CASE_EXPECT(js.Stats().abandonedAtStop == 0);
    return g_caseFailures;
  }

  // ~JobSystem は Stop() を呼ぶ(明示的に呼ばずに破棄しても全員が抜ける)
  int CaseDestructorStops() {
    ResetProbes();
    JobSystem* js = new JobSystem(4);
    std::atomic<int> ran{ 0 };
    JobCounter counter;
    JobHandle handle = js->CreateHandle(counter);
    for (int i = 0; i < 7; ++i) {
      js->KickJob([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, &handle);
    }
    js->WaitFor(handle);
    delete js;                                              // Stop() を呼んでいない
    CASE_EXPECT(ran.load() == 7);
    CASE_EXPECT(Hits(Site::StopAfterNotify) == 1);          // デストラクタが Stop() を通った
    CASE_EXPECT(Hits(Site::WorkerExit) == 4);               // 全員が抜けた(join された)
    return g_caseFailures;
  }

  // 死亡テスト: 所有スレッドでもワーカーでもないスレッドからの KickJob は assert で止まる
  int CaseKickFromForeignThread() {
    JobSystem js(2);
    std::thread foreign([&js] { js.KickJob([] {}); });
    foreign.join();
    js.Stop();
    return 0;                                               // ここに来たら assert が無い
  }

  // 死亡テスト: 所有スレッド以外からの Stop() は assert で止まる
  int CaseStopFromForeignThread() {
    JobSystem js(2);
    std::thread foreign([&js] { js.Stop(); });
    foreign.join();
    return 0;
  }

  int RunCase(const char* name) {
    if (std::strcmp(name, "kick_after_stop") == 0)          return CaseKickAfterStop();
    if (std::strcmp(name, "abandoned_at_stop") == 0)        return CaseAbandonedAtStop();
    if (std::strcmp(name, "queue_full_is_counted") == 0)    return CaseQueueFullIsCounted();
    if (std::strcmp(name, "destructor_stops") == 0)         return CaseDestructorStops();
    if (std::strcmp(name, "kick_from_foreign_thread") == 0) return CaseKickFromForeignThread();
    if (std::strcmp(name, "stop_from_foreign_thread") == 0) return CaseStopFromForeignThread();
    std::printf("    [child] unknown case %s\n", name);
    return 100;
  }

  // =========================================================================
  //  親側
  // =========================================================================

  // T-ECS-30: 停止の再現ハーネス。修正前は 89.8-100 % ハングした構成 (手順2)
  void TestStopNeverHangs(const char* scenario, const char* widen) {
    const Summary s = RunTrials(scenario, g_trials, kTimeoutMs, widen);
    PrintSummary(s, scenario, kTimeoutMs, widen);
    CHECK(s.trials == g_trials);
    CHECK(s.hangs == 0);
    CHECK(s.crashes == 0);                                  // terminate (Stop の呼び忘れ) もここ
    CHECK(s.ioErrors == 0);
    CHECK(s.oks == g_trials);
    CHECK(s.notAllExited == 0);                             // ~JobSystem が全員を join した
    CHECK(s.sumFail == 0);                                  // jobs: 積んだジョブが全部実行された
    CHECK(s.setupFail == 0);                                // idle: 全員が眠ってから止めた
  }

  void CheckCase(const char* name, DWORD expectedExit) {
    const CaseResult r = RunCaseInChild(name, kCaseTimeoutMs);
    if (r.timedOut) std::printf("    case %s did not finish within %d ms\n", name, kCaseTimeoutMs);
    if (r.exitCode != expectedExit) {
      std::printf("    case %s exited with 0x%08lx, expected 0x%08lx\n", name, r.exitCode, expectedExit);
    }
    CHECK(!r.timedOut);
    CHECK(r.exitCode == expectedExit);
  }
}

int main(int argc, char** argv) {
  int childExit = 0;
  if (HandleChild(argc, argv, childExit)) return childExit;
  if (argc >= 3 && std::strcmp(argv[1], "--case") == 0) {
    ConfigureChildProcess();
    return RunCase(argv[2]);
  }
  // 変異テストで回数を減らすため(既定は 200)
  if (argc >= 3 && std::strcmp(argv[1], "--trials") == 0) g_trials = std::max(1, std::atoi(argv[2]));

  GLFD::Test::BeginSuite("JobSystemStop (ECS 2-4)");

  // ---- T-ECS-30 ----
  GLFD::Test::BeginCase("T-ECS-30 idle: stop while every worker sleeps");
  TestStopNeverHangs("idle", nullptr);
  GLFD::Test::BeginCase("T-ECS-30 immediate: stop right after construction");
  TestStopNeverHangs("immediate", nullptr);
  GLFD::Test::BeginCase("T-ECS-30 jobs: stop after a frame of jobs");
  TestStopNeverHangs("jobs", nullptr);
  GLFD::Test::BeginCase("T-ECS-30 H2 window: stop lands between the checks and wait (idle + wait:spin:2000)");
  TestStopNeverHangs("idle", "wait:spin:2000");
  GLFD::Test::BeginCase("T-ECS-30 H2 window with jobs (jobs + wait:spin:200)");
  TestStopNeverHangs("jobs", "wait:spin:200");
  GLFD::Test::BeginCase("T-ECS-30 widened Stop (immediate + notify:spin:2000)");
  TestStopNeverHangs("immediate", "notify:spin:2000");

  // ---- T-ECS-31 ----
  GLFD::Test::BeginCase("T-ECS-31 KickJob after Stop is rejected and counted; Stop twice is a no-op");
  CheckCase("kick_after_stop", 0);
  GLFD::Test::BeginCase("T-ECS-31 jobs left in the queue are not run, are counted, and release the handle");
  CheckCase("abandoned_at_stop", 0);
  GLFD::Test::BeginCase("T-ECS-31 a job the full queue cannot take is dropped, counted, and releases the handle");
  CheckCase("queue_full_is_counted", 0);
  GLFD::Test::BeginCase("~JobSystem calls Stop (no explicit Stop, every worker joined)");
  CheckCase("destructor_stops", 0);

  // ---- 呼び出し元の検査 (Debug の assert) ----
#if defined(_DEBUG)
  GLFD::Test::BeginCase("KickJob from a thread that is neither the owner nor a worker asserts");
  CheckCase("kick_from_foreign_thread", kAbortExit);
  GLFD::Test::BeginCase("Stop from a thread other than the owner asserts");
  CheckCase("stop_from_foreign_thread", kAbortExit);
#else
  std::printf("  (caller checks are Debug-only: assert is compiled out under NDEBUG)\n");
#endif

  return GLFD::Test::Summarize();
}
