#pragma once

#include <thread>
#include <vector>
#include <functional>
#include <future>
#include <atomic>
#include "LockFreeQueue.h"

namespace GLFD::Thread {
  /**
   * @brief ロックフリーキューを使用した高パフォーマンススレッドプール
   */
  class ThreadPool {
  public:
    // タスク型: voidを返し引数を取らない関数オブジェクト
    using Task = std::function<void()>;

    /**
     * @brief コンストラクタ
     * @param numThreads スレッド数 (0の場合はハードウェアの並行数を使用)
     */
    explicit ThreadPool(unsigned int numThreads = 0);

    /**
     * @brief デストラクタ
     * スレッドの停止と結合を待機します。
     */
    ~ThreadPool();

    /**
     * @brief タスクを追加します (Fire-and-Forget)
     */
    template <typename F>
    void Enqueue(F&& f) {
      // タスクをキューにプッシュ
      // 失敗した場合（満杯）はスピンして待つ（簡易実装）
      // 本格的なエンジンでは別のバックアップキューに入れるなどの対策が必要
      while (!m_queue.Push(Task(std::forward<F>(f)))) {
        std::this_thread::yield();
      }

      // タスク数をインクリメントし、待機中のスレッドを1つ起こす
      m_activeTaskCount.fetch_add(1, std::memory_order_release);
      m_activeTaskCount.notify_one(); // C++20 atomic notify
    }

    /**
     * @brief すべてのスレッドを停止させます
     */
    void Stop();

  private:
    // LockFreeQueue: 容量4096 (2^12)
    LockFreeQueue<Task, 4096> m_queue;

    // 割り込み可能なスレッド
    std::vector<std::jthread> m_workers;

    // 停止フラグ
    std::atomic<bool> m_stopSource;

    // 待機用のアトミック変数
    // タスクが入っているかどうかのシグナルとして使用
    std::atomic<size_t> m_activeTaskCount;

    /**
     * @brief ワーカースレッドのメインループ
     */
    void WorkerLoop(std::stop_token st);
  };
}