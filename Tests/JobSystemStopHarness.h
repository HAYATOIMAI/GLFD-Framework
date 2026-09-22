#pragma once
// ---------------------------------------------------------------------------
//  JobSystem 停止の再現ハーネス(ECS 2-4 成果物 A)― 共有部分
//
//  **本物の JobSystem を作って壊すだけ**を N 回繰り返し、停止のハングを数える。
//  使うのは2本: 単体のハーネス(JobSystemStopHarness.cpp、1000 回の証明用)と
//  T-ECS-30 のスイート(JobSystemStopTests.cpp)。**判定の仕組みは1つだけにする。**
//
//  構成:
//    親          … 子を起こし、子が書いた結果を集計する
//    子(--child)… 試行を順に回す。**試行ごとにウォッチドッグ**を持ち、時間内に
//                   停止が終わらなければ「hang」を1行書いてプロセスを終わらせる。
//                   **ハングしたスレッドは回収できない**ので、プロセスごと捨てる
//    親は子が終わったら、次の試行から新しい子を起こして続ける。
//    **ハングは「終わらなかった」ではなく「失敗1件」として数えられる。**
//    子が terminate / abort / アクセス違反で落ちたら crash として数える。
//    **だからテストランナーに時間上限が無くても、有限時間で失敗を返して終わる。**
//
//  観測(§0.2 ④): 競合の窓の中ではログを出さない。JobSystemProbe.h の差し込み点で
//  **原子カウンタを増やすだけ**にし、結果は試行が終わってから(ハング時は事後に)読む。
//
//  窓を広げる(§0.2 ②): widen = "<site>:<kind>:<amount>"
//    site : wait (WorkerBeforeWait) | notify (StopBeforeNotify)
//    kind : yield (amount 回) | spin (amount マイクロ秒の空回り)
//
//  **Probe::Hit の定義をここに持つ。** 1つの exe で1つの翻訳単位だけが include すること
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <crtdbg.h>

#include "Threading/JobSystem.h"
#include "Threading/JobSystemProbe.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <string>
#include <thread>
#include <vector>

#if !defined(GLFD_JOBSYSTEM_PROBE)
#error "the JobSystem stop harness must be built with /DGLFD_JOBSYSTEM_PROBE"
#endif

namespace GLFD::Test::JobStop {
  using GLFD::Thread::JobSystem;
  using GLFD::Thread::Probe::Site;
  using Clock = std::chrono::steady_clock;

  // ハーネス自身の終了コードは、**abort() (3) や例外コード (0xC000xxxx) と衝突しない値**にする。
  // 実際に踏んだ: 当初 kHangExit を 3 にしていたため、Stop() を呼び忘れた変異が terminate
  // (= abort = 3) で落ちると「ハング」と読まれ、しかもハングの行が無いので親が同じ試行を
  // 起こし直し続けた。**うるさい終了が、黙った無限ループに変わっていた**
  constexpr int kHangExit = 0x4A5301;      // 子のウォッチドッグがハングと判定した
  constexpr int kIoErrorExit = 0x4A5302;   // 結果ファイルに書けなかった(ハーネス自身の失敗。crash とは別に数える)
  constexpr int kBackstopExit = 0x4A5303;  // 親の保険が子を終わらせた
  constexpr int kSiteCount = static_cast<int>(Site::Count);

  // abort() の終了コード。assert の発火もこれになる(死亡テストが期待する値)
  constexpr DWORD kAbortExit = 3;

#if defined(_DEBUG)
  constexpr const char* kConfig = "Debug";
#else
  constexpr const char* kConfig = "Release";
#endif

  // ---- 差し込み点の状態(プロセス内だけ) --------------------------------------
  inline std::atomic<std::uint32_t> g_hits[kSiteCount];
  inline std::atomic<std::size_t>   g_stopValue{ 0 };   // StopBeforeNotify に渡された値

  enum class WidenKind { None, Yield, Spin };
  inline Site      g_widenSite = Site::Count;          // Count = どこも広げない
  inline WidenKind g_widenKind = WidenKind::None;
  inline int       g_widenAmount = 0;

