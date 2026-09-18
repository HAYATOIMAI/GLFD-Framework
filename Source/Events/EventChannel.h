#pragma once

/**
 * @file  EventChannel.h
 * @brief 型ごとのイベントキューと購読者の一覧
 *
 * @details
 *  ## 購読者は `std::function` ではない (ECS 2-1 / 論点1)
 *  **関数ポインタ + 文脈の `void*`** で持つ。理由は 3 つある。
 *
 *   1. **N-1**(§18.5: 所有・確保・例外を伴う型)に `std::function` が当たる。
 *      2-2 の監査で実測したとおり、捕捉が MSVC のインラインバッファを超えると
 *      **黙って確保し、`std::bad_alloc` を投げ得る**。関数ポインタなら
 *      構造的に起きない
 *   2. **`std::function` は比較できない。** 購読解除に要るのは同一性であり、
 *      比較できない型では「この購読を外す」が書けない
 *   3. 捕捉の大きさで挙動が変わらない。`[&s]` と `[=, &grid, &bus]` が
 *      別物になるような差が、購読の登録側に出なくなる
 *
 *  ## 購読の同一性は**バス全体で単調に増える通し番号**
 *  `SceneManager` が「シーンに入る直前の番号」を控え、抜けた後にそれ以降の
 *  購読が残っていれば**そのシーンの外し忘れ**だと判定する (2-1 / 論点5)。
 *  持ち主を記録しなくてよく、シーンを積んだ場合も自然に成り立つ。
 *
 *  ## 外すときは詰める。末尾と入れ替えない
 *  入れ替えると**残った購読者の配信順が変わる**。購読者は 1 チャネルあたり
 *  数件なので、詰める費用は問題にならない。
 */

