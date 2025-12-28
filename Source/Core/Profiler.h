#pragma once
#include <string>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <thread>
#include <mutex>
#include <iostream>

namespace GLFD::Core {

  // 計測結果データ
  struct ProfileResult {
    std::string Name;
    long long Start, End;
    uint32_t ThreadID;
  };

  struct InstrumentationSession {
    std::string Name;
  };

  class Instrumentor {
  public:
    Instrumentor() : m_CurrentSession(nullptr), m_ProfileCount(0) {}

    // 計測開始 (ファイル作成)
    void BeginSession(const std::string& name, const std::string& filepath = "results.json");

    // 計測終了
    void EndSession();

    // 結果書き込み (スレッドセーフ)
    void WriteProfile(const ProfileResult& result);

    static Instrumentor& Get()
    {
      static Instrumentor instance;
      return instance;
    }

  private:
    std::mutex m_Mutex;
    InstrumentationSession* m_CurrentSession;
    std::ofstream m_OutputStream;
    int m_ProfileCount;

    void WriteHeader();

    void WriteFooter();

    void InternalEndSession();
  };

  // RAIIタイマー: スコープを抜けると自動で計測終了して書き込む
  class InstrumentationTimer  {
  public:
    InstrumentationTimer(const char* name) : m_Name(name), m_Stopped(false)
    {
      m_StartTimepoint = std::chrono::high_resolution_clock::now();
    }

    ~InstrumentationTimer() {
      if (!m_Stopped) Stop();
    }

    void Stop() {
      auto endTimepoint = std::chrono::high_resolution_clock::now();

      auto start = std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimepoint).time_since_epoch().count();
      auto end = std::chrono::time_point_cast<std::chrono::microseconds>(endTimepoint).time_since_epoch().count();

      // スレッドIDをハッシュ化して数値にする
      auto threadID = static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

      Instrumentor::Get().WriteProfile({ m_Name, start, end, threadID });
      m_Stopped = true;
    }

  private:
    const char* m_Name;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_StartTimepoint;
    bool m_Stopped;
  };
}

#define GAMELIB_PROFILE 1

#if GAMELIB_PROFILE
// スコープの先頭に置くと、そのスコープが終わるまでの時間を計測
#define PROFILE_SCOPE(name) ::GLFD::Core::InstrumentationTimer timer##__LINE__(name)
// 関数全体の時間を計測（関数名を自動取得）
#define PROFILE_FUNCTION() PROFILE_SCOPE(__FUNCTION__)
#else
#define PROFILE_SCOPE(name)
#define PROFILE_FUNCTION()
#endif