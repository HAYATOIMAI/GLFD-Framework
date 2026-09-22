#include "JobSystem.h"
#include "JobSystemProbe.h"   // 試験用の差し込み点。本番では空になる

namespace GLFD::Thread {
  JobSystem::JobSystem(unsigned int numThreads) {
    if (numThreads == 0) {
      numThreads = std::thread::hardware_concurrency();

      if (numThreads == 0) numThreads = 1;
    }

    // ワーカースレッド起動
    m_workers.reserve(numThreads);
    m_workerIds.reserve(numThreads);

    for (unsigned int i = 0; i < numThreads; ++i) {
      m_workers.emplace_back([this] {
        WorkerLoop();
        });
      m_workerIds.push_back(m_workers.back().get_id());
    }
  }

  JobSystem::~JobSystem() {
    // **必ず呼ぶ。** 呼ばなければ join されていない std::thread の破棄で terminate する
    Stop();
  }

  void JobSystem::KickJob(JobFunction job, JobHandle* parentHandle) {
    // 呼んでよいのは所有スレッドとワーカーだけ(ECS 2-4 論点5 の前提を機械的に確かめる)
    assert(IsOwnerOrWorker() && "KickJob: call from the owner thread or from a job on a worker");

    // 停止を始めた後は積まない。親ハンドルの件数も増やさない
    if (m_stopping.load(std::memory_order_acquire)) {
      m_rejectedAfterStop.fetch_add(1, std::memory_order_relaxed);
      return;
    }

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

    // 通知: **待たれている値(世代)そのものを進めてから**起こす。
    // Push の後に進めるので、このジョブを取り逃したワーカーは必ず起きる
    m_wakeGeneration.fetch_add(1, std::memory_order_release);
    m_wakeGeneration.notify_one();
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
    assert(std::this_thread::get_id() == m_owner && "Stop: call from the owner thread (a worker would join itself)");

    // 2 回目以降は何もしない(1 回目で join と回収まで終えている)
    if (m_stopping.exchange(true, std::memory_order_acq_rel)) return;

    GLFD_JOB_PROBE(StopBeforeNotify, static_cast<size_t>(m_wakeGeneration.load(std::memory_order_relaxed)));
    // **待たれている値そのものを変えてから起こす。** 世代を読んだ後のワーカーは、
    // wait に入る時点で値が変わっているので眠らない。世代を読む前のワーカーは、
    // この加算を acquire で読んで m_stopping を見る
    m_wakeGeneration.fetch_add(1, std::memory_order_release);
    m_wakeGeneration.notify_all();
    GLFD_JOB_PROBE(StopAfterNotify, 0);

    // メンバの寿命が残っているうちに明示的に join する(1-C H4)。宣言順には頼らない
    for (std::thread& worker : m_workers) {
      worker.join();
    }

    // join の後でキューに残っているジョブは実行しない。親ハンドルの件数を減らし
    // (WaitFor が永久に待たないように)、件数を数える
    while (std::optional<JobWrapper> jobOpt = m_queue.Pop()) {
      if (jobOpt->counter) {
        jobOpt->counter->count.fetch_sub(1, std::memory_order_release);
      }
      m_abandonedAtStop.fetch_add(1, std::memory_order_relaxed);
    }
  }

  JobSystemStats JobSystem::Stats() const {
    JobSystemStats stats;
    stats.rejectedAfterStop = m_rejectedAfterStop.load(std::memory_order_relaxed);
    stats.abandonedAtStop = m_abandonedAtStop.load(std::memory_order_relaxed);
    return stats;
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
  }

  void JobSystem::WorkerLoop() {
    // 順序が要点(ECS 2-4)。**世代を先に読み、停止とキューを確認し、読んだ世代で眠る。**
    // - Stop() の加算が (1) より前: (1) の acquire がその加算を読むので (2) で停止が見える
    // - Stop() の加算が (1) より後: (4) の時点で値が変わっているので wait は即座に戻る
    // KickJob の加算も同じ論法で取り逃さない。件数は眠る判断に使わない
    for (;;) {
      const std::uint64_t generation = m_wakeGeneration.load(std::memory_order_acquire);   // (1)
      if (m_stopping.load(std::memory_order_relaxed)) {                                   // (2)
        break;
      }

      std::optional<JobWrapper> jobOpt = m_queue.Pop();                                   // (3)
      if (jobOpt) {
        ExecuteJob(*jobOpt);
        continue;
      }

      // ジョブがないときはスリープ
      GLFD_JOB_PROBE(WorkerBeforeWait, static_cast<size_t>(generation));
      m_wakeGeneration.wait(generation, std::memory_order_acquire);                       // (4)
    }
    GLFD_JOB_PROBE(WorkerExit, 0);
  }

  bool JobSystem::IsOwnerOrWorker() const {
    const std::thread::id self = std::this_thread::get_id();
    if (self == m_owner) return true;
    for (const std::thread::id& id : m_workerIds) {
      if (id == self) return true;
    }
    return false;
  }
}
