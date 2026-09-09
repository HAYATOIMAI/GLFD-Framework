#pragma once

#include "SparseSet.h"
#include "../Core/DynamicArray.h"
#include "../Core/TypeInfo.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace GLFD::ECS {

  /// 1-4: `ApplyCommands` の引数。**定義は `CommandBuffer.h` にある**
  class CommandBuffer;

  /// 1-5: 反復。**定義は `View.h` にある**
  template <class... Ts> class View;

  /**
   * @brief エンティティの生存管理とコンポーネントの所有者 (ECS 1-1)
   *
   * @details
   *  ## 生存管理 (R-5 〜 R-10)
   *   - index ごとの**現世代**を `m_generations` に持つ。**使った分だけ伸ばす**
   *     ので、`MaxEntities` が大きくても実エンティティ数に比例した量しか使わない
   *   - 再利用できる index は `m_freeIndices` に**明示的なスタック**で持つ。
   *     `m_generations` へ埋め込むフリーリストにしないのは、`IsAlive` の比較が
   *     「その枠が空いていないこと」に暗黙に依存するのを避けるためと、
   *     R-9(一周した index を永久に引退させる)が表現しづらいため
   *   - **generation は破棄時に加算する**(生成時ではない)
   *
   *  ## 検査の責任者は Registry ただ 1 つ (§3.1)
   *  `Entity` を受け取る API はすべて入口で `IsAlive` を通す。`SparseSet` 側では
   *  無効なハンドルを弾かない(二重コストになり、責任者が曖昧になる)。
   *
   *  @warning **即時 API (`DestroyEntity` / `AddComponent` / `RemoveComponent`) を
   *           反復中に呼んではならない。** dense 配列の swap-and-pop が走るため、
   *           反復中の生ポインタが飛ばしと重複を起こす。
   *           **反復中は `CommandBuffer` へ積むこと** (1-4)。積むだけなら
   *           `SparseSet` は 1 ミリも動かない。
   *
   *  @note **即時 API は残してある** (1-4 の §4 論点6)。`CreateEntity` は R-17 に
   *        より、そもそも即時でなければならない(返せないと「生成した弾に速度を
   *        設定する」が書けない)ので、「全部遅延」という API は作れない。
   *        加えて `OnEnter` のような反復の外での構築で 2 万件をバッファへ
   *        積む理由が無い。
   *        **上の制約は規約で守らせ続けるのではなく、1-5 の R-24
   *        (`View` の生存中に構造が変わったら Debug で `assert`)が
   *        機械的に検出する。** 検出する側に投資する。
   */
  class Registry {
  public:
    explicit Registry(Memory::IMemoryResource* resource)
      : m_resource(resource)
      , m_pools(resource)
      , m_generations(resource)
      , m_freeIndices(resource) {
    }

    /**
     * @note **確保と解放を対にする** (R-15)。以前はデストラクタを手で呼ぶだけで
     *       `Deallocate` を呼んでいなかった。`StackResource` では no-op なので
     *       無害だったが、対にしない設計を残さない
     */
    ~Registry() {
      for (std::size_t i = 0; i < m_pools.GetSize(); ++i) {
        PoolSlot& slot = m_pools[i];
        if (slot.pool == nullptr) {
          continue;
        }
        slot.pool->~ISparseSet();
        m_resource->Deallocate(slot.pool, slot.bytes, slot.align);
        slot.pool = nullptr;
      }
    }

    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;

    // ------------------------------------------------------------------------
    // 生存管理
    // ------------------------------------------------------------------------

    /**
     * @brief 新しいエンティティを作る
     * @return 生きているハンドル。**上限や確保失敗では `Entity::Invalid()`**
     *
     * @warning **戻り値を必ず確認すること** (R-10)。無効なハンドルを他の API へ
     *          渡しても弾かれるが、「作れたつもり」で先へ進むと原因が追えなくなる
     */
    [[nodiscard]] Entity CreateEntity() noexcept {
      if (!m_freeIndices.IsEmpty()) {
        const std::uint32_t index = m_freeIndices[m_freeIndices.GetSize() - 1];
        m_freeIndices.PopBack();
        return Entity::Make(index, m_generations[index]);
      }

      if (m_generations.GetSize() >= MaxEntities) {
        return Entity::Invalid();           // 上限 (R-10)
      }
      if (!m_generations.TryPushBack(std::uint32_t{ 0 })) {
        return Entity::Invalid();           // 確保失敗 (N-2: 投げない)
      }
      const std::uint32_t index = static_cast<std::uint32_t>(m_generations.GetSize() - 1);
      return Entity::Make(index, m_generations[index]);
    }

    /**
     * @brief エンティティを破棄し、**全プールから成分を取り除く** (R-7)
     *
     * @note 生きていなければ何もしない。**二重呼び出しは安全**。
     * @warning 反復中に呼んではならない(クラスの @warning を参照)
     */
    void DestroyEntity(Entity entity) noexcept {
      if (!IsAlive(entity)) {
        return;
      }

      ++m_structureVersion;   // dense が動く (1-5 / R-24)

      // 全プールを 1 回ずつ回る。`Remove` は不在なら何もしない契約 (ISparseSet)
      for (std::size_t i = 0; i < m_pools.GetSize(); ++i) {
        if (m_pools[i].pool != nullptr) {
          m_pools[i].pool->Remove(entity);
        }
      }

      const std::uint32_t index = entity.Index();
      const std::uint32_t next  = m_generations[index] + 1u;
      m_generations[index] = next;

      if (next == 0u) {
        // R-9: 世代が一周した index は**再利用集合へ戻さない**。
        // index を 1 つ失うだけで、誤検出を出さない
        ++m_retiredCount;
        return;
      }
      if (!m_freeIndices.TryPushBack(index)) {
        // 空き集合を伸ばせなかった。**index を捨てる方に倒す**(N-2: 投げない)。
        // 生存判定は狂わない — 引退として数える
        ++m_retiredCount;
      }
    }

    /**
     * @brief そのハンドルが今も生きているか (R-8)
     *
     * @note **`Entity::IsValid()` とは別物。** あちらは無効値でないことしか
     *       言わない。破棄されたハンドルは `IsValid() == true` のまま
     *       ここで false になる。
     *
     * @note 見ているのは「その index の**現世代**と一致するか」である。
     *       **空き枠の現世代を手で組み立てたハンドルは区別できない**
     *       (`Entity::Make(freeIndex, m_generations[freeIndex])`)。
     *       `Entity::Make` を `Registry` 以外から呼ばない規約で担保している。
     *       閉じるには空き集合の線形探索か、生死ビットの追加が要る
     */
    [[nodiscard]] bool IsAlive(Entity entity) const noexcept {
      if (!entity.IsValid()) {
        return false;
      }
      const std::size_t index = entity.Index();
      return index < m_generations.GetSize()
          && m_generations[index] == entity.Generation();
    }

    /**
     * @brief 構造が変わった回数 (1-5 / R-24)
     *
     * @details
     *  **dense 配列の並びが動き得る操作**のたびに増える。`View` は構築時に
     *  この値を控え、反復中に変わっていたら Debug で気づく。
     *  `SpatialHashGrid` も組んだ時点の値を控える(グリッドに入っている
     *  dense 添字がまだ有効かを、規約ではなく検査で言えるようにするため)。
     *
     *  ## `CreateEntity` では**増えない**
     *  1-4 で「反復中に `CreateEntity` を呼んで結果をバッファへ積む」を
     *  **正しい使い方**と決めた(T-ECS-4)。生成は `m_generations` を伸ばすだけで
     *  **どのプールの dense も動かさない**ので、反復は無効化されない。
     *  ここで増やすと、正しい使い方が検出に引っかかる。
     *
     *  ## 増えるのは 3 つ
     *   - `AddComponent` が成功したとき(dense が伸びる = 再確保があり得る)
     *   - `RemoveComponent`(swap-and-pop で並びが変わる)
     *   - `DestroyEntity`(全プールから抜ける)
     *
     *  `ApplyCommands` は上の 3 つを呼ぶので自然に増える。
     *
     *  @note 一周は考えない。32 bit を使い切るには構造変更が 42 億回必要で、
     *        仮に一周しても**同じ値に戻る確率**の話になり、誤検出ではなく
     *        見逃しの側に倒れる(検出器としては安全側)。
     */
    [[nodiscard]] std::uint32_t StructureVersion() const noexcept {
      return m_structureVersion;
    }

    /**
     * @brief 生きているエンティティの数
     *
     * @note **カウンタを持たず導出する** (§5 論点5)。振る舞いを支配している
     *       配列そのものから計算されるので、実態からずれる状態が存在しない。
     *       ECS-0c で「カウンタが止まっていること」をテストが押さえられなかった
     *       経緯への答えである。
     *       `m_retiredCount` だけは持たざるを得ないが、**触るのは R-9 の 1 箇所と
     *       空き集合の確保失敗時だけ**で、増える一方の量である
     */
    [[nodiscard]] std::uint32_t AliveCount() const noexcept {
      return static_cast<std::uint32_t>(m_generations.GetSize())
           - static_cast<std::uint32_t>(m_freeIndices.GetSize())
           - m_retiredCount;
    }

    /**
     * @brief 積まれた構造変更を**積んだ順に**適用する (1-4 / R-34)
     *
     * @note **本体は `CommandBuffer.h` にある。** 依存を
     *       `CommandBuffer.h -> Registry.h` の一方向にするためで、
     *       理由はその位置のコメントに書いてある。
     * @note 適用したバッファは空になる(二重適用を構造的に防ぐ)。
     *       何が捨てられたかは `CommandBuffer::Report()` にある
     */
    void ApplyCommands(CommandBuffer& buffer) noexcept;

    // ------------------------------------------------------------------------
    // コンポーネント
    // ------------------------------------------------------------------------

    /**
     * @brief 成分を足す
     * @return 足した成分。**無効なハンドル / 既に持っている / 確保失敗で `nullptr`** (R-25)
     * @note  `T&` では失敗を表現できないので `T*` を返す
     */
    template <class T, class... Args>
    [[nodiscard]] T* AddComponent(Entity entity, Args&&... args) noexcept {
      if (!IsAlive(entity)) {
        return nullptr;
      }
      SparseSet<T>* const pool = TryAssure<T>();
      if (pool == nullptr) {
        return nullptr;
      }
      T* const created = pool->TryEmplace(entity, std::forward<Args>(args)...);
      if (created != nullptr) {
        // **成功したときだけ**進める。失敗は dense を動かしていない (1-5)
        ++m_structureVersion;
      }
      return created;
    }

    /// @brief 成分を取り除く。無効なハンドルでも持っていなくても安全
    template <class T>
    void RemoveComponent(Entity entity) noexcept {
      if (!IsAlive(entity)) {
        return;
      }
      if (SparseSet<T>* const pool = FindPool<T>()) {
        // 不在なら `Remove` は何もしないが、**版はどちらでも進める**。
        // 「持っていたか」を先に見ると疎配列を 2 回引くことになり、
        // 反復中に呼んではならない操作のために払う費用ではない
        ++m_structureVersion;
        pool->Remove(entity);
      }
    }

    /// @return 成分。**無効なハンドル / 持っていない場合は `nullptr`**
    template <class T>
    [[nodiscard]] T* GetComponent(Entity entity) noexcept {
      if (!IsAlive(entity)) {
        return nullptr;
      }
      SparseSet<T>* const pool = FindPool<T>();
      return (pool != nullptr) ? pool->Find(entity) : nullptr;
    }

    /// @copydoc GetComponent
    template <class T>
    [[nodiscard]] const T* GetComponent(Entity entity) const noexcept {
      if (!IsAlive(entity)) {
        return nullptr;
      }
      const SparseSet<T>* const pool = FindPool<T>();
      return (pool != nullptr) ? pool->Find(entity) : nullptr;
    }

    template <class T>
    [[nodiscard]] bool HasComponent(Entity entity) const noexcept {
      return GetComponent<T>(entity) != nullptr;
    }

    /**
     * @brief 成分の組を反復する (1-5 / R-21 〜 R-23)
     *
     * @note **本体は `View.h` にある。** `View` が `Registry` の中身を使うため
     *       依存を `View.h` -> `Registry.h` の一方向にしてある
     *       (1-4 の `CommandBuffer` と同じ形)
     */
    template <class... Ts>
    [[nodiscard]] ::GLFD::ECS::View<Ts...> View() noexcept;

  private:
    template <class... Ts> friend class ::GLFD::ECS::View;

    struct PoolSlot {
      std::size_t typeHash = 0;
      ISparseSet* pool     = nullptr;
      std::size_t bytes    = 0;   ///< Deallocate に要る (R-15)
      std::size_t align    = 0;
    };

    /// 型 T のプールを返す。**無ければ作る。作れなければ `nullptr`**
    template <class T>
    [[nodiscard]] SparseSet<T>* TryAssure() noexcept {
      constexpr std::size_t typeHash = TypeInfo::GetID<T>();

      if (SparseSet<T>* const found = FindPool<T>()) {
        return found;
      }

      constexpr std::size_t bytes = sizeof(SparseSet<T>);
      constexpr std::size_t align = alignof(SparseSet<T>);

      void* const buffer = m_resource->Allocate(bytes, align);
      if (buffer == nullptr) {
        return nullptr;
      }

      SparseSet<T>* const pool = new(buffer) SparseSet<T>(m_resource);

      // 疎配列を確保できなかった、または一覧へ積めなかった場合は**きれいに戻す**
      if (!pool->IsReady()
          || !m_pools.TryPushBack(PoolSlot{ typeHash, pool, bytes, align })) {
        pool->~SparseSet<T>();
        m_resource->Deallocate(buffer, bytes, align);
        return nullptr;
      }
      return pool;
    }

    template <class T>
    [[nodiscard]] SparseSet<T>* FindPool() noexcept {
      constexpr std::size_t typeHash = TypeInfo::GetID<T>();
      for (std::size_t i = 0; i < m_pools.GetSize(); ++i) {
        if (m_pools[i].typeHash == typeHash) {
          return static_cast<SparseSet<T>*>(m_pools[i].pool);
        }
      }
      return nullptr;
    }

    template <class T>
    [[nodiscard]] const SparseSet<T>* FindPool() const noexcept {
      constexpr std::size_t typeHash = TypeInfo::GetID<T>();
      for (std::size_t i = 0; i < m_pools.GetSize(); ++i) {
        if (m_pools[i].typeHash == typeHash) {
          return static_cast<const SparseSet<T>*>(m_pools[i].pool);
        }
      }
      return nullptr;
    }

    Memory::IMemoryResource*   m_resource;
    DynamicArray<PoolSlot>     m_pools;
    /// index -> 現世代。**使った分だけ伸ばす**(`MaxEntities` 分は取らない)
    DynamicArray<std::uint32_t> m_generations;
    /// 再利用できる index。スタックとして使う (§5 論点2)
    DynamicArray<std::uint32_t> m_freeIndices;
    /// 生存でも空きでもない index の数。R-9 と空き集合の確保失敗でのみ増える
    std::uint32_t               m_retiredCount = 0;
    /// 構造が変わった回数 (R-24)。**`CreateEntity` では増えない**
    std::uint32_t               m_structureVersion = 0;
  };

}
