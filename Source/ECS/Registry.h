#pragma once

#include "SparseSet.h"
#include "../Core/DynamicArray.h"
#include "../Core/TypeInfo.h"
#include <unordered_map>
#include <memory_resource>
#include <memory>
#include <atomic>

namespace GLFD::ECS {
  class Registry {
  public:
    explicit Registry(Memory::IMemoryResource* resource)
      : m_resource(resource), 
        m_pools(resource),
        m_typeMap(resource)
        {}

    ~Registry() {
      // 管理しているSparseSetを破棄
      for (size_t i = 0; i < m_pools.GetSize(); ++i) {
        ISparseSet* pool = m_pools[i];
        if (pool) {
          // デストラクタを手動呼び出し
          pool->~ISparseSet();
        }
      }
    }
    /**
     * @brief 新しいエンティティIDを発行
     */
    Entity CreateEntity() {
      // 簡易実装: IDをインクリメントするだけ
      // 削除されたIDの再利用（リサイクル）は今回は省略
      return m_nextEntityId++;
    }

    /**
     * @brief コンポーネントを追加
     */
    template <typename T, typename... Args>
    T& AddComponent(Entity entity, Args&&... args)
    {
      return Assure<T>()->Emplace(entity, std::forward<Args>(args)...);
    }

    /**
     * @brief コンポーネントを取得
     */
    template <typename T>
    T& GetComponent(Entity entity) { return Assure<T>()->Get(entity); }

    /**
     * @brief コンポーネントプール（View）を取得
     * Systemがループ処理するために使用
     */
    template <typename T>
    SparseSet<T>& View() { return *Assure<T>(); }

  private:
    Memory::IMemoryResource* m_resource;
    Entity m_nextEntityId = 0;

    // 型ID -> SparseSetへのポインタ
    DynamicArray<ISparseSet*> m_pools;

    struct TypeMapEntry {
      size_t typeHash;
      size_t poolIndex;
    };

    DynamicArray<TypeMapEntry> m_typeMap{ nullptr };

    /**
     * @brief 型 T に対応する SparseSet を取得（なければ作成）
     */
    template <typename T>
    SparseSet<T>* Assure() {
      // コンパイル時ハッシュを取得 (DLLまたいでも不変)
      constexpr size_t typeHash = TypeInfo::GetID<T>();

      // マップを検索 (線形探索)
      size_t poolIndex = size_t(-1);

      // DynamicArrayのイテレータを使って検索
      for (const auto& entry : m_typeMap) {
        if (entry.typeHash == typeHash) {
          poolIndex = entry.poolIndex;
          break;
        }
      }

      // 存在しなければ新規登録
      if (poolIndex == size_t(-1)) {
        poolIndex = m_pools.GetSize();

        // プール配列拡張
        size_t oldSize = m_pools.GetSize();
        m_pools.Resize(poolIndex + 1);
        for (size_t i = oldSize; i < m_pools.GetSize(); ++i) m_pools[i] = nullptr;

        // マップ登録
        m_typeMap.PushBack({ typeHash, poolIndex });
      }

      // プールインスタンス作成（まだなければ）
      if (!m_pools[poolIndex]) {
        void* buffer = m_resource->Allocate(sizeof(SparseSet<T>), alignof(SparseSet<T>));
        m_pools[poolIndex] = new(buffer) SparseSet<T>(m_resource);
      }

      return static_cast<SparseSet<T>*>(m_pools[poolIndex]);
    }
  };
} 