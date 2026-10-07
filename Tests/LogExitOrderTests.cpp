/**
 * @file  LogExitOrderTests.cpp
 * @brief T-ECS-48: コンソールが詰まってもファイルの行は失われない (ECS 2-11 問題2)
 *
 * @details
 *  conhost で利用者が文字を選んでいる間は、コンソールへの書き込みが戻らない。2-10 までの `Logger` は
 *  コンソールに先に書いていたので、止まった行はファイルにも残らなかった。2-11 で出口の順番を
 *  ファイル → OutputDebugString → コンソール にした(本当の記録はファイル)。
 *
 *  本物の `Logger` を使い、`std::cout` の行き先を「戻らない」出口に差し替えて詰まりを作る
 *  (本番のコードに差し替え口は足さない)。`Log` を別のスレッドで呼び、コンソールで止まっている間に
 *  ファイルを読む。待ちにはすべて上限がある。
 *
 *  ## 確かめること
 *   - 48a: コンソールで止まっている間に、その行がもうファイルにある(`Log` はまだ戻っていない)。
 *          詰まりを解くと `Log` は戻り、次の行も書ける
 *   - 48b: 2-10 の守り(`gLogToConsole` が偽)の間はコンソールに向かわず、ファイルには書く
 *
 *  ## 押さえないもの
 *   - OutputDebugString がファイルの後であること(読み戻す手段が無い。順番はコードで確かめる)
 *   - 本物の conhost の選択: `console_close_check.ps1` の C11
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <share.h>
#include <streambuf>
#include <string>
#include <thread>

#include "Core/Logger.h"

#include "TestHarness.h"

namespace {

  void TempPath(char (&path)[512], const char* name) {
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\%s", hasTemp ? tempDir : ".", name);
  }

  /// ファイル全体を読む(読めなければ空)
  std::string ReadAll(const char* path) {
    std::string text;
    // the Logger holds the file open for writing: share it (fopen_s would ask for _SH_SECURE)
    FILE* const f = _fsopen(path, "rb", _SH_DENYNO);
    if (f == nullptr) { return text; }
    int c = 0;
    while ((c = std::fgetc(f)) != EOF) { text.push_back(static_cast<char>(c)); }
    std::fclose(f);
    return text;
  }

  /// 書かれると、放されるまで戻らない出口(conhost の選択の間のコンソールの代わり)
  class BlockingBuf : public std::streambuf {
  public:
    void Release() {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_released = true;
      m_cv.notify_all();
    }
    /// 書き込みが来て止まっているか。`ms` まで待つ
    bool WaitEntered(int ms) {
      std::unique_lock<std::mutex> lock(m_mutex);
      return m_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return m_entered; });
    }
    bool Entered() {
      std::lock_guard<std::mutex> lock(m_mutex);
      return m_entered;
    }
    std::string Written() {
      std::lock_guard<std::mutex> lock(m_mutex);
      return m_written;
    }

  protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
      Block();
      std::lock_guard<std::mutex> lock(m_mutex);
      m_written.append(s, static_cast<std::size_t>(n));
      return n;
    }
    int_type overflow(int_type c) override {
      Block();
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!traits_type::eq_int_type(c, traits_type::eof())) { m_written.push_back(traits_type::to_char_type(c)); }
      return traits_type::not_eof(c);
    }

  private:
    void Block() {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_entered = true;
      m_cv.notify_all();
      // 放されるまで待つ。テストが放し忘れても終わるよう、上限は 10 秒
      (void)m_cv.wait_for(lock, std::chrono::seconds(10), [this] { return m_released; });
    }

    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_entered  = false;
    bool                    m_released = false;
    std::string             m_written;
  };

  bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

  // ===========================================================================
  // 48a コンソールで止まっている間も、ファイルにはもうある
  // ===========================================================================
  void TestFileBeforeBlockedConsole() {
    GLFD::Test::BeginCase("T-ECS-48a: while the console write is stuck, the line is already in the file");

    char path[512]{};
    TempPath(path, "glfd_log_exit_order.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    BlockingBuf blocking;
    std::streambuf* const original = std::cout.rdbuf(&blocking);
    std::atomic<bool> returned{ false };
    std::thread writer([&returned] {
      GLFD::Core::Logger::Get().Log(GLFD::Core::LogLevel::Info, "line written while the console is stuck");
      returned.store(true);
    });

    const bool entered = blocking.WaitEntered(5000);      // コンソールの出口で止まった
    const std::string whileStuck = ReadAll(path);         // その間のファイル
    const bool returnedWhileStuck = returned.load();
    blocking.Release();
    writer.join();                                        // 放したので戻る (Block の上限 10 秒もある)
    std::cout.rdbuf(original);

    CHECK(entered);
    CHECK(!returnedWhileStuck);                           // Log はまだ戻っていなかった(詰まりを作れていた)
    CHECK(Contains(whileStuck, "[INFO] line written while the console is stuck"));
    if (!Contains(whileStuck, "line written while the console is stuck")) {
      std::printf("      file while the console was stuck: [%s]\n", whileStuck.c_str());
    }
    CHECK(returned.load());
    CHECK(Contains(blocking.Written(), "line written while the console is stuck"));   // 放した後、コンソールにも出た

    // 戻った後も書ける
    GLFD::Core::Logger::Get().Log(GLFD::Core::LogLevel::Info, "next line");
    GLFD::Core::Logger::Get().Shutdown();
    const std::string after = ReadAll(path);
    std::remove(path);
    CHECK(Contains(after, "line written while the console is stuck"));
    CHECK(Contains(after, "[INFO] next line"));
  }

  // ===========================================================================
  // 48b 2-10 の守り
  // ===========================================================================
  void TestSilencedConsoleStillWritesFile() {
    GLFD::Test::BeginCase("T-ECS-48b: with the console silenced (2-10), nothing goes to the console and the file still gets the line");

    char path[512]{};
    TempPath(path, "glfd_log_exit_silenced.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    BlockingBuf blocking;
    std::streambuf* const original = std::cout.rdbuf(&blocking);
    GLFD::Core::gLogToConsole.store(false);
    GLFD::Core::Logger::Get().Log(GLFD::Core::LogLevel::Info, "line while the console is closing");   // 止まらずに戻る
    GLFD::Core::gLogToConsole.store(true);
    const bool touched = blocking.Entered();
    blocking.Release();
    std::cout.rdbuf(original);
    GLFD::Core::Logger::Get().Shutdown();

    const std::string text = ReadAll(path);
    std::remove(path);
    CHECK(!touched);
    CHECK(Contains(text, "[INFO] line while the console is closing"));
  }

}

int main() {
  GLFD::Test::BeginSuite("LogExitOrder (ECS 2-11)");
  TestFileBeforeBlockedConsole();
  TestSilencedConsoleStillWritesFile();
  return GLFD::Test::Summarize();
}
