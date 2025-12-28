#include "Profiler.h"

namespace GLFD::Core {
  void Instrumentor::BeginSession(const std::string& name, const std::string& filepath) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_CurrentSession) {
      InternalEndSession(); // 既に開いていれば閉じる
    }
    m_OutputStream.open(filepath);
    WriteHeader();
    m_CurrentSession = new InstrumentationSession{ name };
  }

  void Instrumentor::EndSession() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    InternalEndSession();
  }

  void Instrumentor::WriteProfile(const ProfileResult& result) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_ProfileCount++ > 0)
      m_OutputStream << ",";

    std::string name = result.Name;
    std::replace(name.begin(), name.end(), '"', '\'');

    m_OutputStream << "{";
    m_OutputStream << "\"cat\":\"function\",";
    m_OutputStream << "\"dur\":" << (result.End - result.Start) << ",";
    m_OutputStream << "\"name\":\"" << name << "\",";
    m_OutputStream << "\"ph\":\"X\",";
    m_OutputStream << "\"pid\":0,";
    m_OutputStream << "\"tid\":" << result.ThreadID << ",";
    m_OutputStream << "\"ts\":" << result.Start;
    m_OutputStream << "}";

    // 即時フラッシュしないとクラッシュ時にデータが残らないが、
    // 重すぎる場合はコメントアウトする
    // m_OutputStream.flush();
  }
  void Instrumentor::WriteHeader() {
    m_OutputStream << "{\"otherData\": {},\"traceEvents\":[";
    m_OutputStream.flush();
  }
  void Instrumentor::WriteFooter() {
    m_OutputStream << "]}";
    m_OutputStream.flush();
  }
  void Instrumentor::InternalEndSession() {
    if (m_CurrentSession) {
      WriteFooter();
      m_OutputStream.close();
      delete m_CurrentSession;
      m_CurrentSession = nullptr;
      m_ProfileCount = 0;
    }
  }
}
