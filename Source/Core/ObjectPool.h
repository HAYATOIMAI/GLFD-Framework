#pragma once

#include "DynamicArray.h"
#include <functional>
#include <cassert>

namespace GLFD {
  template <typename T>
  class ObjectPool {
  public:
    using value_type = T;
    using pointer = T*;
    using reference = T&;

    /**
     * @brief コンストラクタ
     * @param resource メモリリソース（StackAllocatorなど）
     */
    explicit ObjectPool(Memory::IMemoryResource* resource = nullptr)
      : m_pool(resource)
      , m_activeCount(0) {}

    /**
     * @brief 指定した数だけ事前にメモリを確保し、オブジェクトをデフォルト構築します。
     * @param capacity 確保する最大数
     */
    void Reserve(size_t capacity) {
      m_pool.Reserve(capacity);
      // 事前に最大数までデフォルト構築してしまう（メモリ領域を埋める）
      // これにより Rent 時のコストを単なるポインタ移動だけにする
      m_pool.Resize(capacity);

      // 全て「非アクティブ」として扱うためカウントは0
      m_activeCount = 0;
    }

    /**
     * @brief オブジェクトを1つ借ります（Activeにします）。
     * @return 借りたオブジェクトへのポインタ。プールが満杯の場合は nullptr
     */
    pointer Rent() {
      if (m_activeCount >= m_pool.GetSize()) {

        // 容量不足時は拡張 (Resizeで要素が増え、デフォルト構築される)
        size_t newCap = (m_pool.GetCapacity() > 0) ? m_pool.GetCapacity() * 2 : 8;
        m_pool.Resize(newCap);
      }

      // 現在の境界線にあるオブジェクトを返す
      pointer ptr = &m_pool[m_activeCount];
      m_activeCount++;
      return ptr;
    }

    /**
     * @brief オブジェクトを借りて、引数付きで再初期化します。
     * @tparam Args 初期化引数の型
     * @param args 初期化引数
     * @return 借りたオブジェクトへのポインタ
     */
    template <typename... Args>
    pointer RentArgs(Args&&... args) {
      pointer ptr = Rent();
      if (ptr) {
        std::destroy_at(ptr);
        std::construct_at(ptr, std::forward<Args>(args)...);
      }
      return ptr;
    }

    /**
     * @brief オブジェクトを返却（解放）します。
     * @param obj 返却するオブジェクトへのポインタ
     */
    void Return(pointer obj) {
      assert(m_activeCount > 0);
      assert(obj >= m_pool.GetData() && obj < m_pool.GetData() + m_pool.GetSize());

      // アドレスからインデックスを逆算
      size_t index = obj - m_pool.GetData();

      // 既に非アクティブ領域にあるものを返していないかチェック
      assert(index < m_activeCount && "Object is already inactive!");

      // Swap-and-Pop 処理
      // 返却されたオブジェクトの位置に、Activeリストの「最後」のオブジェクトを移動（コピー/ムーブ）
      size_t lastIndex = m_activeCount - 1;

      if (index != lastIndex) {
        // 自分自身とのSwapでなければ入れ替え
        // 代入で中身を入れ替える（ムーブ代入が定義されていれば高速）
        m_pool[index] = std::move(m_pool[lastIndex]);
      }

      m_activeCount--;
    }

    /**
     * @brief 全てのActiveなオブジェクトに対して処理を行います。
     * @details メモリ連続性が保証されているため、最速のイテレーションが可能です。
     * @param func 実行する関数
     */
    template <typename Func>
    void ForEachActive(Func func) {
      // ポインタアクセスで最適化
      pointer data = m_pool.GetData();
      for (size_t i = 0; i < m_activeCount; ++i) {
        func(data[i]);
      }
    }
    /**
     * @brief 全オブジェクトを強制回収します。
     */
    void Reset() { m_activeCount = 0; }

    // 情報取得
    [[nodiscard]] size_t GetActiveCount() const { return m_activeCount; }
    [[nodiscard]] size_t GetCapacity() const { return m_pool.GetSize(); } // Resize済みサイズ＝実質容量
  private:
    DynamicArray<T> m_pool; // 実体配列
    size_t m_activeCount;   // どこまでがActiveかの境界線
  };
}