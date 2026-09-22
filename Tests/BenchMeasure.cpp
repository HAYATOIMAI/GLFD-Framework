/**
 * @file  BenchMeasure.cpp
 * @brief BenchMeasure.h の実装。<windows.h> はこの翻訳単位に閉じ込める
 */

#include "BenchMeasure.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace GLFD::Bench {

  namespace {
    std::uint64_t ToU64(const FILETIME& ft) {
      return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }
  }

  CpuSample SampleCpu() {
    CpuSample s;
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user);
    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    s.tsc           = __rdtsc();
    s.processTime   = ToU64(kernel) + ToU64(user);
    s.processCycles = cycles;
    s.wallSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return s;
  }

  CpuWindow Diff(const CpuSample& b, const CpuSample& e) {
    CpuWindow w;
    w.wallSec = e.wallSec - b.wallSec;
    w.cpuSec  = static_cast<double>(e.processTime - b.processTime) * 1e-7;
    if (w.wallSec > 0.0) { w.coresByTimes = w.cpuSec / w.wallSec; }
    const std::uint64_t tsc = e.tsc - b.tsc;
    if (tsc > 0) {
      w.coresByCycles = static_cast<double>(e.processCycles - b.processCycles)
                      / static_cast<double>(tsc);
    }
    return w;
  }

  Topology ReadTopology() {
    Topology t;
    t.hardwareConcurrency = std::thread::hardware_concurrency();

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) { return t; }
    std::vector<unsigned char> buf(len);
    auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &len)) { return t; }

    unsigned cores[256] = {}, logical[256] = {};
    int maxClass = 0;
    for (DWORD off = 0; off < len;) {
      auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
      const int cls = info->Processor.EfficiencyClass;
      unsigned n = 0;
      for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
        for (KAFFINITY m = info->Processor.GroupMask[g].Mask; m; m &= m - 1) { ++n; }
      }
      ++cores[cls];
      logical[cls] += n;
      if (cls > maxClass) { maxClass = cls; }
      off += info->Size;
    }
    for (int c = 0; c <= maxClass; ++c) {
      t.physicalCores     += cores[c];
      t.logicalProcessors += logical[c];
      if (c == maxClass) { t.performanceCores += cores[c]; t.performanceLogical += logical[c]; }
      else               { t.efficiencyCores  += cores[c]; t.efficiencyLogical  += logical[c]; }
    }
    return t;
  }

  Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
      const char* s = argv[i];
      if (std::strncmp(s, "--", 2) != 0) {
        if (a.positionalCount < 8) { a.positional[a.positionalCount++] = s; }
        continue;
      }
      if      (std::strcmp(s, "--fail=crash")    == 0) { a.fail = Fail::Crash;    a.failName = "crash"; }
      else if (std::strcmp(s, "--fail=hang")     == 0) { a.fail = Fail::Hang;     a.failName = "hang"; }
      else if (std::strcmp(s, "--fail=noend")    == 0) { a.fail = Fail::NoEnd;    a.failName = "noend"; }
      else if (std::strcmp(s, "--fail=exitcode") == 0) { a.fail = Fail::ExitCode; a.failName = "exitcode"; }
      else if (std::strncmp(s, "--fail-at=", 10) == 0) { a.failAt = std::atoi(s + 10); }
      else { a.bad = true; std::printf("[ERROR] unknown option: %s\n", s); }
    }
    return a;
  }

  int IntArg(const Args& args, int i, int fallback) {
    return (i < args.positionalCount) ? std::atoi(args.positional[i]) : fallback;
  }

  void MaybeFail(const Args& args, int measuredFrame) {
    if (measuredFrame != args.failAt) { return; }
    switch (args.fail) {
      case Fail::Crash:
        std::printf("[INJECTED] crash at measured frame %d\n", measuredFrame);
        std::fflush(stdout);
        std::abort();
      case Fail::Hang:
        std::printf("[INJECTED] hang at measured frame %d\n", measuredFrame);
        std::fflush(stdout);
        for (;;) { Sleep(1000); }
      case Fail::NoEnd:
        std::printf("[INJECTED] leaving with exit code 0 at measured frame %d\n", measuredFrame);
        std::fflush(stdout);
        std::_Exit(0);   // 故意に `@end` を出さずに抜ける(停止経路は通さない)
      default:
        return;
    }
  }

  int ExitCode(const Args& args) {
    return (args.fail == Fail::ExitCode) ? 7 : 0;
  }

  void PrintCommonMeta(const Args& args) {
    const Topology t = ReadTopology();
    std::printf("@meta hardware_concurrency=%u physical_cores=%u logical_processors=%u "
                "p_cores=%u p_logical=%u e_cores=%u e_logical=%u\n",
                t.hardwareConcurrency, t.physicalCores, t.logicalProcessors,
                t.performanceCores, t.performanceLogical, t.efficiencyCores, t.efficiencyLogical);
    std::printf("@meta fail=%s fail_at=%d\n", args.failName, args.failAt);
    std::printf("@meta msc_full_ver=%d\n", _MSC_FULL_VER);
#ifdef NDEBUG
    std::printf("@meta configuration=Release\n");
#else
    std::printf("@meta configuration=Debug\n");
#endif
  }

  void PrintCpu(const CpuWindow& w) {
    std::printf("@cpu wall_s=%.6f cpu_s=%.6f cores_by_times=%.3f cores_by_cycles=%.3f\n",
                w.wallSec, w.cpuSec, w.coresByTimes, w.coresByCycles);
  }

  void PrintRow(const char* kind, const char* name, double median, double mean,
                double min, double p95) {
    std::printf("@%s name=%s median=%.2f mean=%.2f min=%.2f p95=%.2f\n",
                kind, name, median, mean, min, p95);
  }

  void PrintEnd() {
    std::printf("@end\n");
    std::fflush(stdout);
  }

}
