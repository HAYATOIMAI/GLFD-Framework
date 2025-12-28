#pragma once
#include "../Threading/LockFreeQueue.h"
#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"
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
  };

  /**
   * @brief 型 T 専用のイベントチャネル
   */
  template <typename T>
  class EventChannel : public IEventChannel {
  public:
    using EventCallback = std::function<void(const T&)>;

    // イベントキューの容量。あふれる場合は増やすか、リングバッファの連鎖などを検討。
    static constexpr size_t QUEUE_CAPACITY = 4096;

    explicit EventChannel(Memory::IMemoryResource* resource)
      : m_subscribers(resource)  { }

    /**
     * @brief イベントをキューに積む (Thread-Safe, Lock-Free)
     * ワーカースレッドから呼ばれる
     */
    void Publish(const T& event)
    {
      // キューが満杯なら、残念ながらドロップするか、警告を出す
      // リアルタイムゲームでは「古いイベントを捨てる」か「諦める」が一般的
      if (!m_queue.Push(event)) {
        // printf("Event Queue Overflow!\n");
      }
    }

    /**
     * @brief 購読者を登録 (Main Thread Only)
     */
    void Subscribe(EventCallback callback) {
      // std::function はムーブ可能
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
  };
}
