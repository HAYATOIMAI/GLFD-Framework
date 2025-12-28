#include "ThreadPool.h"

namespace GLFD::Thread {
  ThreadPool::ThreadPool(unsigned int numThreads) {
    if (numThreads == 0) {
      numThreads = std::thread::hardware_concurrency();
      if (numThreads == 0) numThreads = 4; // フォールバック
    }

    m_stopSource.store(false, std::memory_order_relaxed);
    m_activeTaskCount.store(0, std::memory_order_relaxed);

    // ワーカースレッドの起動
    m_workers.reserve(numThreads);
    for (unsigned int i = 0; i < numThreads; ++i) {
      // jthreadはデストラクタで自動的にjoinする (C++20)
      m_workers.emplace_back([this](std::stop_token st) {
        WorkerLoop(st);
        });
    }
  }
  ThreadPool::~ThreadPool() {
    Stop();
  }

  void ThreadPool::Stop() {
    m_stopSource.store(true, std::memory_order_release);
    m_activeTaskCount.notify_all();
  }

  void ThreadPool::WorkerLoop(std::stop_token st) {
    while (!st.stop_requested() && !m_stopSource.load(std::memory_order_acquire)) {
      // キューからタスクを取り出す
      std::optional<Task> taskOpt = m_queue.Pop();

      // タスクがあれば実行
      if (taskOpt) {
        try {
          (*taskOpt)();
        }
        catch (...) {
          // タスク内での例外は握りつぶす（スレッドを落とさないため）
        }

        // タスク完了に伴いカウントを減らす
        //（ここでのカウントは厳密なタスク数というより、通知用のカウンタとして利用）
        if (m_activeTaskCount.load(std::memory_order_relaxed) > 0) {
          m_activeTaskCount.fetch_sub(1, std::memory_order_relaxed);
        }
      }
      else {
        // タスクがない場合、スリープする (Wait)
        // 値が 0 である限り待機し続ける
        // notifyされるか、値が変わると目覚める（Spurious wakeupあり）

        size_t currentCount = m_activeTaskCount.load(std::memory_order_acquire);
        if (currentCount == 0) {
          m_activeTaskCount.wait(0);
        }
      }
    }
  }
} // namespace GLFD::Thread