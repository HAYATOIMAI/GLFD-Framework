// ---------------------------------------------------------------------------
//  JobSystem 停止の再現ハーネス(ECS 2-4 成果物 A)― 単体の実行ファイル
//
//  修正前後の比較(1000 回 × 構成)に使う。仕組みはすべて JobSystemStopHarness.h にあり、
//  T-ECS-30(JobSystemStopTests.cpp)と同じものを使う。
//
//  usage:
//    JobSystemStopHarness <scenario> <trials> <timeout_ms> [--widen site:kind:amount]
//      scenario : idle | immediate | jobs
//      site     : wait (WorkerBeforeWait) | notify (StopBeforeNotify)
//      kind     : yield (amount 回) | spin (amount マイクロ秒)
//    終了コード: ハング / crash / ジョブの欠落 / 記録失敗が 0 件なら 0、1 件以上なら 1、
//                使い方の誤りは 2
// ---------------------------------------------------------------------------
#include "JobSystemStopHarness.h"

namespace {
  void Usage() {
    std::printf("usage: JobSystemStopHarness <idle|immediate|jobs> <trials> <timeout_ms> [--widen wait|notify:yield|spin:N]\n");
  }
}

int main(int argc, char** argv) {
  using namespace GLFD::Test::JobStop;

  int childExit = 0;
  if (HandleChild(argc, argv, childExit)) return childExit;

  Scenario s;
  if (argc < 4 || !ParseScenario(argv[1], s)) { Usage(); return 2; }
  const int trials = std::atoi(argv[2]);
  const int timeoutMs = std::atoi(argv[3]);
  if (trials <= 0 || timeoutMs <= 0) { Usage(); return 2; }
  const char* widen = nullptr;
  if (argc >= 6 && std::strcmp(argv[4], "--widen") == 0) {
    if (!ParseWiden(argv[5])) { Usage(); return 2; }
    widen = argv[5];
  } else if (argc != 4) {
    Usage(); return 2;
  }

  const Summary summary = RunTrials(argv[1], trials, timeoutMs, widen);
  PrintSummary(summary, argv[1], timeoutMs, widen);
  return summary.Failed() ? 1 : 0;
}
