#pragma once

#include <tuple>
#include <algorithm>
#include <array>
#include "Registry.h"

namespace GLFD::ECS {
  template <typename... Components>
  class View {
  public:
    // イテレータの中身
    struct Iterator {
      // 最小プールのイテレータ
      // SparseSet内部のDense配列(Entity*)へのポインタを使う
      using BaseIterator = Entity*;

      BaseIterator current;
      BaseIterator end;
      View* view;

      Iterator(BaseIterator c, BaseIterator e, View* v)
        : current(c), end(e), view(v)
      {
        // 最初から条件を満たさないものをスキップ
        if (current != end && !view->IsValid(*current)) {
          ++(*this);
        }
      }

      // 前置インクリメント
      Iterator& operator++()
      {
        // 次の有効なエンティティまで進める
        do {
          ++current;
        } while (current != end && !view->IsValid(*current));
        return *this;
      }

      bool operator!=(const Iterator& other) const { return current != other.current; }

      // デリファレンスするとEntityIDを返す
      Entity operator*() const { return *current; }
    };

    /**
     * @brief コンストラクタ
     * @param registry レジストリへの参照
     */
    View(Registry& registry) : m_registry(registry)
    {
      // 全てのプールを取得
      // パラメータパック展開を使用して、コンパイル時に配列を初期化
      // ヒープ確保は一切発生しない
      size_t i = 0;
      ((m_pools[i++] = &registry.View<Components>()), ...);

      // 最小プールを探す
      m_smallestPool = m_pools[0];
      for (auto* pool : m_pools) {
        if (pool->GetSize() < m_smallestPool->GetSize()) {
          m_smallestPool = pool;
        }
      }
    }

    // 範囲for文用
    Iterator begin() {
      Entity* data = m_smallestPool->GetEntityList();
      size_t count = m_smallestPool->GetSize();
      return Iterator(data, data + count, this);
    }

    Iterator end() {
      Entity* data = m_smallestPool->GetEntityList();
      size_t count = m_smallestPool->GetSize();
      return Iterator(data + count, data + count, this);
    }

    /**
     * @brief 指定したコンポーネントを取得するヘルパー
     */
    template <typename T>
    T& Get(Entity entity)
    {
      // 高速化のため、Registry経由ではなく保持しているプールから直接取ることも可能だが、
      // ここでは安全にRegistry経由（または保持プール検索）する。
      return m_registry.GetComponent<T>(entity);
    }

  private:
    Registry& m_registry;

    std::array<ISparseSet*, sizeof...(Components)> m_pools;
    
    ISparseSet* m_smallestPool = nullptr;

    /**
     * @brief エンティティが全ての対象コンポーネントを持っているかチェック
     */
    bool IsValid(Entity entity) const
    {
      for (auto* pool : m_pools) {
        if (!pool->Has(entity)) {
          return false;
        }
      }
      return true;
    }
  };
} 