#pragma once

#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <functional>
#include <cassert>
#include "LockFreeQueue.h"
#include "../Core/HardwareConstants.h"
#include <iostream> // エラー出力用
#include <stdexcept> // std::runtime_error


namespace GLFD::Thread {
  // ジョブの完了状態を管理する内部構造体
  struct alignas(System::Constants::CacheLineSize) JobCounter {
    std::atomic<int32_t> count{ 0 };
  };

  /**
   * @brief ユーザーが扱うジョブ制御用ハンドル
   * @details JobCounterへのポインタをラップし、誤用を防ぐ
   */
  class JobHandle {
    friend class JobSystem;
  public:
    JobHandle() = default;

    // 完了しているか確認
    [[nodiscard]] bool IsBusy() const {
      return m_counter && m_counter->count.load(std::memory_order_acquire) > 0;
    }

  private:
    explicit JobHandle(JobCounter* counter) : m_counter(counter) {}
    JobCounter* m_counter = nullptr;
  };

  /**
   * @brief 依存関係解決とWork-Helping機能を備えたジョブシステム
   */
  class JobSystem {
  public:
    using JobFunction = std::function<void()>;
    // キューのサイズ定義 (2の累乗)
    static constexpr size_t QUEUE_CAPACITY = 4096;
    // KickJobのリトライ上限回数
    static constexpr int MAX_RETRY_COUNT = 10000;
    /**
     * @brief コンストラクタ
     * @param numThreads ワーカースレッド数（0で自動設定）
     */
    explicit JobSystem(unsigned int numThreads = 0);
    /**
     * デストラクタ
     */
    ~JobSystem();
    /**
     * @brief ジョブを投入する
     * @param job 実行する処理
     * @param parentHandle 依存関係を持たせたい親ハンドルのポインタ（任意）
     */
    void KickJob(JobFunction job, JobHandle* parentHandle = nullptr);
    /**
     * @brief 新しいカウンタを作成し、ハンドルを返す
     * @details JobSystem側でカウンタをプール管理するのが理想だが、
     * ここでは呼び出し元がスコープを持つ一時的な変数を渡すパターンを想定し、
     * ユーザーが用意した構造体を初期化してラップするヘルパーを用意する形とする。
     */
    [[nodiscard]] JobHandle CreateHandle(JobCounter& rawCounter);
    /**
     * @brief ハンドルが指すジョブ群が完了するまで待機（Help実行含む）
     */
    void WaitFor(JobHandle handle);

    void Stop();

  private:
    struct JobWrapper {
      JobFunction task;
      JobCounter* counter;
    };

    // LockFreeQueue (MPMC)
    LockFreeQueue<JobWrapper, 4096> m_queue;

    std::vector<std::jthread> m_workers;
    std::atomic<bool> m_stopSource;
    std::atomic<size_t> m_activeJobCount{ 0 }; // 待機スレッドへの通知用

    /**
     * @brief ジョブを実際に実行し、カウンターを減らす内部関数
     */
    void ExecuteJob(const JobWrapper& wrapper);

    void WorkerLoop(std::stop_token st);
  };
}