  inline void ResetProbes() {
    for (auto& h : g_hits) h.store(0, std::memory_order_relaxed);
    g_stopValue.store(0, std::memory_order_relaxed);
  }

  inline std::uint32_t Hits(Site s) {
    return g_hits[static_cast<int>(s)].load(std::memory_order_acquire);
  }

  inline void WidenWindow() {
    if (g_widenKind == WidenKind::Yield) {
      for (int i = 0; i < g_widenAmount; ++i) std::this_thread::yield();
    } else if (g_widenKind == WidenKind::Spin) {
      const auto until = Clock::now() + std::chrono::microseconds(g_widenAmount);
      while (Clock::now() < until) { /* 空回り: sleep の粒度 (~1ms) より細かく広げる */ }
    }
  }

  // 結果ファイルは親(読む)と子(追記する)が同時に開く。**fopen_s は共有しないモードで
  // 開くため、親が読んでいる瞬間に子の追記が失敗する**(実際に踏んだ: 対照の実行で
  // 300 回中 1〜6 回、試行の結果が失われて crash と数えられた)。共有モードで開き、
  // それでも開けなければ少し待って開き直す。ここは試行の外なので待ってよい。
  // fopen / sscanf は C4996 (/WX で止まる) なので使わない
  //
  // **開き直すのは共有違反のときだけ。** ファイルが無いときに開き直すと、親が「子を
  // 起こす前の行数」を数えるつもりで最大 1 秒待ち、その間に子が書いた行まで数えてしまう
  // (実際に踏んだ: 2 番目の子の試行が「記録なし」と誤判定され、crash と数えられた)
  inline FILE* OpenFile(const char* path, const char* mode) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
      if (FILE* f = _fsopen(path, mode, _SH_DENYNO)) return f;
      if (errno == ENOENT) return nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return nullptr;
  }

  inline unsigned WorkerCount() {
    // JobSystem(0) と同じ決め方(JobSystem.cpp のコンストラクタ)
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
  }

  // 子プロセスでは、abort / assert / terminate がダイアログで止まらないようにする。
  // **ダイアログで止まった子は「ハング」に見える**ので、落ちたら即座に終わらせて
  // 親に crash(または死亡テストの期待値)として見せる
  inline void ConfigureChildProcess() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
  }

  inline std::string SelfPath() {
    char self[MAX_PATH]{};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    return self;
  }

  // 自分自身を引数付きで起こし、終わるか期限が来るまで待つ。期限が来たら終わらせる
  inline bool Spawn(const std::string& args, PROCESS_INFORMATION& pi) {
    const std::string cmd = "\"" + SelfPath() + "\" " + args;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back('\0');
    return CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0;
  }
}

// JobSystemProbe.h が宣言し、**ハーネスだけが定義する**
void GLFD::Thread::Probe::Hit(Site site, std::size_t value) noexcept {
  using namespace GLFD::Test::JobStop;
  g_hits[static_cast<int>(site)].fetch_add(1, std::memory_order_acq_rel);
  if (site == Site::StopBeforeNotify) g_stopValue.store(value, std::memory_order_relaxed);
  if (site == g_widenSite) WidenWindow();
}

namespace GLFD::Test::JobStop {
  // ---- 1 試行 ------------------------------------------------------------------
  enum class Scenario { Idle, Immediate, Jobs };

  struct TrialResult {
    bool      setupOk = true;    // idle: 全ワーカーが wait の手前に着いたか
    long long stopMicros = -1;   // 破棄(= Stop + join)にかかった時間
    bool      sumOk = true;      // jobs: 全ジョブが実行されたか
  };

