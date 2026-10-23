#include "Logger.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>

// 出力ウィンドウへも流す。`file(line,col): message` 形式の診断を
// Visual Studio がダブルクリックで辿れるようにするため (2-4)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace GLFD::Core {
  Logger& Logger::Get() {
    static Logger instance;
    return instance;
  }

  Logger::~Logger() {
    Shutdown();
  }

  namespace {
    /// ワイドのパスを、ログの行に書く UTF-8 にする。直せなければ空
    std::string ToUtf8(const wchar_t* wide) {
      const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
      if (needed <= 1) { return std::string(); }
      std::string utf8(static_cast<size_t>(needed), '\0');
      if (::WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), needed, nullptr, nullptr) <= 0) { return std::string(); }
      utf8.resize(static_cast<size_t>(needed) - 1);
      return utf8;
    }
  }

  bool Logger::Initialize(const std::string& filePath) {
    // UTF-8 として直す。直せなければ空のパスで開こうとして、開けなかった理由の行を書く
    std::wstring wide;
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filePath.c_str(), -1, nullptr, 0);
    if (needed > 0) {
      wide.assign(static_cast<size_t>(needed), L'\0');
      if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filePath.c_str(), -1, wide.data(), needed) <= 0) { wide.clear(); }
      else { wide.resize(static_cast<size_t>(needed) - 1); }
    }
    return Initialize(wide.c_str());
  }

  bool Logger::Initialize(const wchar_t* filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // ログの中身は UTF-8 である(JSON の値をそのまま流すため)。ファイル側は
    // 正しく書けているのに、コンソールだけが既定のコードページで化けるので、
    // **初期化で 1 回だけ**切り替える。出力のたびに呼ぶ類のものではない。
    // 出力先がコンソールでない(リダイレクトされている)場合などは失敗するが、
    // その場合もログ自体は成立するので続行する。結果は記録しない(2-4 から記録していた
    // m_consoleCodePageChanged は読まれていなかったので ECS 2-14 で消した)
    (void)::SetConsoleOutputCP(CP_UTF8);

    errno = 0;
    m_fileStream.open(filePath, std::ios::out | std::ios::trunc);
    if (m_fileStream.is_open()) { return true; }

    // **黙らない** (ECS 2-11)。ファイルが無いので、コンソールと OutputDebugString に 1 行だけ書く。
    // errno / _doserrno は開こうとした直後に読む (CRT が開けなかった理由を残す)
    const int           err    = errno;
    const unsigned long winErr = _doserrno;
    char errText[128] = {};
    if (strerror_s(errText, sizeof errText, err) != 0) { errText[0] = '\0'; }
    // パスは切らない (ECS 2-16。以前は char[512] と char[1024] の行で、長いパスは黙って切れた)。
    // フルパスが配列に入らなければ、渡されたパスをそのまま書く
    wchar_t fullPath[1024] = {};
    const DWORD n = ::GetFullPathNameW(filePath, static_cast<DWORD>(sizeof fullPath / sizeof fullPath[0]), fullPath, nullptr);
    const wchar_t* shownPath = (n > 0 && n < sizeof fullPath / sizeof fullPath[0]) ? fullPath : filePath;
    char tail[256];
    std::snprintf(tail, sizeof tail,
                  " for writing: errno %d (%s), Windows error %lu. "
                  "this run writes no log file; lines go to the console only",
                  err, errText, winErr);
    const std::string finalMsg = FormatLine(LogLevel::Error, "log: could not open " + ToUtf8(shownPath) + tail);
    // Log と同じ順番: 待たされ得るコンソールを最後に (ECS 2-11 の決定)
    ::OutputDebugStringA(finalMsg.c_str());
    if (gLogToConsole.load(std::memory_order_relaxed)) { std::cout << finalMsg; }
    return false;
  }

  bool Logger::FileOpen() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fileStream.is_open();
  }

  void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fileStream.is_open()) {
      m_fileStream.close();
    }
  }

  std::string Logger::FormatLine(LogLevel level, const std::string& message) {
    // タイムスタンプ取得
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);

    std::stringstream ss;
    ss << "[" << std::put_time(&tm, "%H:%M:%S") << "] ";

    switch (level) {
    case LogLevel::Info:    ss << "[INFO] "; break;
    case LogLevel::Warning: ss << "[WARN] "; break;
    case LogLevel::Error:   ss << "[ERR ] "; break;
    }

    ss << message << "\n";
    return ss.str();
  }

  void Logger::Log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string finalMsg = FormatLine(level, message);

    // **出口の順番: ファイル → OutputDebugString → コンソール** (ECS 2-11)。本当の記録はファイル。
    // conhost で利用者が文字を選んでいる間はコンソールへの書き込みが戻らない。2-10 まではコンソールが
    // 先だったので、止まった行はファイルにも残らなかった。OutputDebugString もデバッガをつないでいる間は
    // 待たされ得るので、ファイルを待たされ得る出口のすべてより前に置く。
    // コンソールで止まること自体は残る (負債。別のスレッドで書けば止まらないが、終了の順番に
    // 戻らないかもしれない部品が増える)
    if (m_fileStream.is_open()) {
      m_fileStream << finalMsg;
      m_fileStream.flush(); // 行ごとに OS へ渡す。途中で切られても、書き終わった行は残る
    }

    // デバッガの出力ウィンドウ。診断の行をダブルクリックで辿れるようにする
    ::OutputDebugStringA(finalMsg.c_str());

    // コンソール。UTF-8 へ切り替えられていない場合、日本語を含む行は端末で化ける
    // (ファイルと OutputDebugString は影響を受けない)
    if (gLogToConsole.load(std::memory_order_relaxed)) {   // ECS 2-10: コンソールが閉じかけなら書かない
      std::cout << finalMsg;
    }
  }
}
