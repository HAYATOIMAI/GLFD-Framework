/**
 * @file  JsonBenchmark.cpp
 * @brief JSON サブシステムの計測ツール(N-6)
 *
 * @details
 *  要件 N-6 は「具体的な数値目標は設定せず、**計測手段を用意する**」と定めている。
 *  これはその手段であり、**CI ではなく手動実行**するもの。
 *  絶対値の目標は無く、最適化の前後で比較できればよい。
 *
 *  ファイル名を `*Tests.cpp` にしていないのは、`build_and_run.bat` の
 *  自動探索(`Tests\*Tests.cpp`)に拾わせないため。
 *  実行は `Tests\run_benchmark.bat` から。
 *
 *  計測対象(代表的なパラメータファイル = `Resource/GameConfig.jsonc`):
 *   1. `Document::ParseFile`              — DOM の構築まで
 *   2. `Json::LoadFromJsonFile`           — パース + アーカイブ(実運用の経路)
 *   3. `Json::SaveToJson` (compact)       — セーブ経路
 *   4. `Json::SaveToJson` (pretty)        — エディタ出力経路
 *
 *  併せて `JsonArena` の `UsedBytes` / `AllocatedBytes` / `BlockCount` と、
 *  `IMemoryResource::Allocate` の呼び出し回数を出す(N-4 の確認にもなる)。
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/GameConfig.h"
#include "Core/Json/Json.h"

#include "RepositoryPath.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>

namespace {

  using Clock = std::chrono::steady_clock;

  /// 確保回数と総バイト数を数えるだけのリソース(失敗注入はしない)
  class CountingResource final : public GLFD::Memory::IMemoryResource {
  public:
    void* Allocate(size_t size, size_t alignment) override {
      ++m_allocateCalls;
      m_totalBytes += size;
      return _aligned_malloc(size, alignment);
    }
    void Deallocate(void* p, size_t, size_t) override {
      if (p != nullptr) { ++m_deallocateCalls; _aligned_free(p); }
    }

    [[nodiscard]] int    AllocateCalls()   const { return m_allocateCalls; }
    [[nodiscard]] int    DeallocateCalls() const { return m_deallocateCalls; }
    [[nodiscard]] size_t TotalBytes()      const { return m_totalBytes; }
    void Reset() { m_allocateCalls = 0; m_deallocateCalls = 0; m_totalBytes = 0; }

  private:
    int    m_allocateCalls   = 0;
    int    m_deallocateCalls = 0;
    size_t m_totalBytes      = 0;
  };

  /// 計測対象の設定ファイル。パスの解決規則は `RepositoryPath.h` を参照
  const char* ConfigPath() {
    static char path[1024];
    static bool initialized = false;
    if (!initialized) {
      GLFD::Test::RepositoryFile(path, sizeof(path), "Resource\\GameConfig.jsonc");
      initialized = true;
    }
    return path;
  }

  void PrintRow(const char* label, double totalMs, int iterations) {
    std::printf("  %-34s %9.3f ms total   %8.3f us/op\n",
                label, totalMs, (totalMs * 1000.0) / iterations);
  }

  void PrintArena(const char* label, const GLFD::Json::JsonArena& arena) {
    std::printf("  %-34s used=%zu  allocated=%zu  blocks=%zu\n",
                label, arena.UsedBytes(), arena.AllocatedBytes(), arena.BlockCount());
  }

}

int main(int argc, char** argv) {
  int iterations = 2000;
  if (argc >= 2) {
    const int parsed = std::atoi(argv[1]);
    if (parsed > 0) { iterations = parsed; }
  }

  std::printf("=== GLFD JSON benchmark (N-6) ===\n");
#ifdef NDEBUG
  std::printf("configuration: Release / NDEBUG\n");
#else
  std::printf("configuration: Debug   <-- Debug の数字は最適化前後の比較にしか使えない\n");
#endif
  std::printf("file: %s\n", ConfigPath());
  std::printf("iterations: %d\n\n", iterations);

  CountingResource resource;

  // --- 入力サイズの確認も兼ねて 1 回読む -----------------------------------
  {
    GLFD::Json::Document doc(&resource);
    if (!doc.ParseFile(ConfigPath(), GLFD::Json::ParseFlags::JsonC).IsOk()) {
      std::printf("[ERROR] could not read the config file\n");
      return 1;
    }
    std::printf("root members: %u\n\n", doc.Root().MemberCount());
  }

  // --- 1. ParseFile ---------------------------------------------------------
  {
    resource.Reset();
    GLFD::Json::Document doc(&resource);
    const int            warmup = 1;
    for (int i = 0; i < warmup; ++i) {
      doc.Clear();
      (void)doc.ParseFile(ConfigPath(), GLFD::Json::ParseFlags::JsonC);
    }
    const int allocationsAfterWarmup = resource.AllocateCalls();

    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
      doc.Clear();
      (void)doc.ParseFile(ConfigPath(), GLFD::Json::ParseFlags::JsonC);
    }
    const auto finish = Clock::now();

    PrintRow("Document::ParseFile",
             std::chrono::duration<double, std::milli>(finish - start).count(), iterations);
    PrintArena("  arena after ParseFile", doc.Arena());
    // 出力は ASCII のみにする。printf の宛先はコンソール(cp932)なので、
    // ソースが UTF-8 だと日本語が文字化けする
    std::printf("  %-34s %d (warmup %d) <- must not grow for the same Document\n\n",
                "  IMemoryResource::Allocate",
                resource.AllocateCalls(), allocationsAfterWarmup);
  }

  // --- 2. LoadFromJsonFile (実運用の経路) -----------------------------------
  {
    resource.Reset();
    GLFD::Json::Document       doc(&resource);
    GLFD::Json::ArchiveContext ctx(doc.Arena());
    GLFD::GameConfig           config(&resource);

    (void)GLFD::Json::LoadFromJsonFile(config, ConfigPath(), doc, ctx);
    const int allocationsAfterWarmup = resource.AllocateCalls();

    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
      (void)GLFD::Json::LoadFromJsonFile(config, ConfigPath(), doc, ctx);
    }
    const auto finish = Clock::now();

    PrintRow("Json::LoadFromJsonFile",
             std::chrono::duration<double, std::milli>(finish - start).count(), iterations);
    PrintArena("  arena after LoadFromJsonFile", doc.Arena());
    std::printf("  %-34s %d (warmup %d)\n\n", "  IMemoryResource::Allocate",
                resource.AllocateCalls(), allocationsAfterWarmup);
  }

  // --- 3 / 4. SaveToJson ----------------------------------------------------
  for (int pass = 0; pass < 2; ++pass) {
    const bool  pretty = (pass == 1);
    const char* label  = pretty ? "Json::SaveToJson (pretty)" : "Json::SaveToJson (compact)";

    resource.Reset();
    GLFD::Json::Document doc(&resource);
    GLFD::GameConfig     config(&resource);
    if (!GLFD::Json::LoadFromJsonFile(config, ConfigPath(), doc)) {
      std::printf("[ERROR] could not load the config for the save benchmark\n");
      return 1;
    }

    GLFD::Json::JsonArena        arena(&resource);
    GLFD::Json::JsonStringBuffer buffer(arena);
    (void)GLFD::Json::SaveToJson(config, buffer, GLFD::kGameConfigVersion, pretty);
    const size_t outputSize = buffer.Size();

    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
      buffer.Clear();   // 容量は保持されるので伸長は初回だけ
      (void)GLFD::Json::SaveToJson(config, buffer, GLFD::kGameConfigVersion, pretty);
    }
    const auto finish = Clock::now();

    PrintRow(label, std::chrono::duration<double, std::milli>(finish - start).count(), iterations);
    std::printf("  %-34s %zu bytes\n", "  output size", outputSize);
    PrintArena("  output arena", arena);
    std::printf("\n");
  }

  std::printf("resource totals: allocate=%d deallocate=%d bytes=%zu\n",
              resource.AllocateCalls(), resource.DeallocateCalls(), resource.TotalBytes());
  return 0;
}
