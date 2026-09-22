#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>
#include <functional>
#include <cassert>
#include "LockFreeQueue.h"
#include "../Core/HardwareConstants.h"


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
   * @brief 積まなかった / 実行しなかったジョブの件数(ECS 2-4)
   * @details **JobSystem は出力しない**(構造は下層、出力は上層)。std::cerr には書かない。
   *  上層(Engine)が毎フレームと Stop() の後に読んで Logger へ出す
   */
  struct JobSystemStats {
    std::uint32_t queueFullDrops = 0;      // キューが満杯のまま再試行の上限に達し、捨てた件数
    std::uint32_t rejectedAfterStop = 0;   // Stop() の開始後に KickJob された件数(積んでいない)
    std::uint32_t abandonedAtStop = 0;     // join の後にキューに残っていた件数(実行していない)
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
     * @details 構築したスレッドが所有者になる(Stop() / KickJob の呼び出し元検査に使う)
     */
    explicit JobSystem(unsigned int numThreads = 0);
    /**
     * デストラクタ
     * @details **必ず Stop() を呼ぶ。** join されていない std::thread は破棄されると terminate する
     */
    ~JobSystem();
    /**
     * @brief ジョブを投入する
     * @param job 実行する処理
     * @param parentHandle 依存関係を持たせたい親ハンドルのポインタ（任意）
     * @details
     *  - **呼んでよいのは所有スレッドとワーカー(実行中のジョブ)だけ**(Debug で assert)。
     *    それ以外のスレッドが Stop() と並行して積むと、回収の後に積まれたジョブを誰も数えられない
     *  - Stop() の開始後は積まない。親ハンドルの件数も増やさず、rejectedAfterStop に数える
     *  - キューが満杯のまま再試行の上限に達したら捨て、親ハンドルの件数を戻して queueFullDrops に数える
     *  - **ジョブは投げないこと**(N-2)。ワーカー上で投げれば std::terminate で落ちる
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
    /**
     * @brief ワーカーを止めて join し、キューに残ったジョブを回収する(ECS 2-4)
     * @details
     *  - 1 回目の呼び出しで join と回収まで終える。2 回目以降は何もしない
     *  - **所有スレッドから呼ぶこと**(Debug で assert)。ワーカーから呼ぶと自分を join する
     *  - 開始時点で実行中のジョブは最後まで実行する
     *  - join の後でキューに残っていたジョブは実行しない。親ハンドルの件数を減らして
     *    (WaitFor が戻れるように)abandonedAtStop に数える
     */
    void Stop();
    /**
     * @brief 停止の経路で実行しなかったジョブの件数
     */
    [[nodiscard]] JobSystemStats Stats() const;

  private:
    struct JobWrapper {
      JobFunction task;
      JobCounter* counter;
    };

    // LockFreeQueue (MPMC)
    LockFreeQueue<JobWrapper, 4096> m_queue;

    // **std::jthread ではなく std::thread**(ECS 2-4)。
    // 停止の経路は m_stopping と m_wakeGeneration の 1 本だけにする。jthread の
    // request_stop() は世代を変えないので、眠っているワーカーを起こせない。jthread の
    // ままだと Stop() を呼び忘れたとき、デストラクタの request_stop() + join() が
    // 修正前と同じ「黙ったハング」になる。std::thread なら、join されずに破棄されると
    // std::terminate で即座に落ちる。**落ちる方がハングより見つけやすい**(開発手法 4.4)。
    // ~JobSystem が必ず Stop() を呼ぶことは T-ECS-30 / T-ECS-31 が固定している
    std::vector<std::thread> m_workers;
    // KickJob の呼び出し元検査に使う。構築の後は読むだけ
    // (m_workers[i].get_id() は join で変わるので、Stop() と並行して読むと競合する)
    std::vector<std::thread::id> m_workerIds;
    std::thread::id m_owner = std::this_thread::get_id();   // 構築したスレッド

    std::atomic<bool> m_stopping{ false };
    // **ワーカーを起こす合図**(ECS 2-4)。件数ではなく世代で、投入と停止のたびに進める。
    // ワーカーは「世代を読む -> 停止とキューを確認 -> 読んだ世代で wait」の順に動くので、
    // 確認と wait の間に何が割り込んでも世代が変わっていて、wait は即座に戻る。
    // **待たれている値そのものを変えずに通知しても wait は戻らない**(修正前の欠陥 1-C H1)。
    // 64 ビットなので、1 回の確認のあいだに一周して同じ値に戻ることは無い
    std::atomic<std::uint64_t> m_wakeGeneration{ 0 };

    std::atomic<std::uint32_t> m_queueFullDrops{ 0 };
    std::atomic<std::uint32_t> m_rejectedAfterStop{ 0 };
    std::atomic<std::uint32_t> m_abandonedAtStop{ 0 };

    /**
     * @brief ジョブを実際に実行し、カウンターを減らす内部関数
     */
    void ExecuteJob(const JobWrapper& wrapper);

    void WorkerLoop();

    // 呼び出し元が所有スレッドかワーカーか(assert 用)
    [[nodiscard]] bool IsOwnerOrWorker() const;
  };
}
