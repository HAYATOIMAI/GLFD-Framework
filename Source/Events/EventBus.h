#pragma once

/**
 * @file  EventBus.h
 * @brief 型ごとのチャネルを束ねるイベントバス
 *
 * @details
 *  ## 購読は外せる (ECS 2-1 / 論点1)
 *  1-8 まで `Subscribe` は配列の末尾に足すだけで、外す手段が無かった。
 *  `BoidDemoScene` は `[this]` を掴んだ購読を登録するので、**シーンを抜けると
 *  次の配信が解放済みのシーンを呼ぶ**(use-after-free)。シーン遷移を配線する
 *  前にここを直した。
 *
 *  - `Subscribe` は `SubscriptionId` を返す。**確保に失敗したら無効値**(N-2)
 *  - `Unsubscribe(id)` は 1 件外す。二重解除も古いハンドルも `false` が返るだけ
 *  - `UnsubscribeSince(serial)` は**ある時点以降に足された購読を全部外す**。
 *    `SceneManager` の安全網がこれを使う (論点5)
 *  - `NextSerial()` がその「ある時点」を取る手段である
 *
 *  ## 数えられる (§4.12 / §6.6)
 *  `SubscriberCount()` を公開する。**購読が外れたことを `assert` では
 *  確かめられない**(発火すると `abort` する)ので、テストは数を読む。
 */

#include "EventChannel.h"
#include "../Core/TypeInfo.h"
#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>

namespace GLFD::Events {

  /// 1 フレームに何件発行され、何件取りこぼしたか (ECS 1-7 / R-28)
  struct BusCounters {
    std::uint32_t published = 0;
    std::uint32_t dropped   = 0;
  };

  class EventBus {
  public:
    explicit EventBus(Memory::IMemoryResource* resource)
      : m_resource(resource)
      , m_channels(resource)
      , m_typeMap(resource) {}

    ~EventBus() {
      // ここでチャネルを破棄しているため、EventBus のコピーは厳禁!
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
     *
     * @note  チャネルを作れなかった場合(確保失敗)は**捨てた 1 件として
     *        数える**。黙って消さない (R-28)
     */
    template <typename T>
    void Publish(const T& event) {
      if (EventChannel<T>* const channel = Assure<T>()) {
        channel->Publish(event);
        return;
      }
      m_unassuredDrops.fetch_add(1u, std::memory_order_relaxed);
    }

    /**
     * @brief イベントを購読する (Setup Phase Only)
     *
     * @param callback 捕捉を持たない関数ポインタ。**`std::function` ではない**
     *                 (`EventChannel.h` 冒頭の @details)
     * @param context  `callback` の第 1 引数にそのまま渡る。購読者の状態
     * @return 外すためのハンドル。**確保に失敗したら無効値**(N-2: 投げない)
     *
     * @warning **戻り値を捨ててよいのは「二度と外さない」ときだけである。**
     *          シーンは必ず控えて `OnExit` で外すこと (`IScene.h` の契約)
     */
    template <typename T>
    [[nodiscard]] SubscriptionId Subscribe(void* context,
                                           typename EventChannel<T>::EventCallback callback) {
      EventChannel<T>* const channel = Assure<T>();
      if (channel == nullptr) { return SubscriptionId{}; }

      const std::uint32_t serial = m_nextSerial;
      if (!channel->Subscribe(callback, context, serial)) {
        return SubscriptionId{};      // 確保失敗。番号は進めない
      }
      ++m_nextSerial;
      return SubscriptionId{ FindChannelIndex<T>(), serial };
    }

    /// 型 T のチャネルを先に作っておく。@return 確保に失敗したら false
    template <typename T>
    [[nodiscard]] bool Register() {
      return Assure<T>() != nullptr;
    }

    /**
     * @brief 購読を 1 件外す
     * @return 外れたら true。**見つからなくても安全**(二重解除・古いハンドル)
     */
    bool Unsubscribe(SubscriptionId id) noexcept {
      if (!id.IsValid()) { return false; }
      std::shared_lock<std::shared_mutex> lock(m_mutex);
      if (id.channel < m_channels.GetSize() && m_channels[id.channel] != nullptr) {
        if (m_channels[id.channel]->Unsubscribe(id.serial)) { return true; }
      }
      // ハンドルのチャネル添字が当てにならない場合への保険。**正しさは
      // 通し番号だけで担保している**ので、全チャネルを見れば必ず見つかる
      for (std::size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i] != nullptr && m_channels[i]->Unsubscribe(id.serial)) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief 次に配られる通し番号 (ECS 2-1 / 論点5)
     * @note  シーンに入る直前にこれを控えておくと、抜けた後に
     *        「それ以降に足された購読」= そのシーンが足したものが分かる
     */
    [[nodiscard]] std::uint32_t NextSerial() const noexcept { return m_nextSerial; }

    /// `serial` 以降に足された購読を全部外し、外した数を返す(安全網)
    std::size_t UnsubscribeSince(std::uint32_t serial) noexcept {
      std::shared_lock<std::shared_mutex> lock(m_mutex);
      std::size_t removed = 0;
      for (std::size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i] != nullptr) {
          removed += m_channels[i]->UnsubscribeSince(serial);
        }
      }
      return removed;
    }

