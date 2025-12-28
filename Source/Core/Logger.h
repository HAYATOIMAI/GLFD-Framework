#pragma once
#include <string>
#include <mutex>
#include <fstream>
#include <iostream>
#include <sstream>

namespace GLFD::Core {
  enum class LogLevel {
    Info,
    Warning,
    Error
  };

  class Logger {
  public:
    // シングルトンアクセス
    static Logger& Get();

    bool Initialize(const std::string& filePath = "engine.log");
    void Shutdown();

    // ログ出力関数
    void Log(LogLevel level, const std::string& message);

    // フォーマット付きログ（簡易版）
    template<typename... Args>
    void LogFmt(LogLevel level, const char* fmt, Args... args) {
      char buffer[1024];
      snprintf(buffer, sizeof(buffer), fmt, args...);
      Log(level, std::string(buffer));
    }

  private:
    Logger() = default;
    ~Logger();

    std::ofstream m_fileStream;
    std::mutex m_mutex; // スレッドセーフ用
  };
}
#define LOG_INFO(...)    ::GLFD::Core::Logger::Get().LogFmt(::GLFD::Core::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)    ::GLFD::Core::Logger::Get().LogFmt(::GLFD::Core::LogLevel::Warning, __VA_ARGS__)
#define LOG_ERROR(...)   ::GLFD::Core::Logger::Get().LogFmt(::GLFD::Core::LogLevel::Error, __VA_ARGS__)