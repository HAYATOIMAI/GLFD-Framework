#pragma once
#include "EventChannel.h"
#include "../Core/TypeInfo.h"
#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"
#include <shared_mutex>

namespace GLFD::Events {
  class EventBus {
  public:
    explicit EventBus(Memory::IMemoryResource* resource)
      : m_resource(resource)
      , m_channels(resource)
      , m_typeMap(resource) {}

    ~EventBus() {
      // ここでチャネルを破棄しているため、EventBusのコピーは厳禁！
     // コピー禁止対応済み
      for (size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i]) {
          m_channels[i]->~IEventChannel();
        }
      }
    }

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    /**
     * @brief イベントを発行する (Thread-Safe)
     */
    template <typename T>
    void Publish(const T& event) {
      Assure<T>()->Publish(event);
    }

    /**
     * @brief イベントを購読する (Setup Phase Only)
     */
    template <typename T>
    void Subscribe(std::function<void(const T&)> callback)
    {
      Assure<T>()->Subscribe(std::move(callback));
    }

    template <typename T>
    void Register() {
      Assure<T>(); // 事前に作成しておく
    }

    /**
     * @brief 全てのチャネルのイベントを配信する (Main Thread / Update Phase)
     */
    void DispatchAll() {
      std::shared_lock<std::shared_mutex> lock(m_mutex);

      for (size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i]) {
          m_channels[i]->Dispatch();
        }
      }
    }

  private:
    Memory::IMemoryResource* m_resource;

    // 全チャネルのリスト
    DynamicArray<IEventChannel*> m_channels;

    // TypeHash -> ChannelIndex のマップ
    struct TypeMapEntry {
      size_t typeHash;
      size_t index;
    };

    DynamicArray<TypeMapEntry> m_typeMap;
    // 【追加】書き込み/読み込みを制御するミューテックス
    std::shared_mutex m_mutex;

    /**
     * @brief 型 T に対応するチャネルを取得・作成
     */
    template <typename T>
    EventChannel<T>* Assure() {
      size_t typeHash = TypeInfo::GetID<T>();

      // 線形探索 (Registryと同じロジック)
      {
        std::shared_lock<std::shared_mutex> readLock(m_mutex);
        for (const auto& entry : m_typeMap) {
          if (entry.typeHash == typeHash) {
            return static_cast<EventChannel<T>*>(m_channels[entry.index]);
          }
        }
      }

      // 2. 見つからなかった場合、「書き込みロック」を取得して作成する
                  // Double-Checked Locking パターン
      std::unique_lock<std::shared_mutex> writeLock(m_mutex);

      // ロック取得の間にもう一度チェック（他のスレッドが先に作ったかもしれない）
      for (const auto& entry : m_typeMap) {
        if (entry.typeHash == typeHash) {
          return static_cast<EventChannel<T>*>(m_channels[entry.index]);
        }
      }

      // 本当にないので作成（ここでResizeが発生する可能性がある）
      size_t index = m_channels.GetSize();
      m_channels.PushBack(nullptr);
      m_typeMap.PushBack({ typeHash, index });

      void* buf = m_resource->Allocate(sizeof(EventChannel<T>), alignof(EventChannel<T>));
      m_channels[index] = new(buf) EventChannel<T>(m_resource);

      return static_cast<EventChannel<T>*>(m_channels[index]);
    }
  };
} // namespace GLFD::Event