    /// 購読者の総数 (§4.12: `assert` では確かめられないので数を公開する)
    [[nodiscard]] std::size_t SubscriberCount() const noexcept {
      std::shared_lock<std::shared_mutex> lock(m_mutex);
      std::size_t count = 0;
      for (std::size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i] != nullptr) { count += m_channels[i]->SubscriberCount(); }
      }
      return count;
    }

    /// `serial` 以降に足された購読者の数
    [[nodiscard]] std::size_t SubscriberCountSince(std::uint32_t serial) const noexcept {
      std::shared_lock<std::shared_mutex> lock(m_mutex);
      std::size_t count = 0;
      for (std::size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i] != nullptr) {
          count += m_channels[i]->SubscriberCountSince(serial);
        }
      }
      return count;
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

    /**
     * @brief 全チャネルの発行数と取りこぼし数 (1-7 / R-28)
     * @note  **`ResetCounters` を呼ぶまで積み上がる。** 呼び出し側が
     *        フレームの区切りで読んで戻す
     */
    [[nodiscard]] BusCounters Counters() const {
      std::shared_lock<std::shared_mutex> lock(m_mutex);
      BusCounters out;
      for (size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i]) {
          out.published += m_channels[i]->PublishedCount();
          out.dropped   += m_channels[i]->DroppedCount();
        }
      }
      // チャネルを作れずに捨てた分も**取りこぼしに数える**
      out.dropped += m_unassuredDrops.load(std::memory_order_relaxed);
      return out;
    }

    /// @copydoc Counters
    void ResetCounters() {
      std::shared_lock<std::shared_mutex> lock(m_mutex);
      for (size_t i = 0; i < m_channels.GetSize(); ++i) {
        if (m_channels[i]) {
          m_channels[i]->ResetCounters();
        }
      }
      m_unassuredDrops.store(0u, std::memory_order_relaxed);
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
    mutable std::shared_mutex m_mutex;

    /// 次に配る購読の通し番号。**0 は無効値なので 1 から始める**
    std::uint32_t m_nextSerial = 1;

    /// チャネルを作れずに捨てた発行の数 (2-1 / R-28)
    std::atomic<std::uint32_t> m_unassuredDrops{ 0 };

    /// 型 T のチャネル添字。無ければ 0(`Subscribe` が成功した後にしか呼ばない)
    template <typename T>
    [[nodiscard]] std::uint32_t FindChannelIndex() const noexcept {
      const size_t typeHash = TypeInfo::GetID<T>();
      std::shared_lock<std::shared_mutex> readLock(m_mutex);
      for (const auto& entry : m_typeMap) {
        if (entry.typeHash == typeHash) { return static_cast<std::uint32_t>(entry.index); }
      }
      return 0;
    }

    /**
     * @brief 型 T に対応するチャネルを取得・作成
     *
     * @return 確保に失敗したら nullptr。**呼び出し側が確認すること**
     *
     * @note  1-8 まで `m_resource->Allocate` の戻り値を確認せずに placement new
     *        していた。確保に失敗すると nullptr へ構築して未定義動作になる。
     *        配列も投擲版の `PushBack` を呼んでいた (N-2 違反)。2-1 で両方直した
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

      // ロック取得の間にもう一度チェック(他のスレッドが先に作ったかもしれない)
      for (const auto& entry : m_typeMap) {
        if (entry.typeHash == typeHash) {
          return static_cast<EventChannel<T>*>(m_channels[entry.index]);
        }
      }

      // **確保を先に済ませ、成功してから配列へ載せる。** 逆順にすると、
      // 載せた後の確保に失敗したときに nullptr のチャネルが残る
      void* const buf = m_resource->Allocate(sizeof(EventChannel<T>), alignof(EventChannel<T>));
      if (buf == nullptr) { return nullptr; }

      const size_t index = m_channels.GetSize();
      if (!m_channels.TryPushBack(nullptr)) {
        m_resource->Deallocate(buf, sizeof(EventChannel<T>), alignof(EventChannel<T>));
        return nullptr;
      }
      if (!m_typeMap.TryPushBack(TypeMapEntry{ typeHash, index })) {
        m_channels.PopBack();
        m_resource->Deallocate(buf, sizeof(EventChannel<T>), alignof(EventChannel<T>));
        return nullptr;
      }

      m_channels[index] = new(buf) EventChannel<T>(m_resource);
      return static_cast<EventChannel<T>*>(m_channels[index]);
    }
  };
} // namespace GLFD::Event
