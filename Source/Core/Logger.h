#pragma once
#include <string>
#include <mutex>
#include <fstream>
#include <iostream>
#include <sstream>
#include <atomic>

namespace GLFD::Core {
  /// コンソール (std::cout) にも書くか (ECS 2-10)。コンソールが閉じられたら、信号の処理のスレッドが
  /// false にする(閉じかけのコンソールへの書き込みで待たされないように。ファイルへは書き続ける)。
  /// **`Logger` の外に置く**: プロセスの終わりに `Logger` のシングルトンが壊されている最中も、
  /// 処理のスレッドが触り得るため(constinit の原子的な変数は壊されない)
  inline constinit std::atomic<bool> gLogToConsole{ true };

  enum class LogLevel {
    Info,
    Warning,
    Error
  };

  class Logger {
  public:
    // シングルトンアクセス
    static Logger& Get();

    /// ファイルを開けなければ false。そのときは理由 (パスとエラーのコード) をコンソールと
    /// OutputDebugString に 1 行書き、ファイルに書けていないことを覚える (ECS 2-11)。ゲームは止めない
    bool Initialize(const std::string& filePath = "engine.log");
    void Shutdown();

    /// ログのファイルに書けているか (ECS 2-11)。書けていなければ、2-9 の知らせは「Game.log を送って
    /// ください」と言わず「Game.log を書けませんでした」と伝える
    bool FileOpen() const;

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
    /// "[HH:MM:SS] [INFO] message\n" を作る (Log と、ファイルを開けなかった行が使う)
    static std::string FormatLine(LogLevel level, const std::string& message);
    ~Logger();

    std::ofstream m_fileStream;
    mutable std::mutex m_mutex; // スレッドセーフ用

    /// コンソールを UTF-8 に切り替えられたか (2-4)。false なら端末側で
    /// 日本語が化ける。ファイルと OutputDebugString は常に UTF-8 で正しい
    bool m_consoleCodePageChanged = true;
  };
}
#define LOG_INFO(...)    ::GLFD::Core::Logger::Get().LogFmt(::GLFD::Core::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)    ::GLFD::Core::Logger::Get().LogFmt(::GLFD::Core::LogLevel::Warning, __VA_ARGS__)
#define LOG_ERROR(...)   ::GLFD::Core::Logger::Get().LogFmt(::GLFD::Core::LogLevel::Error, __VA_ARGS__)