  // 試行本体。**本番と同じ経路**: 既定の引数で作り、**Stop() を呼ばずに** delete する。
  // ~JobSystem が Stop() を呼ばなければ、ハング(修正前)か terminate(修正後)になる
  inline void RunTrial(Scenario scenario, unsigned workers, TrialResult& out) {
    JobSystem* js = new JobSystem();

    if (scenario == Scenario::Idle) {
      // 全ワーカーが眠ると決めて wait の手前まで来るのを待つ(準備。窓の外)
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      while (Hits(Site::WorkerBeforeWait) < workers) {
        if (Clock::now() > deadline) { out.setupOk = false; break; }
        std::this_thread::yield();
      }
    } else if (scenario == Scenario::Jobs) {
      // ゲームの1フレームに近い使い方: 親ハンドルに束ねて投入し、WaitFor で待つ
      constexpr int kJobs = 257;           // ワーカー数の倍数と偶然揃えない
      std::atomic<long long> sum{ 0 };
      GLFD::Thread::JobCounter counter;
      GLFD::Thread::JobHandle handle = js->CreateHandle(counter);
      for (int i = 1; i <= kJobs; ++i) {
        js->KickJob([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); }, &handle);
      }
      js->WaitFor(handle);
      out.sumOk = (sum.load() == static_cast<long long>(kJobs) * (kJobs + 1) / 2);
    }

