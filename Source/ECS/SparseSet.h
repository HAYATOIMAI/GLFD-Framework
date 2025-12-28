#pragma once
#include "Entity.h"
#include "../Core/DynamicArray.h"
#include <cassert>
#include <limits>

namespace GLFD::ECS {
  /**
   * @brief 型消去のための基底クラス
   * Registryが異種混合のSparseSetをリストで管理するために必要
   */
  class ISparseSet {
  public:
    virtual ~ISparseSet() = default;
    virtual void Remove(Entity entity) = 0;
    virtual bool Has(Entity entity) const = 0;

    virtual size_t GetSize() const = 0;
    virtual Entity* GetEntityList() = 0; // Dense配列（Entity逆引き用）へのポインタ
  };

  /**
   * @brief データ型 T を管理する Sparse Set
   * @details
   * - Dense配列 : コンポーネントデータそのもの (イテレーション用)
   * - Sparse配列: エンティティIDからDense配列へのインデックス (ランダムアクセス用)
   */
  template <typename T>
  class SparseSet : public ISparseSet {
  public:
    explicit SparseSet(Memory::IMemoryResource* resource, size_t maxEntities = MaxEntities)
      : m_components(resource)
      , m_denseToEntity(resource) // DenseからEntityへの逆引き用
      , m_sparse(resource) {
      // Sparse配列は全エンティティID分確保し、無効値で埋める
      // スタックアロケータなら一瞬で終わるが、ヒープだと少し重いので注意
      m_sparse.Resize(maxEntities);

      // 初期化: 全て NullIndex
      for (size_t i = 0; i < maxEntities; ++i) {
        m_sparse[i] = NullIndex;
      }
    }

    /**
     * @brief コンポーネントを追加 (Emplace)
     */
    template <typename... Args>
    T& Emplace(Entity entity, Args&&... args) {
      assert(entity < m_sparse.GetSize() && "Entity ID out of range");
      assert(m_sparse[entity] == NullIndex && "Component already exists");

      // 1. Dense配列の末尾にコンポーネントを追加
      m_components.EmplaceBack(std::forward<Args>(args)...);

      // 2. 逆引き用配列にもEntityIDを追加 (Swap-and-Pop時に必要)
      m_denseToEntity.PushBack(entity);

      // 3. Sparse配列に「Dense配列のインデックス(末尾)」を記録
      size_t denseIndex = m_components.GetSize() - 1;
      m_sparse[entity] = denseIndex;

      return m_components.Back();
    }

    /**
     * @brief コンポーネントを削除 (Remove)
     * @details Swap-and-Pop を使用して O(1) で削除し、密な状態を維持する
     */
    void Remove(Entity entity) override {
      assert(Has(entity));

      size_t indexToRemove = m_sparse[entity];
      size_t lastIndex = m_components.GetSize() - 1;

      // 削除対象が末尾でない場合、末尾の要素を持ってきて穴埋めする
      if (indexToRemove != lastIndex) {
        // 末尾にあるコンポーネントとその所有エンティティIDを取得
        Entity lastEntity = m_denseToEntity[lastIndex];

        // 1. コンポーネントを移動 (Swap or Move)
        m_components[indexToRemove] = std::move(m_components[lastIndex]);

        // 2. 逆引き配列も更新
        m_denseToEntity[indexToRemove] = lastEntity;

        // 3. 移動してきたエンティティ(lastEntity)のSparse情報を更新
        m_sparse[lastEntity] = indexToRemove;
      }

      // 末尾を削除
      m_components.PopBack();
      m_denseToEntity.PopBack();

      // 削除されたエンティティのSparse情報を無効化
      m_sparse[entity] = NullIndex;
    }

    bool Has(Entity entity) const override {
      return entity < m_sparse.GetSize() && m_sparse[entity] != NullIndex;
    }

    T& Get(Entity entity) {
      assert(Has(entity));
      return m_components[m_sparse[entity]];
    }

    // オーバーライド実装
    size_t GetSize() const override { return m_components.GetSize(); }

    Entity* GetEntityList() override {
      // m_denseToEntity は DynamicArray<Entity>
      return m_denseToEntity.GetData();
    }

    // --- イテレータ (Systemが回す用) ---
    // Dense配列への直接アクセスを提供するので爆速
    T* GetData() { return m_components.GetData(); }

    // 範囲for文用
    auto begin() { return m_components.begin(); }
    auto end() { return m_components.end(); }

  private:
    static constexpr size_t NullIndex = SIZE_MAX;

    // 密 (Dense): データ本体
    DynamicArray<T> m_components;
    // 密 (Dense): コンポーネント配列と同じ並び順のエンティティID (逆引き用)
    DynamicArray<Entity> m_denseToEntity;

    // 疎 (Sparse): EntityID -> DenseIndex
    DynamicArray<size_t> m_sparse;
  };
}