/**
 * @file  ConfigLogTests.cpp
 * @brief T-ECS-59: 設定の版の行は、読めたファイルについてだけ出る (ECS 2-16)
 *
 * @details
 *  2-15 までは、土台のファイルが見つからなくても、パースに失敗しても、`ConfigDiagnosticsLogger` が
 *  版 0 として `LogBaseVersion` を呼び、`Game.log` に次の行を並べていた(2-16 手順1 §0 の 3、実機で確認)。
 *
 *      [ERR ] Resource/GameConfig.jsonc: FileNotFound
 *      [WARN] Resource/GameConfig.jsonc: no $version; read as the oldest format
 *      [WARN] Resource/GameConfig.jsonc: migrated $version 0 -> 3 (...)
 *
 *  読めていないファイルを「移行した」と書き、`"$version": 1` と書いてあるファイルでも「$version が無い」と
 *  書いていた。どのファイルをどう読んだかを書く行が中身と食い違うのは、2-16 で直す形と同じ壊れ方。
 *
 *  本番と同じ `ReloadGameConfig` + `ConfigDiagnosticsLogger` を本物の `Logger` で呼び、書かれた行を
 *  読み戻して照らす(手順を写さない。開発手法 §4.7)。
 *
 *  ## 確かめること
 *   - 59a: 見つからないファイル: 見つからない行は出る。「no $version」「migrated」は出ない
 *   - 59b: パースに失敗するファイル(`"$version": 1` の後で壊れている): 位置の行は出る。版の行は出ない
 *   - 59c: 読めた v1 のファイルでは「migrated $version 1 -> 3」が出る(直した結果、正しい移行の行まで
 *          消していないことの歯)。読めた v99 では「newer than this build」が出る
 *
 *  ## 押さえないもの
 *   - 実機の `Game.log`(2-16 の完了報告で、パースに失敗する設定を置いたフォルダから起動して確かめる)
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "Core/GameConfig.h"
#include "Core/GameConfigLoad.h"
#include "Core/GameConfigLog.h"
#include "Core/Json/Json.h"
#include "Core/Logger.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

namespace {

  constexpr int kMaxLines   = 64;
  constexpr int kLineLength = 1024;

  struct Lines {
    char text[kMaxLines][kLineLength]{};
    int  count = 0;
  };

  void TempPath(char (&path)[512], const char* name) {
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\%s", hasTemp ? tempDir : ".", name);
  }

  bool WriteText(const char* path, const char* text) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) { return false; }
    const std::size_t n = std::strlen(text);
    const bool ok = std::fwrite(text, 1, n, f) == n;
    std::fclose(f);
    return ok;
  }

  void ReadLines(const char* path, Lines& out) {
    FILE* f = nullptr;
    out.count = 0;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) { out.count = -1; return; }
    char buf[kLineLength];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
      if (out.count < kMaxLines) { strcpy_s(out.text[out.count], buf); }
      ++out.count;
    }
    std::fclose(f);
  }

  int CountWith(const Lines& lines, const char* part) {
    int n = 0;
    for (int i = 0; i < lines.count && i < kMaxLines; ++i) {
      if (std::strstr(lines.text[i], part) != nullptr) { ++n; }
    }
    return n;
  }

  /// `basePath` を本番と同じ手順で読み、書かれた行を返す。.local.json は無いパスを渡す
  void LoadAndRead(const char* basePath, const char* logName, Lines& lines) {
    char logPath[512]{};
    TempPath(logPath, logName);
    char noLocal[512]{};
    TempPath(noLocal, "glfd_config_log_no_such.local.json");
    CHECK(GLFD::Core::Logger::Get().Initialize(logPath));
    {
      GLFD::Test::MockMemoryResource mock;
      std::unique_ptr<GLFD::Json::Document> doc;
      std::unique_ptr<GLFD::Json::Document> localDoc;
      std::unique_ptr<GLFD::GameConfig>     config;
      GLFD::ConfigDiagnosticsLogger diagnostics;
      (void)GLFD::ReloadGameConfig(doc, localDoc, config, basePath, noLocal, &mock, diagnostics);
    }
    GLFD::Core::Logger::Get().Shutdown();
    ReadLines(logPath, lines);
    CHECK(lines.count > 0);
  }

  Lines g_lines;   // 64 KB。スタックに置かない

  void TestNotFound() {
    GLFD::Test::BeginCase("T-ECS-59a: a file that is not found gets no version lines");
    char base[512]{};
    TempPath(base, "glfd_config_log_does_not_exist.jsonc");
    LoadAndRead(base, "glfd_config_log_59a.log", g_lines);
    CHECK(CountWith(g_lines, "FileNotFound") == 1);
    CHECK(CountWith(g_lines, "no $version") == 0);
    CHECK(CountWith(g_lines, "migrated") == 0);
    CHECK(CountWith(g_lines, "writing this file out") == 0);
  }

  void TestParseError() {
    GLFD::Test::BeginCase("T-ECS-59b: a file that does not parse gets its position, not version lines");
    char base[512]{};
    TempPath(base, "glfd_config_log_broken.jsonc");
    CHECK(WriteText(base, "{ \"$version\": 1, \"world\": { \"bounds\": [85.0, "));
    LoadAndRead(base, "glfd_config_log_59b.log", g_lines);
    CHECK(CountWith(g_lines, "glfd_config_log_broken.jsonc(1,") == 1);   // file(line,col): code
    CHECK(CountWith(g_lines, "no $version") == 0);
    CHECK(CountWith(g_lines, "migrated") == 0);
  }

  void TestReadFilesStillReport() {
    GLFD::Test::BeginCase("T-ECS-59c: a file that was read still reports its migration or its refusal");
    char base[512]{};
    TempPath(base, "glfd_config_log_v1.jsonc");
    CHECK(WriteText(base, "{ \"$version\": 1, \"world\": { \"bounds\": [85.0, 64.0] } }"));
    LoadAndRead(base, "glfd_config_log_59c1.log", g_lines);
    CHECK(CountWith(g_lines, "migrated $version 1 -> 3") == 1);
    CHECK(CountWith(g_lines, "no $version") == 0);

    TempPath(base, "glfd_config_log_v99.jsonc");
    CHECK(WriteText(base, "{ \"$version\": 99 }"));
    LoadAndRead(base, "glfd_config_log_59c2.log", g_lines);
    CHECK(CountWith(g_lines, "$version 99 is newer than this build") == 1);
    CHECK(CountWith(g_lines, "migrated") == 0);
  }

}

int main() {
  GLFD::Test::BeginSuite("ConfigLog (ECS 2-16)");
  TestNotFound();
  TestParseError();
  TestReadFilesStillReport();
  return GLFD::Test::Summarize();
}
