#include "JobSystem.h"

namespace GLFD::Thread {
  JobSystem::JobSystem(unsigned int numThreads) {
    if (numThreads == 0) {
      numThreads = std::thread::hardware_concurrency();

      if (numThreads == 0) numThreads = 1;
    }

    m_stopSource.store(false, std::memory_order_relaxed);

    // ワーカースレッド起動
    m_workers.reserve(numThreads);

    for (unsigned int i = 0; i < numThreads; ++i) {
      m_workers.emplace_back([this](std::stop_token st) {
        WorkerLoop(st);
        });
    }
  }

  JobSystem::~JobSystem() {
    Stop();
  }

  void JobSystem::KickJob(JobFunction job, JobHandle* parentHandle) {
    JobCounter* counter = nullptr;
    if (parentHandle) {
      counter = parentHandle->m_counter;
      if (counter) {
        counter->count.fetch_add(1, std::memory_order_relaxed);
      }
    }

    JobWrapper wrapper{ std::move(job), counter };

    int retryCount = 0;

    while (!m_queue.Push(wrapper)) {
      // リトライ回数上限チェック
      if (++retryCount > MAX_RETRY_COUNT) {
        // カウンターを戻す（投入失敗したため）
        if (counter) {
          counter->count.fetch_sub(1, std::memory_order_relaxed);
        }

        // 致命的なエラーとして通知
        // ゲーム開発中はここでアサートが落ち、キューサイズを増やすか負荷分散を検討する
        std::cerr << "[JobSystem] CRITICAL: Queue Full. Dropping Job." << std::endl;
        return;
        //std::cerr << "[JobSystem] Error: Job Queue Overflow!" << std::endl;
        //throw std::runtime_error("JobQueue Overflow: System is overloaded.");
      }

      // 少しCPUを譲って再試行
      std::this_thread::yield();
    }

    // 通知
    m_activeJobCount.fetch_add(1, std::memory_order_release);
    m_activeJobCount.notify_one();
  }

  JobHandle JobSystem::CreateHandle(JobCounter& rawCounter) {
    rawCounter.count.store(0, std::memory_order_relaxed);
    return JobHandle(&rawCounter);
  }

  void JobSystem::WaitFor(JobHandle handle) {
    if (!handle.m_counter) return;

    JobCounter* counter = handle.m_counter;

    while (counter->count.load(std::memory_order_acquire) > 0) {
      // 待機中に他のジョブを処理（Work Helping）
      std::optional<JobWrapper> jobOpt = m_queue.Pop();
      
      if (jobOpt) {
        ExecuteJob(*jobOpt);
      }
      else {
        std::this_thread::yield();
      }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
  }

  void JobSystem::Stop() {
    m_stopSource.store(true, std::memory_order_release);
    m_activeJobCount.notify_all();
  }

  void JobSystem::ExecuteJob(const JobWrapper& wrapper) {
    // ジョブ実行
    try {
      if (wrapper.task) wrapper.task();
    }
    catch (const std::exception e) {
      std::cerr << "[JobSystem] Exception in worker thread: " << e.what() << std::endl;
      // デバッグビルドでは即座に停止させて気づかせる
      assert(false && "Exception thrown in Job");
    }
    catch (...) {
      std::cerr << "[JobSystem] Unknown exception in worker thread." << std::endl;
      assert(false && "Unknown Exception thrown in Job");
    }
    // 完了通知
    if (wrapper.counter) {
      // カウンターを減らす
      wrapper.counter->count.fetch_sub(1, std::memory_order_release);
      // 0になったら wait しているスレッド（WaitForCounter中のスレッド）に通知
      wrapper.counter->count.notify_all();
    }
    // 通知用カウンタの更新（ワーカーのスリープ制御用）
    // 厳密な数は重要ではないため relaxed
    if (m_activeJobCount.load(std::memory_order_relaxed) > 0) {
      m_activeJobCount.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  void JobSystem::WorkerLoop(std::stop_token st) {
    while (!st.stop_requested() && !m_stopSource.load(std::memory_order_acquire)) {
      std::optional<JobWrapper> jobOpt = m_queue.Pop();

      if (jobOpt) {
        ExecuteJob(*jobOpt);
      }
      else {
        // ジョブがないときはスリープ
        size_t val = m_activeJobCount.load(std::memory_order_acquire);
        if (val == 0) {
          m_activeJobCount.wait(0);
        }
      }
    }
  }
}