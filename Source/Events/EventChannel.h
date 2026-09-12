#pragma once
#include "../Threading/LockFreeQueue.h"
#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace GLFD::Events {
  /**
   * @brief イベントチャネルの基底クラス
   * EventBusがリストで管理するために必要
   */
  class IEventChannel {
  public:
    virtual ~IEventChannel() = default;
    virtual void Dispatch() = 0;

    /**
     * @brief 発行と取りこぼしの観測 (ECS 1-7 / R-28)
     *
     * @details
     *  **キューが溢れたら黙って捨てていた。** しかも知らせる printf は
     *  コメントアウトされていた。**診断は足すより消される方が簡単**である、
     *  という実例。1-7 で衝突検出が動き始めると毎フレーム溢れ得るので、
     *  捨てたことが分かる形にする。
     */
    [[nodiscard]] virtual std::uint32_t PublishedCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t DroppedCount() const noexcept = 0;
    virtual void ResetCounters() noexcept = 0;
  };

  /**
   * @brief 型 T 専用のイベントチャネル
   */
  template <typename T>
  class EventChannel : public IEventChannel {
  public:
    using EventCallback = std::function<void(const T&)>;

    /**
     * @brief イベントキューの容量 (ECS 1-7 で 4096 から変更)
     *
     * @details
     *  **実測で決めた。** 1-7 で衝突検出が動き始めたときの発行数は、
     *  20,000 体の Boid デモで **1 フレームあたり 1,479 件(群れが散っている
     *  初期)から 4,686 件(群れが固まったとき)** だった。4,096 では実際に
     *  溢れ、1 回で 590 件を捨てた。
     *
     *  16,384 は観測した最大の約 3.5 倍。**上限の根拠は「上限を超えたら
     *  分かる」ことと組でしか成立しない**ので、溢れは数え続ける
     *  (`PublishedCount` / `DroppedCount`)。
     *
     *  @note 見積もりでは 20,000 x MAX_CHECKS(16) = 320,000 件/フレームまで
     *        あり得たが、実際はその 1/68 だった。**上限の見積もりでキューを
     *        決めていたら 25 倍過大になっていた。**
     *
     *  @note ここは全チャネル共通である。チャネルごとに変えたくなったら
     *        テンプレート引数にすること(今は要らない)
     */
    static constexpr size_t QUEUE_CAPACITY = 16384;

    explicit EventChannel(Memory::IMemoryResource* resource)
      : m_subscribers(resource)  { }

    /**
     * @brief イベントをキューに積む (Thread-Safe, Lock-Free)
     * ワーカースレッドから呼ばれる
     */
    void Publish(const T& event)
    {
      // キューが満杯なら捨てる。リアルタイムゲームでは
      // 「古いイベントを捨てる」か「諦める」が一般的。
      // **ただし黙って捨てない** (1-7 / R-28)。数えて上層が報告する
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

    /**
     * @brief 購読者を登録 (Main Thread Only)
     */
    void Subscribe(EventCallback callback) {
      // std::function はムーブ可能。
      // DynamicArray がオブジェクトを適切に管理できる前提
      // ※ DynamicArray の PushBack(T&&) が実装済みであること
      m_subscribers.PushBack(std::move(callback));
    }

    /**
     * @brief 蓄積されたイベントを全購読者に配信 (Main Thread Only)
     */
    void Dispatch() override {
      // キューから全てのイベントを取り出す
      while (auto eventOpt = m_queue.Pop()) {
        const T& event = *eventOpt;

        // 全ての購読者に通知
        // DynamicArrayのイテレータを使用
        for (auto& callback : m_subscribers) {
          callback(event);
        }
      }
    }

  private:
    // LockFreeQueue: イベントのバッファ
    Thread::LockFreeQueue<T, QUEUE_CAPACITY> m_queue;

    // 購読者リスト: コールバック関数の配列
    // ここは頻繁に変更されないため、DynamicArrayで管理
    DynamicArray<EventCallback> m_subscribers;

    /// 1-7: 観測用。**ワーカースレッドから増える**ので atomic
    std::atomic<std::uint32_t> m_published{ 0 };
    std::atomic<std::uint32_t> m_dropped{ 0 };
  };
}
