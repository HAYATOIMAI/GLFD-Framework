/**
 * @file  StartupLogTests.cpp
 * @brief T-ECS-47: 起動の失敗の理由が Game.log に残る (ECS 2-11 問題1)
 *
 * @details
 *  2-10 までは起動の失敗の理由が `std::cerr` にしか出ず、`Game.log` には `DX11 Init Failed!` しか
 *  残らなかった。理由はすべて `Logger` に書く (`Graphics/StartupFailureLog.h`)。本番と同じ関数を本物の
 *  `Logger` で呼び、書かれた行を読み戻して照らす(手順を写さない。開発手法 §4.7)。
 *
 *  ## 確かめること
 *   - 47a: `Source/` に、`Logger` を通さずにコンソールへ書く箇所が無い(`Logger.cpp` だけが出口)。
 *          grep をテストにしたもの。コメントの中は数えない
 *   - 47b: シェーダーのコンパイルエラーの本文が 1 行ずつ、`shader: ` の頭付きで書かれ、最初の行に
 *          行数がある。1,024 バイトを超える行も切れない。本文が無いときも 1 行書く
 *   - 47c: 起動の途中の呼び出しの失敗は、`HRESULT` を 2-9 と同じ「名前 (0x…)」の形で書く
 *   - 47d: `Logger::Initialize` がファイルを開けないとき、黙らない(パスとエラーのコードを
 *          コンソールに 1 行)。`FileOpen()` が false になり、次に開ければ true に戻る
 *   - 47e: ファイルに書けていないとき、2-9 の知らせは「Game.log を送ってください」と言わず、
 *          「Game.log を書けませんでした (パス)」と伝える
 *
 *  ## 押さえないもの
 *   - 本物の起動の失敗で `Game.log` に理由が残ること: `render_fault_check.ps1` の H6
 *   - 本物の開けないログで知らせが変わること: `render_fault_check.ps1` の H7
 *   - `OutputDebugStringA` に書かれること(読み戻す手段が無い)
 *   - 47a は文字の並びを探すだけ。`//` の後と、`*` / `/*` で始まる行は数えない。そのため、行の頭に `*` の無い
 *     ブロックのコメントの中の `std::cerr` は誤って数え、`using namespace std;` の後の `cerr <<` や、マクロに
 *     隠した書き込みは見逃す
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。日本語の文は \u で照らす。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "Core/Logger.h"
#include "Graphics/RenderHealth.h"
#include "Graphics/StartupFailureLog.h"

#include "TestHarness.h"

namespace {

  void TempPath(char (&path)[512], const char* name) {
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\%s", hasTemp ? tempDir : ".", name);
  }

  /// 行を読み戻す(長い行も切らない)。読めなければ空。
  /// ファイルは text モードで書かれ、行末は CRLF。行末の CR を 1 つだけ除く(行の中の CR は残して捕まえる)
  std::vector<std::string> ReadLines(const char* path) {
    std::vector<std::string> lines;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) { return lines; }
    std::string current;
    int c = 0;
    while ((c = std::fgetc(f)) != EOF) {
      if (c == '\n') {
        if (!current.empty() && current.back() == '\r') { current.pop_back(); }
        lines.push_back(current);
        current.clear();
      }
      else { current.push_back(static_cast<char>(c)); }
    }
    if (!current.empty()) { lines.push_back(current); }
    std::fclose(f);
    return lines;
  }

  bool Contains(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

  // ===========================================================================
  // 47a Source/ に Logger を通さない出力が無い
  // ===========================================================================

  /// `word(` が、前に英数字や _ の無い形で現れるか(snprintf の中の printf を数えない)
  bool HasCall(const std::string& code, const char* word) {
    const std::string needle = std::string(word) + "(";
    for (std::size_t at = code.find(needle); at != std::string::npos; at = code.find(needle, at + 1)) {
      const char before = (at == 0) ? ' ' : code[at - 1];
      const bool partOfName = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
                              (before >= '0' && before <= '9') || before == '_';
      if (!partOfName) { return true; }
    }
    return false;
  }

  /// 1 行からコメントを除いた部分。`*` か `/*` で始まる行(ブロックのコメントの中)は空
  std::string CodeOf(const std::string& line) {
    std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) { return std::string(); }
    if (line.compare(first, 1, "*") == 0 || line.compare(first, 2, "/*") == 0) { return std::string(); }
    const std::size_t comment = line.find("//");
    return (comment == std::string::npos) ? line : line.substr(0, comment);
  }

  void TestNoDirectConsoleWrites() {
    GLFD::Test::BeginCase("T-ECS-47a: nothing in Source/ writes to the console without the Logger");

    // このファイルの場所から Source/ を探す(cl にはフルパスで渡している)
    std::error_code ec;
    const std::filesystem::path source =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "Source";
    CHECK(std::filesystem::is_directory(source, ec));

    static const char* const kStreams[] = { "std::cerr", "std::cout", "std::clog", "std::wcerr", "std::wcout" };
    static const char* const kCalls[]   = { "printf", "wprintf", "fprintf", "fwprintf", "puts", "_putws",
                                            "_cprintf", "WriteConsoleA", "WriteConsoleW", "WriteConsole" };
    int  files = 0;
    int  found = 0;
    bool sawLogger = false;
    std::filesystem::recursive_directory_iterator it(source, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) { continue; }
      const std::filesystem::path& p = it->path();
      const std::string ext = p.extension().string();
      if (ext != ".h" && ext != ".cpp" && ext != ".hpp" && ext != ".inl") { continue; }
      ++files;
      // Logger.cpp だけが出口(コンソール・OutputDebugString・ファイル)
      if (p.filename() == "Logger.cpp" && p.parent_path().filename() == "Core") { sawLogger = true; continue; }
      FILE* f = nullptr;
      if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || f == nullptr) { ++found; std::printf("      could not read %ls\n", p.c_str()); continue; }
      std::string line;
      int lineNo = 0;
      int c = 0;
      bool more = true;
      while (more) {
        c = std::fgetc(f);
        if (c != '\n' && c != EOF) { line.push_back(static_cast<char>(c)); continue; }
        ++lineNo;
        const std::string code = CodeOf(line);
        bool bad = false;
        for (const char* s : kStreams) { if (code.find(s) != std::string::npos) { bad = true; } }
        for (const char* s : kCalls)   { if (HasCall(code, s)) { bad = true; } }
        if (bad) {
          ++found;
          std::printf("      %ls(%d): %s\n", p.c_str(), lineNo, line.c_str());
        }
        line.clear();
        more = (c != EOF);
      }
      std::fclose(f);
    }
    CHECK(!ec);
    CHECK(sawLogger);
    CHECK(files >= 50);              // 0 件で通らないように(探し方を誤ったら落ちる)
    CHECK(found == 0);
    std::printf("      scanned %d file(s) under Source/, %d direct console write(s)\n", files, found);
  }

  // ===========================================================================
  // 47b シェーダーのコンパイルエラーの本文
  // ===========================================================================
  void TestShaderCompileLines() {
    GLFD::Test::BeginCase("T-ECS-47b: shader compiler output is written one line at a time, uncut");

    char path[512]{};
    TempPath(path, "glfd_startup_shader.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    // 改行は CRLF と LF が混じり、空の行があり、1 行は 1,024 バイトを超える。blob の本文は終端を含む
    std::string text = "particle.hlsl(12,5): error X3000: syntax error\r\nparticle.hlsl(13,1): error X3004: undeclared identifier\n\n";
    std::string longLine = "particle.hlsl(40,9): warning X3206: ";
    longLine.append(1500, 'x');
    longLine += "END";
    text += longLine;
    text += "\n";
    const int written = GLFD::Graphics::ReportShaderCompileError("VS", "vs_4_0", E_FAIL, text.c_str(), text.size() + 1);
    const int none    = GLFD::Graphics::ReportShaderCompileError("PS", "ps_4_0", E_INVALIDARG, nullptr, 0);
    GLFD::Core::Logger::Get().Shutdown();

    const std::vector<std::string> lines = ReadLines(path);
    std::remove(path);
    CHECK(written == 3);
    CHECK(none == 0);
    CHECK(lines.size() == 5);
    if (lines.size() != 5) {
      for (const std::string& l : lines) { std::printf("      line: %.200s\n", l.c_str()); }
      return;
    }
    CHECK(Contains(lines[0], "[ERR ] shader: could not compile VS (vs_4_0): E_FAIL (0x80004005). 3 line(s) of compiler output follow"));
    CHECK(Contains(lines[1], "[ERR ] shader: particle.hlsl(12,5): error X3000: syntax error"));
    CHECK(Contains(lines[2], "[ERR ] shader: particle.hlsl(13,1): error X3004: undeclared identifier"));
    CHECK(Contains(lines[3], "[ERR ] shader: particle.hlsl(40,9): warning X3206: xxxx"));
    CHECK(Contains(lines[3], "xEND"));                                  // 切れていない
    CHECK(lines[3].size() > 1500);
    CHECK(Contains(lines[4], "[ERR ] shader: could not compile PS (ps_4_0): E_INVALIDARG (0x80070057). the compiler gave no output"));
    for (const std::string& l : lines) { CHECK(!Contains(l, "\r")); }   // 行の中に CR を残さない
  }

  // ===========================================================================
  // 47c HRESULT の書き方
  // ===========================================================================
  void TestStartupCallLines() {
    GLFD::Test::BeginCase("T-ECS-47c: a failed startup call is written with its HRESULT as name (0x...)");

    char path[512]{};
    TempPath(path, "glfd_startup_calls.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));
    GLFD::Graphics::ReportStartupCall("graphics", "creating the D3D11 device and swap chain", E_OUTOFMEMORY);
    GLFD::Graphics::ReportStartupCall("shader", "creating the input layout", static_cast<HRESULT>(0x88760870L));
    GLFD::Core::Logger::Get().Shutdown();

    const std::vector<std::string> lines = ReadLines(path);
    std::remove(path);
    CHECK(lines.size() == 2);
    if (lines.size() != 2) { return; }
    CHECK(Contains(lines[0], "[ERR ] graphics: creating the D3D11 device and swap chain failed: E_OUTOFMEMORY (0x8007000E)"));
    CHECK(Contains(lines[1], "[ERR ] shader: creating the input layout failed: 0x88760870"));   // 表に無い値は 16 進だけ
  }

  // ===========================================================================
  // 47d Logger::Initialize がファイルを開けない
  // ===========================================================================
  void TestLogFileCannotOpen() {
    GLFD::Test::BeginCase("T-ECS-47d: when the log file cannot be opened, the Logger says so and remembers it");

    char dir[512]{};
    TempPath(dir, "glfd_no_such_dir_ecs_2_11");
    std::error_code ec;
    CHECK(!std::filesystem::exists(dir, ec));   // 開けないことの前提
    char path[600]{};
    std::snprintf(path, sizeof path, "%s\\sub\\Game.log", dir);

    // コンソールに書かれた行を捕まえる(Logger は std::cout に書く)
    std::ostringstream captured;
    std::streambuf* const original = std::cout.rdbuf(captured.rdbuf());
    const bool opened = GLFD::Core::Logger::Get().Initialize(path);
    const bool openAfter = GLFD::Core::Logger::Get().FileOpen();
    LOG_INFO("a line while no file is open");   // 落ちない。コンソールにだけ出る
    std::cout.rdbuf(original);

    const std::string text = captured.str();
    CHECK(!opened);
    CHECK(!openAfter);
    CHECK(Contains(text, "[ERR ] log: could not open "));
    CHECK(Contains(text, "glfd_no_such_dir_ecs_2_11\\sub\\Game.log for writing: errno "));
    CHECK(!Contains(text, "errno 0 "));          // 理由のコードが入っている
    CHECK(Contains(text, "Windows error "));
    CHECK(Contains(text, "this run writes no log file"));
    CHECK(Contains(text, "a line while no file is open"));
    if (!Contains(text, "[ERR ] log: could not open ")) { std::printf("      console: %s\n", text.c_str()); }
    CHECK(!std::filesystem::exists(dir, ec));    // 何も作っていない

    // 次に開ければ戻る
    char good[512]{};
    TempPath(good, "glfd_startup_reopen.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(good));
    CHECK(GLFD::Core::Logger::Get().FileOpen());
    GLFD::Core::Logger::Get().Shutdown();
    CHECK(!GLFD::Core::Logger::Get().FileOpen());
    std::remove(good);
  }

  // ===========================================================================
  // 47e 2-9 の知らせ
  // ===========================================================================
  void TestExitMessageWithoutLog() {
    GLFD::Test::BeginCase("T-ECS-47e: with no Game.log the notice says it could not be written, and does not ask for it");

    using GLFD::Graphics::ExitReason;
    const wchar_t* const kNotWritten = L"Game.log \u3092\u66f8\u3051\u307e\u305b\u3093\u3067\u3057\u305f (C:\\work\\Game.log)";
    const wchar_t* const kSend       = L"\u9001\u3063\u3066";          // 送って
    const wchar_t* const kLogLine    = L"\u30ed\u30b0: C:\\work\\Game.log";   // ログ: <path>

    GLFD::Graphics::RenderFailure none{};
    GLFD::Graphics::RenderFailure reset{};
    reset.failed = true;
    reset.where = "Present";
    reset.returned = DXGI_ERROR_DEVICE_REMOVED;
    reset.removedReason = DXGI_ERROR_DEVICE_RESET;
    GLFD::Graphics::RenderFailure notLost{};
    notLost.failed = true;
    notLost.where = "Map";
    notLost.returned = E_OUTOFMEMORY;

    wchar_t out[GLFD::Graphics::kExitMessageLength]{};
    // 起動の失敗
    CHECK(GLFD::Graphics::FormatExitMessage(out, ExitReason::StartupFailed, none, "x", L"C:\\work", false) > 0);
    CHECK(std::wcsstr(out, kNotWritten) != nullptr);
    CHECK(std::wcsstr(out, kLogLine) == nullptr);
    CHECK(GLFD::Graphics::FormatExitMessage(out, ExitReason::StartupFailed, none, "x", L"C:\\work", true) > 0);
    CHECK(std::wcsstr(out, kLogLine) != nullptr);
    CHECK(std::wcsstr(out, kNotWritten) == nullptr);
    // 描画の失敗(リセット / 失われていない): 送ってと言わない
    for (const GLFD::Graphics::RenderFailure* f : { &reset, &notLost }) {
      CHECK(GLFD::Graphics::FormatExitMessage(out, ExitReason::RenderFailed, *f, nullptr, L"C:\\work", false) > 0);
      CHECK(std::wcsstr(out, kNotWritten) != nullptr);
      CHECK(std::wcsstr(out, kSend) == nullptr);
      CHECK(GLFD::Graphics::FormatExitMessage(out, ExitReason::RenderFailed, *f, nullptr, L"C:\\work", true) > 0);
      CHECK(std::wcsstr(out, kSend) != nullptr);
      CHECK(std::wcsstr(out, kLogLine) != nullptr);
    }
  }

}

int main() {
  GLFD::Test::BeginSuite("StartupLog (ECS 2-11)");
  TestNoDirectConsoleWrites();
  TestShaderCompileLines();
  TestStartupCallLines();
  TestLogFileCannotOpen();
  TestExitMessageWithoutLog();
  return GLFD::Test::Summarize();
}
