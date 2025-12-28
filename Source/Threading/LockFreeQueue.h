#pragma once

#include <atomic>
#include <optional>
#include <cassert>
#include <thread>
#include <memory>
#include "../Core/HardwareConstants.h"

namespace GLFD::Thread {
  /**
   * @brief ロックフリーキュークラス
   * @tparam T キューに格納する要素の型
   * @tparam Capacity キューの最大容量（固定サイズ）
   */
  template <typename T, size_t Capacity>
  class LockFreeQueue {
    // 2の累乗チェック (コンパイル時)
    static_assert((Capacity != 0) && ((Capacity& (Capacity - 1)) == 0), "Capacity must be power of 2");
  public:
    LockFreeQueue() {

      m_slots = new Slot[Capacity];

      // 最初は全てのスロットを「空」に初期化
      for (size_t i = 0; i < Capacity; ++i) {
        m_slots[i].sequence.store(i, std::memory_order_relaxed);
      }
    }

    // デストラクタで確実に解放
    ~LockFreeQueue() {
      if (m_slots) {
        delete[] m_slots;
        m_slots = nullptr;
      }
    }

    // コピー・ムーブ禁止
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

    /**
     * @brief 要素をキューに追加します (Push / Enqueue)
     * @param data 追加するデータ
     * @return 追加に成功したら true, 満杯なら false
     */
    [[nodiscard]] bool Push(const T& data) {
      // const参照ではなく値渡し(or ムーブ)用にテンプレート修正も検討可能だが、今回はシンプルに
      size_t head = m_head.load(std::memory_order_relaxed);

      while (true) {
        Slot& slot = m_slots[head & m_mask];
        size_t seq = slot.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(head);

        if (diff == 0) {
          // シーケンス番号が一致＝このスロットは書き込み可能
          // headを1進めることを試みる (CAS)
          if (m_head.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
            // 成功したらデータを書き込む
            slot.data = data;
            // シーケンス番号を更新して、「読み込み可能」状態にする
            // 元のseq(head) + 1 にすることで、Pop側が検知できるようにする
            slot.sequence.store(head + 1, std::memory_order_release);
            return true;
          }
          // CAS失敗：他のスレッドが先に書き込んだので、ループして再試行
        }
        else if (diff < 0) {
          // シーケンス番号が古い＝キューが満杯で周回が追いついていない
          return false;
        }
        else {
          // diff > 0: headが進んでしまった（他のスレッドのCASが成功した）
          // 最新のheadを再ロードしてリトライ
          head = m_head.load(std::memory_order_relaxed);
        }
      }
    }

    /**
     * @brief 要素をキューから取り出します (Pop / Dequeue)
     * @return データが存在すれば std::optional<T> に包んで返す
     */
    [[nodiscard]] std::optional<T> Pop() {
      size_t tail = m_tail.load(std::memory_order_relaxed);

      while (true) {
        Slot& slot = m_slots[tail & m_mask];
        size_t seq = slot.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail + 1);

        if (diff == 0) {
          // シーケンス番号が (tail + 1) と一致＝データが書き込まれており、読み込み可能
          // tailを1進めることを試みる (CAS)
          if (m_tail.compare_exchange_weak(tail, tail + 1, std::memory_order_relaxed)) {
            // データを読み出す
            T result = slot.data;
            // シーケンス番号を更新して、「次の周回まで空き」状態にする
            // 容量分足すことで、次にこのインデックスが使われるのは1周後になる
            slot.sequence.store(tail + m_mask + 1, std::memory_order_release);
            return result;
          }
        }
        else if (diff < 0) {
          // データがまだ書き込まれていない（空）
          return std::nullopt;
        }
        else {
          // diff > 0: 他のスレッドが先にPopした
          tail = m_tail.load(std::memory_order_relaxed);
        }
      }
    }

    [[nodiscard]] bool IsEmpty() const {
      // 厳密な判定ではないが、簡易チェック用
      return m_head.load(std::memory_order_relaxed) == m_tail.load(std::memory_order_relaxed);
    }

  private:
    struct Slot {
      std::atomic<size_t> sequence;
      T data;
    };

    static constexpr size_t m_mask = Capacity - 1;

    // 偽共有 (False Sharing) を防ぐためのパディング
    // キャッシュラインサイズでパディングし、False Sharingを防止
    alignas(System::Constants::CacheLineSize) std::atomic<size_t> m_head{ 0 };
    alignas(System::Constants::CacheLineSize) std::atomic<size_t> m_tail{ 0 };

    Slot* m_slots = nullptr;
  };
}