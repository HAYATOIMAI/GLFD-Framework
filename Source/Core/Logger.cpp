#include "Logger.h"
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

  bool Logger::Initialize(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // ログの中身は UTF-8 である(JSON の値をそのまま流すため)。ファイル側は
    // 正しく書けているのに、コンソールだけが既定のコードページで化けるので、
    // **初期化で 1 回だけ**切り替える。出力のたびに呼ぶ類のものではない。
    // 出力先がコンソールでない(リダイレクトされている)場合などは失敗するが、
    // その場合もログ自体は成立するので続行する
    if (::SetConsoleOutputCP(CP_UTF8) == 0) {
      m_consoleCodePageChanged = false;
    }

    m_fileStream.open(filePath, std::ios::out | std::ios::trunc);
    return m_fileStream.is_open();
  }

  void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fileStream.is_open()) {
      m_fileStream.close();
    }
  }

  void Logger::Log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);

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
    std::string finalMsg = ss.str();

    // コンソール出力。UTF-8 へ切り替えられていない場合、日本語を含む行は
    // 端末側で化ける(ファイルと OutputDebugString は影響を受けない)
    std::cout << finalMsg;

    // デバッガの出力ウィンドウ。診断の行をダブルクリックで辿れるようにする
    ::OutputDebugStringA(finalMsg.c_str());

    // ファイル出力
    if (m_fileStream.is_open()) {
      m_fileStream << finalMsg;
      m_fileStream.flush(); // クラッシュ時にログが残るようにフラッシュ
    }
  }
}
