/**
 * @file  ParallelForTests.cpp
 * @brief T-ECS-33: 分け方の規則と、積んだ塊が取り残されないこと (ECS 2-6)
 *
 * @details
 *  **本物の `Threading/ParallelFor.h` と本物の `JobSystem.cpp` を呼ぶ**(手順を写さない)。
 *
 *  ## 確かめること
 *   1. `PlanChunks` の本数の規則: min(上限, ceil(count / grain))。0 は 0 本
 *   2. `ChunkBegin` の範囲: 塊をつなぐと [0, count) をちょうど覆い、**空の塊が無く**、
 *      大きさの差は 1 以下
 *   3. `ParallelForChunks` で**各添字がちょうど 1 回**処理され、body の呼び出し回数が
 *      本数と一致し、空の塊が渡されない。`WaitFor` が戻る
 *   4. **1 本のときは積まない。** 停止済みの `JobSystem` で呼ぶ。もし `KickJob` を呼べば
 *      `rejectedAfterStop` が増え、body は実行されない。増えず、body が呼んだスレッドで
 *      1 回だけ動いたなら、積んでいない
 *   5. **窓を広げても取り残されない。** ワーカーが眠る直前(`WorkerBeforeWait`)で
 *      空回りさせ、確認と `wait` の間に積まれる形を増やしても、全添字がちょうど 1 回
 *
 *  ## 歯(変異で落ちることを確かめたもの。手順3 の報告を参照)
 *   - 古い分け方(本数 = 常に上限、最後の 1 本が余り)に戻すと 1 / 2 / 3 が落ちる
 *   - 1 本のときの直接呼び出しを消すと 4 が落ちる
 *
 *  このスイートは `GLFD_JOBSYSTEM_PROBE` を定義して建てる(build_and_run.bat)。
 *  `Probe::Hit` はここで定義する。窓を広げるのは 5 の間だけ。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "TestHarness.h"
#include "Threading/JobSystem.h"
#include "Threading/JobSystemProbe.h"
#include "Threading/ParallelFor.h"

// ---------------------------------------------------------------------------
//  差し込み点。5 のあいだだけ、ワーカーが眠る直前に空回りして窓を広げる
// ---------------------------------------------------------------------------
namespace {
  std::atomic<int> g_widenSpins{ 0 };
  std::atomic<long long> g_beforeWait{ 0 };
}

namespace GLFD::Thread::Probe {
  void Hit(Site site, std::size_t) noexcept {
    if (site != Site::WorkerBeforeWait) { return; }
    g_beforeWait.fetch_add(1, std::memory_order_relaxed);
    const int spins = g_widenSpins.load(std::memory_order_relaxed);
    for (int i = 0; i < spins; ++i) { std::this_thread::yield(); }
  }
}

namespace {

  using GLFD::Thread::ChunkBegin;
  using GLFD::Thread::JobSystem;
  using GLFD::Thread::ParallelForChunks;
  using GLFD::Thread::PlanChunks;

  constexpr std::size_t kMax = static_cast<std::size_t>(-1);

  // ------------------------------------------------------------------ 1
  void TestPlanChunks() {
    GLFD::Test::BeginCase("T-ECS-33a: PlanChunks = min(max, ceil(count / grain))");
    CHECK(PlanChunks(0, 1, 20).chunks == 0);       // 何もしない
    CHECK(PlanChunks(0, 64, 20).chunks == 0);
    CHECK(PlanChunks(1, 1, 20).chunks == 1);
    CHECK(PlanChunks(19, 1, 20).chunks == 19);     // 以前は 20 本(先頭 19 本が空)
    CHECK(PlanChunks(20, 1, 20).chunks == 20);
    CHECK(PlanChunks(21, 1, 20).chunks == 20);     // 上限で止まる
    CHECK(PlanChunks(137, 7, 20).chunks == 20);    // ceil(137/7) = 20
    CHECK(PlanChunks(137, 8, 20).chunks == 18);    // ceil(137/8) = 18(割り切れない)
    CHECK(PlanChunks(136, 8, 20).chunks == 17);    // 割り切れる
    CHECK(PlanChunks(352, 63, 20).chunks == 6);    // Survivor large の Hit と同じ形
    CHECK(PlanChunks(220, 3500, 20).chunks == 1);  // 粒度より少なければ 1 本
    CHECK(PlanChunks(5, 0, 20).chunks == 5);       // grain 0 は 1 として扱う
    CHECK(PlanChunks(5, 1, 0).chunks == 1);        // 上限 0 は 1 として扱う
    CHECK(PlanChunks(100, kMax, 20).chunks == 1);  // grain が巨大でも溢れない
    CHECK(PlanChunks(kMax, 1, 20).chunks == 20);   // count が巨大でも溢れない
    CHECK(PlanChunks(1000, 1, 3).chunks == 3);     // 上限が 20 でない場合
    // 上限を省くとワーカーの数
    CHECK(PlanChunks(1u << 20, 1).chunks == GLFD::System::WorkerThreadCount());
  }

  // ------------------------------------------------------------------ 2
  void TestChunkBegin() {
    GLFD::Test::BeginCase("T-ECS-33b: ChunkBegin covers [0, count) with no empty chunk (count 1..300, chunks 1..min(count, 23))");
    ++GLFD::Test::g_checkCount;
    int combos = 0;
    for (std::size_t count = 1; count <= 300; ++count) {
      const std::size_t maxChunks = (count < 23) ? count : 23;
      for (std::size_t chunks = 1; chunks <= maxChunks; ++chunks) {
        ++combos;
        CHECK_QUIET(ChunkBegin(count, chunks, 0) == 0);
        CHECK_QUIET(ChunkBegin(count, chunks, chunks) == count);
        std::size_t smallest = kMax, largest = 0;
        for (std::size_t i = 0; i < chunks; ++i) {
          const std::size_t b = ChunkBegin(count, chunks, i);
          const std::size_t e = ChunkBegin(count, chunks, i + 1);
          CHECK_QUIET(e > b);                       // 空の塊が無い
          const std::size_t size = e - b;
          if (size < smallest) { smallest = size; }
          if (size > largest) { largest = size; }
        }
        CHECK_QUIET(largest - smallest <= 1);       // 均等
      }
    }
    CHECK(combos > 5000);   // 回したことの確認(ループが空でない)
  }

  // ------------------------------------------------------------------ 3
  struct RunResult {
    bool eachOnce = true;
    bool noEmpty  = true;
    std::size_t calls = 0;
  };

  RunResult RunAndCount(JobSystem& js, std::size_t count, std::size_t grain) {
    std::vector<std::atomic<int>> hits(count + 1);   // +1: count が 0 でも確保する
    std::atomic<std::size_t> calls{ 0 };
    std::atomic<bool> empty{ false };
    ParallelForChunks(js, count, grain, [&](std::size_t start, std::size_t end) {
      calls.fetch_add(1, std::memory_order_relaxed);
      if (end <= start) { empty.store(true, std::memory_order_relaxed); }
      for (std::size_t i = start; i < end; ++i) { hits[i].fetch_add(1, std::memory_order_relaxed); }
    });
    RunResult r;
    for (std::size_t i = 0; i < count; ++i) {
      if (hits[i].load() != 1) { r.eachOnce = false; }
    }
    r.noEmpty = !empty.load();
    r.calls = calls.load();
    return r;
  }

  void TestEveryIndexOnce(JobSystem& js) {
    GLFD::Test::BeginCase("T-ECS-33c: every index exactly once, calls == chunks, no empty chunk (real JobSystem)");
    const std::size_t counts[] = { 0, 1, 7, 19, 20, 21, 28, 137, 352, 1000, 20000 };
    const std::size_t grains[] = { 1, 3, 63, 1100, 5000 };
    for (std::size_t count : counts) {
      for (std::size_t grain : grains) {
        const RunResult r = RunAndCount(js, count, grain);
        CHECK(r.eachOnce);
        CHECK(r.noEmpty);
        CHECK(r.calls == PlanChunks(count, grain).chunks);
      }
    }
  }

  // ------------------------------------------------------------------ 4
  void TestSingleChunkDoesNotKick() {
    GLFD::Test::BeginCase("T-ECS-33d: one chunk runs on the caller and never calls KickJob (stopped JobSystem)");
    JobSystem js(3);
    js.Stop();   // 以後の KickJob は積まれず rejectedAfterStop に数えられる
    const std::thread::id self = std::this_thread::get_id();
    int calls = 0;
    std::thread::id ranOn{};
    std::size_t gotStart = 99, gotEnd = 99;
    ParallelForChunks(js, 5, 5, [&](std::size_t start, std::size_t end) {   // ceil(5/5) = 1 本
      ++calls; ranOn = std::this_thread::get_id(); gotStart = start; gotEnd = end;
    });
    CHECK(calls == 1);
    CHECK(ranOn == self);
    CHECK(gotStart == 0 && gotEnd == 5);
    CHECK(js.Stats().rejectedAfterStop == 0);   // KickJob は 1 回も呼ばれていない

    // 0 件も積まない
    ParallelForChunks(js, 0, 1, [&](std::size_t, std::size_t) { ++calls; });
    CHECK(calls == 1);
    CHECK(js.Stats().rejectedAfterStop == 0);
  }

  // ------------------------------------------------------------------ 5
  void TestWidenedWindow(JobSystem& js) {
    GLFD::Test::BeginCase("T-ECS-33e: widened window before wait (yield x200 at WorkerBeforeWait), 500 rounds x 4 shapes");
    const long long before0 = g_beforeWait.load();
    g_widenSpins.store(200);
    ++GLFD::Test::g_checkCount;
    bool allOnce = true, allCalls = true, noEmpty = true;
    const std::size_t shapes[][2] = { { 20, 1 }, { 352, 63 }, { 1000, 1 }, { 21, 1 } };
    for (int round = 0; round < 500; ++round) {
      for (const auto& s : shapes) {
        const RunResult r = RunAndCount(js, s[0], s[1]);
        allOnce = allOnce && r.eachOnce;
        noEmpty = noEmpty && r.noEmpty;
        allCalls = allCalls && (r.calls == PlanChunks(s[0], s[1]).chunks);
      }
    }
    g_widenSpins.store(0);
    CHECK_QUIET(allOnce);
    CHECK_QUIET(allCalls);
    CHECK_QUIET(noEmpty);
    CHECK(g_beforeWait.load() > before0);   // 差し込み点を実際に通った(窓を広げた)
  }

}

int main() {
  GLFD::Test::BeginSuite("ParallelFor (ECS 2-6)");
  TestPlanChunks();
  TestChunkBegin();
  {
    JobSystem js;
    TestEveryIndexOnce(js);
    TestWidenedWindow(js);
    js.Stop();
  }
  TestSingleChunkDoesNotKick();
  return GLFD::Test::Summarize();
}
