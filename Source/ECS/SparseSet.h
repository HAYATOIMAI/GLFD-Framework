#pragma once

#include "Entity.h"
#include "../Core/DynamicArray.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace GLFD::ECS {

  /**
   * @brief 型消去のための基底クラス
   *
   * @details
   *  `Registry` が異種混合の `SparseSet` をまとめて持つために要る。
   *  **`Registry` / `View` 以外から到達させないこと** (R-19)。
   */
  class ISparseSet {
  public:
    virtual ~ISparseSet() = default;

    /**
     * @brief `entity` の成分を取り除く。**持っていなければ何もしない**
     *
     * @note **1-1 で契約を変えた。** 以前は先頭に `assert(Has(entity));` があり、
     *       「持っていること」を呼び出し側の前提にしていた。
     *       **呼び出し元が 0 件だった**ので変更は安全である。
     *
     *       変えた理由: `Registry::DestroyEntity` は全プールを回るが、
     *       `if (pool->Has(e)) { pool->Remove(e); }` と書くと**仮想呼び出しが
     *       プールあたり 2 回**になる。「不在なら何もしない」にすれば 1 回で済む。
     *
     *       これは §3.1(`SparseSet` に無効な `Entity` を弾かせない)に反しない。
     *       有効性 (`IsAlive`) の検査は `Registry` が持ち、
     *       「**このプールが成分を持っているか**」はプールにしか答えられない
     *       別の問いだからである。
     */
    virtual void Remove(Entity entity) = 0;

    [[nodiscard]] virtual bool        Has(Entity entity) const = 0;
    [[nodiscard]] virtual std::size_t GetSize() const = 0;
    [[nodiscard]] virtual Entity*     GetEntityList() = 0;
  };

  /**
   * @brief データ型 T を管理する Sparse Set
   *
   * @details
   *   - dense: コンポーネント本体 (`m_components`) と、同じ並びの
   *     所有者ハンドル (`m_denseToEntity`)
   *   - sparse: **`Entity::Index()` から dense 添字を引く** (R-12)。
   *     要素は `uint32_t`(1-2)。格納するのは dense の添字で、上限は
   *     `MaxEntities` なので 32 bit で足りる。`size_t` の半分で済む
   *
   *  @note 逆引きは `Entity`(64bit)を丸ごと持つ (R-11)。index だけに落とすと
   *        generation が失われ、`IsAlive` が成立しない。
   *
   *  @warning **swap-and-pop なので、1 回の削除で dense の順序が変わる** (R-16)。
   *           **順序に依存するシステムを書いてはならない。** これは規約であり
   *           機械的には検証できない。T-ECS-6 が「順序が変わっても結果が
   *           変わらない」ことで裏を取る。
   */
  template <typename T>
  class SparseSet : public ISparseSet {
  public:
    /**
     * @brief 疎配列を `maxEntities` 分だけ確保する
     * @note  **確保に失敗しても投げない** (R-14 / N-2)。失敗したら
     *        `IsReady()` が false になり、`TryEmplace` が常に `nullptr` を返す。
     *        コンストラクタは失敗を戻り値で返せないので、この形にしてある
     */
    explicit SparseSet(Memory::IMemoryResource* resource,
                       std::size_t maxEntities = MaxEntities)
      : m_components(resource)
      , m_denseToEntity(resource)
      , m_sparse(resource) {
      if (!m_sparse.TryResize(maxEntities)) {
        return;                       // m_ready は false のまま
      }
      for (std::size_t i = 0; i < maxEntities; ++i) {
        m_sparse[i] = kNullIndex;
      }
      static_assert(MaxEntities <= 0xFFFFFFFEu,
                    "the sparse array stores dense indices as uint32_t and reserves "
                    "0xFFFFFFFF as the 'absent' sentinel. If MaxEntities could reach "
                    "0xFFFFFFFF, a real index would be indistinguishable from absent. "
                    "Widen m_sparse before raising MaxEntities past this.");
      m_ready = true;
    }

    /// 疎配列を確保できたか。false なら `TryEmplace` は常に失敗する
    [[nodiscard]] bool IsReady() const noexcept { return m_ready; }

    /**
     * @brief 成分を追加する
     * @return 追加した成分。**失敗したら `nullptr`**
     *
     * @note 失敗するのは「既に持っている」「index が範囲外」「確保に失敗した」の
     *       3 つ。**投げない** (N-2)。有効な `Entity` かどうかは `Registry` が
     *       先に見ている前提で、ここは `assert` だけを持つ (§3.1)
     */
    template <typename... Args>
    [[nodiscard]] T* TryEmplace(Entity entity, Args&&... args) {
      assert(entity.IsValid() && "SparseSet::TryEmplace: the Registry must reject invalid handles");

      const std::size_t index = entity.Index();
      if (!m_ready || index >= m_sparse.GetSize() || m_sparse[index] != kNullIndex) {
        return nullptr;
      }

      // **逆引きを先に伸ばす。** 成分を構築してから失敗すると、
      // 作ったものを壊して戻す後始末が要る
      if (!m_denseToEntity.TryPushBack(entity)) {
        return nullptr;
      }
      T* const created = m_components.TryEmplaceBack(std::forward<Args>(args)...);
      if (created == nullptr) {
        m_denseToEntity.PopBack();
        return nullptr;
      }

      // dense の添字は `MaxEntities` を超えないので 32 bit に収まる
      // (上の static_assert がその条件を守っている)
      m_sparse[index] = static_cast<std::uint32_t>(m_components.GetSize() - 1);
      return created;
    }

    /// @copydoc ISparseSet::Remove
    void Remove(Entity entity) override {
      if (!Has(entity)) {
        return;                      // **不在なら何もしない**(契約は基底の @note)
      }

      const std::size_t indexToRemove = m_sparse[entity.Index()];
      const std::size_t lastIndex     = m_components.GetSize() - 1;

      if (indexToRemove != lastIndex) {
        // 末尾を穴へ持ってくる (swap-and-pop)
        const Entity lastEntity = m_denseToEntity[lastIndex];
        m_components[indexToRemove]    = std::move(m_components[lastIndex]);
        m_denseToEntity[indexToRemove] = lastEntity;
        // 詰め先の添字も 32 bit に収まる(コンストラクタの static_assert)
        m_sparse[lastEntity.Index()]   = static_cast<std::uint32_t>(indexToRemove);
      }

      m_components.PopBack();
      m_denseToEntity.PopBack();
      m_sparse[entity.Index()] = kNullIndex;
    }

    [[nodiscard]] bool Has(Entity entity) const override {
      if (!m_ready || !entity.IsValid()) {
        return false;
      }
      const std::size_t index = entity.Index();
      return index < m_sparse.GetSize() && m_sparse[index] != kNullIndex;
    }

    /**
     * @return 成分。**持っていなければ `nullptr`**
     *
     * @note **疎配列を 1 回しか読まない** (1-5)。以前は `Has()` を通してから
     *       改めて `m_sparse[...]` を引いており、同じ場所を 2 回読んでいた。
     *       `View` の反復と近傍探索がこの関数を毎フレーム数十万回呼ぶので、
     *       ここは 1 回で済ませる。**意味は変えていない。**
     */
    [[nodiscard]] T* Find(Entity entity) {
      const std::uint32_t dense = DenseIndexOf(entity);
      return (dense != kNullIndex) ? &m_components[dense] : nullptr;
    }
    /// @copydoc Find
    [[nodiscard]] const T* Find(Entity entity) const {
      const std::uint32_t dense = DenseIndexOf(entity);
      return (dense != kNullIndex) ? &m_components[dense] : nullptr;
    }

    [[nodiscard]] std::size_t GetSize() const override { return m_components.GetSize(); }

    [[nodiscard]] Entity* GetEntityList() override { return m_denseToEntity.GetData(); }

    // --- dense への直接アクセス (R-13: const 版を対で持つ) --------------------
    [[nodiscard]] T*       GetData()       { return m_components.GetData(); }
    [[nodiscard]] const T* GetData() const { return m_components.GetData(); }

    [[nodiscard]] auto begin()       { return m_components.begin(); }
    [[nodiscard]] auto end()         { return m_components.end(); }
    [[nodiscard]] auto begin() const { return m_components.begin(); }
    [[nodiscard]] auto end()   const { return m_components.end(); }

  private:
    /// 「この index は成分を持たない」を表す番兵。**実在する dense 添字と
    /// 衝突しないこと**をコンストラクタの `static_assert` が守っている
    static constexpr std::uint32_t kNullIndex = 0xFFFFFFFFu;

    /// `entity` の dense 添字。持っていなければ `kNullIndex`。**疎配列は 1 回だけ読む**
    [[nodiscard]] std::uint32_t DenseIndexOf(Entity entity) const noexcept {
      if (!m_ready || !entity.IsValid()) {
        return kNullIndex;
      }
      const std::size_t index = entity.Index();
      return (index < m_sparse.GetSize()) ? m_sparse[index] : kNullIndex;
    }

    DynamicArray<T>           m_components;     ///< dense: 本体
    DynamicArray<Entity>      m_denseToEntity;  ///< dense: 所有者 (R-11)
    DynamicArray<std::uint32_t> m_sparse;       ///< sparse: Index() -> dense 添字 (1-2)
    bool                      m_ready = false;
  };

}