#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"
#include "../Threading/LockFreeQueue.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace GLFD::Events {

  /**
   * @brief 購読の同一性 (ECS 2-1)
   *
   * @details
   *  `serial` は `EventBus` 全体で単調に増える。**0 は無効値**であり、
   *  `Subscribe` が確保に失敗したときに返る (N-2: 投げない)。
   *  `channel` は解除のときに探索を省くためのもので、値の正しさは
   *  `serial` 側だけで担保している(食い違えば単に見つからない)。
   */
  struct SubscriptionId {
    std::uint32_t channel = 0;
    std::uint32_t serial  = 0;

    [[nodiscard]] bool IsValid() const noexcept { return serial != 0u; }

    [[nodiscard]] friend bool operator==(SubscriptionId a, SubscriptionId b) noexcept {
      return a.channel == b.channel && a.serial == b.serial;
    }
  };

  /**
   * @brief イベントチャネルの基底クラス
   * @note  `EventBus` がリストで管理するために必要
   */
  class IEventChannel {
  public:
    virtual ~IEventChannel() = default;
    virtual void Dispatch() = 0;

    /**
     * @brief 発行と取りこぼしの観測 (ECS 1-7 / R-28)
     *
     * @details
     *  **キューが溢れたら黙って捨てていた。** それを知らせる printf が
     *  コメントアウトされていた。**診断は足すより消される方が簡単**であり、
     *  とりわけ 1-7 で衝突検出が動き始めると毎フレーム大量に出得るので、
     *  捨てたことを数えられる形にした。
     */
    [[nodiscard]] virtual std::uint32_t PublishedCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t DroppedCount() const noexcept = 0;
    virtual void ResetCounters() noexcept = 0;

    /// 購読者の数。**テストから数えられるようにする** (§4.12)
    [[nodiscard]] virtual std::size_t SubscriberCount() const noexcept = 0;
    /// 通し番号が `serial` 以上の購読者の数
    [[nodiscard]] virtual std::size_t SubscriberCountSince(std::uint32_t serial) const noexcept = 0;

    /// 1 件外す。見つからなければ false(二重解除と古いハンドルは安全)
    virtual bool Unsubscribe(std::uint32_t serial) noexcept = 0;
    /// 通し番号が `serial` 以上の購読者を全部外し、外した数を返す
    virtual std::size_t UnsubscribeSince(std::uint32_t serial) noexcept = 0;
  };

  /**
   * @brief 型 T 専用のイベントチャネル
   */
  template <typename T>
  class EventChannel : public IEventChannel {
  public:
    /// 購読者の本体。**捕捉を持たない**(ファイル冒頭の @details)
    using EventCallback = void (*)(void* context, const T& event);

    /**
     * @brief イベントキューの容量 (ECS 1-7 に 4096 から変更)
     *
     * @details
     *  **実測で決めた。** 1-7 で衝突検出が動き始めたときの発行数は、
     *  20,000 体の Boid デモで **1 フレームあたり 1,479 件(群れが散っている
     *  とき)から 4,686 件(群れがまとまったとき)**だった。4,096 では実際に
     *  溢れ、1 回で 590 件を捨てていた。
     *
     *  16,384 は観測した最大の約 3.5 倍。**この種の根拠は「上限を超えたら
     *  分かる」ことと組でしか意味がない**ので、数は数え続ける
     *  (`PublishedCount` / `DroppedCount`)。
     *
     *  @note 机上の見積もりでは 20,000 x MAX_CHECKS(16) = 320,000 件/フレームまで
     *        あり得たが、実際はその 1/68 だった。**この見積もりでキューを
     *        決めていたら 25 倍過大になっていた。**
     *
     *  @note 容量は全チャネル共通である。チャネルごとに変えたくなったら
     *        テンプレート引数にすること(今は要らない)
     */
    static constexpr std::size_t QUEUE_CAPACITY = 16384;

    explicit EventChannel(Memory::IMemoryResource* resource)
      : m_subscribers(resource) { }

    /**
     * @brief イベントをキューに積む (Thread-Safe, Lock-Free)
     * @note  ワーカースレッドから呼ばれる
     */
    void Publish(const T& event) {
      // キューが満杯なら捨てる。リアルタイムゲームでは
      // 「古いイベントを捨てる」も「諦める」も一般的。
      // **数えずに捨てない** (1-7 / R-28)。数えて上層が報告する
      m_published.fetch_add(1u, std::memory_order_relaxed);
      if (!m_queue.Push(event)) {
        m_dropped.fetch_add(1u, std::memory_order_relaxed);
      }
    }

    [[nodiscard]] std::uint32_t PublishedCount() const noexcept override {
      return m_published.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t DroppedCount() const noexcept override {
      return m_dropped.load(std::memory_order_relaxed);
    }
    void ResetCounters() noexcept override {
      m_published.store(0u, std::memory_order_relaxed);
      m_dropped.store(0u, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t SubscriberCount() const noexcept override {
      return m_subscribers.GetSize();
    }

    [[nodiscard]] std::size_t SubscriberCountSince(std::uint32_t serial) const noexcept override {
      std::size_t count = 0;
      for (std::size_t i = 0; i < m_subscribers.GetSize(); ++i) {
        if (m_subscribers[i].serial >= serial) { ++count; }
      }
      return count;
    }

    /**
     * @brief 購読者を登録 (Main Thread Only)
     * @return 確保に失敗したら false。**呼び出し側が確認すること** (N-2)
     */
    [[nodiscard]] bool Subscribe(EventCallback callback, void* context,
                                 std::uint32_t serial) noexcept {
      assert(!m_dispatching && "Subscribe called from inside Dispatch");
      assert(callback != nullptr && "Subscribe called with a null callback");
      return m_subscribers.TryPushBack(Subscriber{ callback, context, serial });
    }

    bool Unsubscribe(std::uint32_t serial) noexcept override {
      assert(!m_dispatching && "Unsubscribe called from inside Dispatch");
      for (std::size_t i = 0; i < m_subscribers.GetSize(); ++i) {
        if (m_subscribers[i].serial == serial) {
          m_subscribers.Erase(i);     // 詰める。**入れ替えない**(冒頭の @details)
          return true;
        }
      }
      return false;
    }

    std::size_t UnsubscribeSince(std::uint32_t serial) noexcept override {
      assert(!m_dispatching && "UnsubscribeSince called from inside Dispatch");
      std::size_t removed = 0;
      std::size_t i = 0;
      while (i < m_subscribers.GetSize()) {
        if (m_subscribers[i].serial >= serial) {
          m_subscribers.Erase(i);
          ++removed;
          continue;                   // 詰めたので同じ添字を見直す
        }
        ++i;
      }
      return removed;
    }

    /**
     * @brief 溜められたイベントを全購読者に配信 (Main Thread Only)
     *
     * @warning **配信中に購読を足したり外したりしてはならない。** 配列を
     *          詰めながら回ることになり、1 件飛ぶ。前提条件違反なので
     *          `assert` で止める (N-2)
     */
    void Dispatch() override {
      m_dispatching = true;
      while (auto eventOpt = m_queue.Pop()) {
        const T& event = *eventOpt;
        for (std::size_t i = 0; i < m_subscribers.GetSize(); ++i) {
          const Subscriber& s = m_subscribers[i];
          s.callback(s.context, event);
        }
      }
      m_dispatching = false;
    }

  private:
    struct Subscriber {
      EventCallback callback = nullptr;
      void*         context  = nullptr;
      std::uint32_t serial   = 0;
    };

    // LockFreeQueue: イベントのバッファ
    Thread::LockFreeQueue<T, QUEUE_CAPACITY> m_queue;

    /// 購読者リスト。**登録順を保つ**(外すときも詰めるだけ)
    DynamicArray<Subscriber> m_subscribers;

    /// 1-7: 観測用。**ワーカースレッドから増える**ので atomic
    std::atomic<std::uint32_t> m_published{ 0 };
    std::atomic<std::uint32_t> m_dropped{ 0 };

    /// 配信中か。`assert` 用であって振る舞いには使わない (2-1)
    bool m_dispatching = false;
  };
}
