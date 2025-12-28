#include "Logger.h"
#include <ctime>
#include <iomanip>

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

    // コンソール出力
    std::cout << finalMsg;

    // ファイル出力
    if (m_fileStream.is_open()) {
      m_fileStream << finalMsg;
      m_fileStream.flush(); // クラッシュ時にログが残るようにフラッシュ
    }
  }
}