    const auto t0 = Clock::now();
    delete js;
    const auto t1 = Clock::now();
    out.stopMicros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }

  inline bool ParseScenario(const char* s, Scenario& out) {
    if (std::strcmp(s, "idle") == 0)      { out = Scenario::Idle; return true; }
    if (std::strcmp(s, "immediate") == 0) { out = Scenario::Immediate; return true; }
    if (std::strcmp(s, "jobs") == 0)      { out = Scenario::Jobs; return true; }
    return false;
  }

  inline bool ParseWiden(const char* spec) {
    // site:kind:amount
    const char* c1 = std::strchr(spec, ':');
    const char* c2 = c1 ? std::strchr(c1 + 1, ':') : nullptr;
    if (!c2) return false;
    const std::string site(spec, c1), kind(c1 + 1, c2);
    const int amount = std::atoi(c2 + 1);
    if (amount <= 0) return false;
    if (site == "wait")        g_widenSite = Site::WorkerBeforeWait;
    else if (site == "notify") g_widenSite = Site::StopBeforeNotify;
    else return false;
    if (kind == "yield")      g_widenKind = WidenKind::Yield;
    else if (kind == "spin")  g_widenKind = WidenKind::Spin;
    else return false;
    g_widenAmount = amount;
    return true;
  }

  // ---- 子: 試行を順に回し、1 試行 1 行を結果ファイルへ ------------------------
  inline int RunChild(Scenario scenario, int first, int trials, int timeoutMs, const char* resultPath) {
    const unsigned workers = WorkerCount();
    for (int i = first; i < trials; ++i) {
      ResetProbes();
      TrialResult r;
      std::atomic<bool> done{ false };
      std::thread trial([&] { RunTrial(scenario, workers, r); done.store(true, std::memory_order_release); });

      // ウォッチドッグ。**試行スレッドの終了そのものを待つ**ので、終われば即座に戻る
      // (sleep でのポーリングは Windows のタイマー粒度で 1 試行 ~16ms かかっていた)
      const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(trial.native_handle()),
                                               static_cast<DWORD>(timeoutMs));
      if (waited != WAIT_OBJECT_0) {
        // **ハング。** 状態は事後に読む(差し込み点の原子カウンタ)。
        // 閾値が短すぎて「遅いだけの停止」を数えていないかを見るため、
        // **同じ時間だけ待ち直して、抜けたワーカーが増えないこと**も記録する
        const std::uint32_t exitedAtTimeout = Hits(Site::WorkerExit);
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        const std::uint32_t exitedAfterGrace = Hits(Site::WorkerExit);
        FILE* f = OpenFile(resultPath, "a");
        if (!f) return kIoErrorExit;
        std::fprintf(f, "hang %d stop_value=%zu before_wait=%u after_notify=%u exited=%u exited_after_grace=%u "
                        "done_after_grace=%d workers=%u setup=%d\n",
                     i, g_stopValue.load(), Hits(Site::WorkerBeforeWait), Hits(Site::StopAfterNotify),
                     exitedAtTimeout, exitedAfterGrace, done.load() ? 1 : 0, workers, r.setupOk ? 1 : 0);
        std::fclose(f);
        // ハングしたスレッドは join できない。**監視役としてプロセスを終わらせる**
        std::_Exit(kHangExit);
      }
      trial.join();
      FILE* f = OpenFile(resultPath, "a");
      if (!f) return kIoErrorExit;
      std::fprintf(f, "ok %d stop_us=%lld stop_value=%zu exited=%u workers=%u setup=%d sum=%d\n",
                   i, r.stopMicros, g_stopValue.load(), Hits(Site::WorkerExit), workers,
                   r.setupOk ? 1 : 0, r.sumOk ? 1 : 0);
      std::fclose(f);
    }
    return 0;
  }

  // 子として呼ばれたかを見て、そうなら試行を回す。
  // 引数: --child <scenario> <first> <trials> <timeout_ms> <result> [--widen spec]
  inline bool HandleChild(int argc, char** argv, int& exitCode) {
    if (argc < 7 || std::strcmp(argv[1], "--child") != 0) return false;
    ConfigureChildProcess();
    Scenario s;
    if (!ParseScenario(argv[2], s)) { exitCode = 2; return true; }
    if (argc >= 9 && std::strcmp(argv[7], "--widen") == 0 && !ParseWiden(argv[8])) { exitCode = 2; return true; }
    exitCode = RunChild(s, std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]), argv[6]);
    return true;
  }

  // ---- 親: 子を起こし続け、結果を集計する -----------------------------------
  inline int CountLines(const char* path) {
    FILE* f = OpenFile(path, "r");
    if (!f) return 0;
    int n = 0;
    for (int c; (c = std::fgetc(f)) != EOF;) if (c == '\n') ++n;
    std::fclose(f);
    return n;
  }

  inline long long Field(const char* line, const char* key) {
    const char* p = std::strstr(line, key);
    return p ? std::strtoll(p + std::strlen(key), nullptr, 10) : -1;
  }

  inline unsigned long long UField(const char* line, const char* key) {
    const char* p = std::strstr(line, key);
    return p ? std::strtoull(p + std::strlen(key), nullptr, 10) : 0;
  }

  struct Summary {
    int trials = 0, oks = 0, hangs = 0, crashes = 0, ioErrors = 0, backstops = 0, children = 0;
    int setupFail = 0, sumFail = 0;
    int notAllExited = 0;      // ok なのに、抜けたワーカーが全員ではなかった試行
    int graceProgress = 0;     // 待ち直したら進んだハング(= 閾値による誤判定の疑い)
    int stopValueZero = 0, stopValueNonzero = 0;
    long long stopMin = -1, stopP50 = -1, stopP99 = -1, stopMax = -1;
    long long stuckMin = -1, stuckMax = -1;
    double seconds = 0.0;
    std::string resultPath;

    [[nodiscard]] bool Failed() const { return hangs > 0 || crashes > 0 || ioErrors > 0 || sumFail > 0 || notAllExited > 0; }
  };

  inline void PrintSummary(const Summary& s, const char* scenario, int timeoutMs, const char* widen) {
    std::printf("=== JobSystemStopHarness  config=%s  scenario=%s  workers=%u  timeout=%d ms  widen=%s\n",
                kConfig, scenario, WorkerCount(), timeoutMs, widen ? widen : "none");
    std::printf("  trials      : %d  (children spawned: %d, %.1f s)\n", s.trials, s.children, s.seconds);
    std::printf("  HANG        : %d / %d  (%.1f %%)%s\n", s.hangs, s.trials,
                100.0 * s.hangs / std::max(1, s.trials), s.backstops ? "  [some caught by parent backstop]" : "");
    std::printf("  crash       : %d\n", s.crashes);
    if (s.ioErrors) std::printf("  [ERROR] harness could not record %d trial(s) - result unknown\n", s.ioErrors);
    std::printf("  ok          : %d\n", s.oks);
    std::printf("  stop time of ok trials (us): min %lld  p50 %lld  p99 %lld  max %lld\n",
                s.stopMin, s.stopP50, s.stopP99, s.stopMax);
    std::printf("  value seen at StopBeforeNotify: zero %d  nonzero %d  "
                "(before the fix: m_activeJobCount / after: wake generation)\n", s.stopValueZero, s.stopValueNonzero);
    std::printf("  workers still inside at a hang: min %lld  max %lld  (of %u)\n", s.stuckMin, s.stuckMax, WorkerCount());
    std::printf("  hangs that progressed after waiting another %d ms: %d / %d  (0 = none was merely slow)\n",
                timeoutMs, s.graceProgress, s.hangs - s.backstops);
    if (s.notAllExited) std::printf("  [ERROR] ok trials where not every worker exited: %d\n", s.notAllExited);
    if (s.setupFail) std::printf("  [WARN] idle setup did not see all workers reach wait: %d\n", s.setupFail);
    if (s.sumFail)   std::printf("  [ERROR] jobs lost (sum mismatch): %d\n", s.sumFail);
    std::printf("  raw results : %s\n", s.resultPath.c_str());
    std::fflush(stdout);
  }

  // scenario を trials 回。widen は nullptr で「広げない」
  inline Summary RunTrials(const char* scenario, int trials, int timeoutMs, const char* widen) {
    Summary s;
    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    static int serial = 0;
    s.resultPath = std::string(tmp) + "glfd_jobstop_" + std::to_string(GetCurrentProcessId()) + "_" +
                   std::to_string(++serial) + ".txt";
    std::remove(s.resultPath.c_str());
    // 空で作っておく。子を起こす前の行数は、最初の子でも確実に 0 と読める
    if (FILE* created = OpenFile(s.resultPath.c_str(), "w")) std::fclose(created);
    const std::string extra = widen ? std::string(" --widen ") + widen : std::string();

    const auto begin = Clock::now();
    for (int next = 0; next < trials;) {
      PROCESS_INFORMATION pi{};
      if (!Spawn(std::string("--child ") + scenario + " " + std::to_string(next) + " " + std::to_string(trials) + " " +
                 std::to_string(timeoutMs) + " \"" + s.resultPath + "\"" + extra, pi)) {
        std::printf("[ERROR] CreateProcess failed (%lu)\n", GetLastError());
        ++s.ioErrors;
        break;
      }
      ++s.children;
      const int linesBefore = CountLines(s.resultPath.c_str());
      // 子のウォッチドッグが効かなかった場合の保険: 進捗が止まったら親が終わらせる
      int seen = linesBefore;
      auto lastProgress = Clock::now();
      for (;;) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        const int now = CountLines(s.resultPath.c_str());
        if (now != seen) { seen = now; lastProgress = Clock::now(); }
        if (Clock::now() - lastProgress > std::chrono::milliseconds(2 * timeoutMs + 10000)) {
          TerminateProcess(pi.hProcess, kBackstopExit);
          WaitForSingleObject(pi.hProcess, INFINITE);
          FILE* f = OpenFile(s.resultPath.c_str(), "a");
          if (f) { std::fprintf(f, "hang %d backstop=1\n", CountLines(s.resultPath.c_str())); std::fclose(f); }
          ++s.backstops;
          break;
        }
      }
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);

      const int written = CountLines(s.resultPath.c_str());
      if (code == static_cast<DWORD>(kIoErrorExit)) {
        // ハーネスが結果を書けなかった。**試行の結果は分からない**ので、ok とも hang とも数えない
        FILE* f = OpenFile(s.resultPath.c_str(), "a");
        if (f) { std::fprintf(f, "ioerror %d\n", written); std::fclose(f); }
      } else if (code != 0 && code != static_cast<DWORD>(kHangExit) && code != static_cast<DWORD>(kBackstopExit)) {
        // **クラッシュも数える**(落ちた試行を ok と読まない)。terminate / abort はここに来る
        FILE* f = OpenFile(s.resultPath.c_str(), "a");
        if (f) { std::fprintf(f, "crash %d code=0x%08lx\n", written, code); std::fclose(f); }
      } else if (written == linesBefore) {
        // **終了コードが何であれ、1 行も書かずに終わった子はその試行の crash として数える。**
        // これが無いと親は同じ試行から子を起こし直し続け、有限時間で終わらない
        FILE* f = OpenFile(s.resultPath.c_str(), "a");
        if (f) { std::fprintf(f, "crash %d code=0x%08lx no_record=1\n", written, code); std::fclose(f); }
      }
      next = CountLines(s.resultPath.c_str());
    }
    s.seconds = std::chrono::duration<double>(Clock::now() - begin).count();

    // ---- 集計 ----
    std::vector<long long> stopUs;
    const unsigned workers = WorkerCount();
    FILE* f = OpenFile(s.resultPath.c_str(), "r");
    char line[512];
    while (f && std::fgets(line, sizeof line, f)) {
      if (std::strncmp(line, "crash ", 6) == 0)   { ++s.crashes; continue; }
      if (std::strncmp(line, "ioerror ", 8) == 0) { ++s.ioErrors; continue; }
      const bool isOk = std::strncmp(line, "ok ", 3) == 0;
      const bool isHang = std::strncmp(line, "hang ", 5) == 0;
      if (!isOk && !isHang) continue;
      if (Field(line, "setup=") == 0) ++s.setupFail;
      if (isOk) {
        ++s.oks;
        stopUs.push_back(Field(line, "stop_us="));
        if (Field(line, "sum=") == 0) ++s.sumFail;
        if (Field(line, "exited=") != static_cast<long long>(workers)) ++s.notAllExited;
      } else {
        ++s.hangs;
        if (Field(line, "backstop=") == 1) continue;
        const long long stuck = static_cast<long long>(workers) - Field(line, "exited=");
        s.stuckMin = s.stuckMin < 0 ? stuck : std::min(s.stuckMin, stuck);
        s.stuckMax = std::max(s.stuckMax, stuck);
        if (Field(line, "exited_after_grace=") != Field(line, "exited=") || Field(line, "done_after_grace=") == 1) {
          ++s.graceProgress;
        }
      }
      if (UField(line, "stop_value=") == 0) ++s.stopValueZero; else ++s.stopValueNonzero;
    }
    if (f) std::fclose(f);
    s.trials = s.oks + s.hangs + s.crashes + s.ioErrors;

    std::sort(stopUs.begin(), stopUs.end());
    auto pct = [&](double p) -> long long {
      if (stopUs.empty()) return -1;
      const size_t k = std::min(stopUs.size() - 1, static_cast<size_t>(p * static_cast<double>(stopUs.size())));
      return stopUs[k];
    };
    s.stopMin = pct(0.0); s.stopP50 = pct(0.5); s.stopP99 = pct(0.99);
    s.stopMax = stopUs.empty() ? -1 : stopUs.back();
    return s;
  }

  // ---- 1 つの検査を子プロセスで ------------------------------------------------
  // 子は `--case <name>` で起こされ、終了コードで結果を返す。期限が来たら親が終わらせる。
  // **止まり得る検査(WaitFor / join)を、スイートのプロセスの中で直接回さない**ため
  struct CaseResult {
    DWORD exitCode = 0;
    bool  timedOut = false;
  };

  inline CaseResult RunCaseInChild(const char* name, int timeoutMs) {
    CaseResult r;
    PROCESS_INFORMATION pi{};
    if (!Spawn(std::string("--case ") + name, pi)) { r.exitCode = 0xFFFFFFFFu; return r; }
    if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
      TerminateProcess(pi.hProcess, kBackstopExit);
      WaitForSingleObject(pi.hProcess, INFINITE);
      r.timedOut = true;
    }
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
  }
}
