#pragma once

#include "Entity.h"
#include "Registry.h"
#include "SparseSet.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace GLFD::ECS {

  /**
   * @brief 成分の組を反復する (ECS 1-5 / R-21 〜 R-24)
   *
   * @details
   *  ## 何をするか (R-22)
   *  **いちばん小さいプールを基準に反復し、他のプールで存在を確認する。**
   *  VS / EDF 型では敵・弾・経験値で成分の組み合わせが違うので、
   *  「全員が同じ成分を持つ」前提の並列配列アクセスは成立しない。
   *
   *  ```cpp
   *  for (auto [entity, pos, vel] : registry.View<Position, Velocity>()) {
   *    pos.x += vel.vx * dt;      // 参照なのでそのまま書ける
   *  }
   *  ```
   *
   *  ## `std::tuple` について (N-1)
   *  **返却用の器としてのみ使う。確保も送出もしない。** N-1 が禁じているのは
   *  所有・確保・送出を伴う型であって `std::` という名前ではない
   *  (`<charconv>` = R1-8、`std::optional` = R3-30 と同じ判定)。
   *  可変長テンプレートではメンバ名を生成できないため名前付きの集約は書けず、
   *  代案はコールバック形だけになるが、早期脱出が `bool` 返しになって読みにくい。
   *
   *  ## 並列に投げられること (§2.2)
   *  `Slice` で基準プールの範囲を切り、`JobSystem` へ値渡しで投げる。
   *  **`View` はコピーできる**(プールへのポインタしか持たない)。
   *
   *  ```cpp
   *  const std::size_t count = view.BaseSize();
   *  jobSystem.KickJob([view, start, end]() {
   *    for (auto [e, pos, vel] : view.Slice(start, end)) { ... }
   *  }, &handle);
   *  ```
   *
   *  @note **範囲を切るのは基準プールの添字**であって「反復して返る件数」では
   *        ない。組み合わせを持たないものは飛ばされるので、切った範囲の
   *        大きさと実際に回る件数は一致しない。**均等に割れるとは限らない。**
   *
   *  ## 反復中に構造を変えたら気づく (R-24)
   *  構築時に `Registry::StructureVersion()` を控え、**反復子を進めるたびに**
   *  Debug で `assert` する。1-4 で即時 API を残した判断 (R-36) は、この検出と
   *  組で成立している。
   *
   *  @note **`assert` は発火すると `abort()` するのでテストから確かめられない**
   *        (1-4 の教訓)。同じ判定を `IsStale()` として公開してある。
   *
   *  @warning **反復中の `CreateEntity` は検出されない。それが正しい。**
   *           生成はどのプールの dense も動かさないので反復は無効化されない。
   *           1-4 で「反復中に生成してバッファへ積む」を正しい使い方と決めた
   *           (T-ECS-4)。ここで弾くと正しい使い方が通らなくなる。
   */
  template <class... Ts>
  class View {
    static_assert(sizeof...(Ts) >= 1, "View needs at least one component type");

    using PoolTuple = std::tuple<SparseSet<Ts>*...>;
    using PtrTuple  = std::tuple<Ts*...>;

  public:
    /// 反復子が返すもの: `Entity` と各成分への**参照** (R-23)
    using Entry = std::tuple<Entity, Ts&...>;

    class Iterator {
    public:
      Iterator(const View* view, std::size_t index) noexcept
        : m_view(view), m_index(index) {
        SeekValid();
      }

      Iterator& operator++() noexcept {
        assert(!m_view->IsStale()
               && "View: the registry changed shape while this view was being iterated "
                  "(R-24). Structural changes belong in a CommandBuffer; CreateEntity is "
                  "safe here, AddComponent / RemoveComponent / DestroyEntity are not.");
        ++m_index;
        SeekValid();
        return *this;
      }

      [[nodiscard]] bool operator!=(const Iterator& other) const noexcept {
        return m_index != other.m_index;
      }
      [[nodiscard]] bool operator==(const Iterator& other) const noexcept {
        return m_index == other.m_index;
      }

      [[nodiscard]] Entry operator*() const noexcept {
        return Deref(std::index_sequence_for<Ts...>{});
      }

      /// いま指しているエンティティ。**`operator*` を組み立てずに済む**
      [[nodiscard]] Entity GetEntity() const noexcept { return m_entity; }

      /// 基準プールでの dense 添字。グリッドへ入れる値がこれ (1-5 / 論点3)
      [[nodiscard]] std::size_t BaseIndex() const noexcept { return m_index; }

    private:
      template <std::size_t... Is>
      [[nodiscard]] Entry Deref(std::index_sequence<Is...>) const noexcept {
        return Entry(m_entity, *std::get<Is>(m_current)...);
      }

      /// 組み合わせを持つものへ進む。持たないものは飛ばす (R-22)
      void SeekValid() noexcept {
        while (m_index < m_view->m_end) {
          const Entity entity = m_view->EntityAtBase(m_index);
          if (m_view->Fetch(m_index, entity, m_current)) {
            m_entity = entity;
            return;
          }
          ++m_index;
        }
        m_index  = m_view->m_end;
        m_entity = Entity::Invalid();
      }

      const View* m_view;
      std::size_t m_index;
      PtrTuple    m_current{};
      Entity      m_entity{};
    };

    explicit View(Registry& registry) noexcept : m_registry(&registry) {
      // プールを掴む。**無ければ作る**(旧 `Registry::View<T>` と同じ挙動)
      Assure(std::index_sequence_for<Ts...>{});

      m_version = registry.StructureVersion();

      if (!AllPoolsReady(std::index_sequence_for<Ts...>{})) {
        // 確保に失敗した。**空の View として安全に回る** (N-2: 投げない)
        m_base  = nullptr;
        m_begin = 0;
        m_end   = 0;
        return;
      }

      m_baseIndex = SmallestPool(std::index_sequence_for<Ts...>{});
      m_base      = PoolAt(m_baseIndex, std::index_sequence_for<Ts...>{});
      m_begin     = 0;
      m_end       = m_base->GetSize();
      // **反復のたびに引かない。** `GetEntityList()` は仮想関数なので、
      // 要素ごとに呼ぶと 1 要素につき 1 回の間接呼び出しになる (1-5 の実測で判明)
      m_baseEntities = m_base->GetEntityList();
    }

    [[nodiscard]] Iterator begin() const noexcept {
      assert(!IsStale()
             && "View: the registry changed shape between building this view and "
                "iterating it (R-24)");
      return Iterator(this, m_begin);
    }
    [[nodiscard]] Iterator end() const noexcept { return Iterator(this, m_end); }

    /**
     * @brief この View が見ている**基準プールの範囲の大きさ**
     * @note  返る件数ではない。組み合わせを持たないものは飛ばされる
     */
    [[nodiscard]] std::size_t BaseSize() const noexcept { return m_end - m_begin; }

    [[nodiscard]] bool IsEmpty() const noexcept { return m_begin == m_end; }

    /**
     * @brief 基準プールの範囲を切る(並列に投げるため)
     * @param first / last **この View の範囲の先頭からの相対位置**。切った View を
     *        さらに切っても意味が保たれる。範囲外は丸める
     */
    [[nodiscard]] View Slice(std::size_t first, std::size_t last) const noexcept {
      View out = *this;
      const std::size_t size = BaseSize();
      if (first > size) { first = size; }
      if (last  > size) { last  = size; }
      if (last  < first) { last = first; }
      out.m_begin = m_begin + first;
      out.m_end   = m_begin + last;
      return out;
    }

    /**
     * @brief 基準プールの「dense 添字 -> `Entity`」表 (1-5 / 論点3)
     *
     * @details
     *  **これは安い向きである。** 直接添字で 8 バイト読むだけで、疎配列の
     *  ランダムアクセス(`Entity` -> dense)は要らない。グリッドが返した
     *  dense 添字から `Entity` を復元するのはこの表で行う。
     *
     *  @warning **添字は「この View の基準プール」の dense 添字である。**
     *           別の View で組んだグリッドの添字を渡してはならない。
     *  @return 空の View では `nullptr`
     */
    [[nodiscard]] const Entity* BaseEntities() const noexcept { return m_baseEntities; }

    /// 構築してから `Registry` の構造が変わったか (R-24)。**`assert` と同じ判定**
    [[nodiscard]] bool IsStale() const noexcept {
      return m_registry->StructureVersion() != m_version;
    }

    /// 構築時に控えた構造版。`SpatialHashGrid::SetBuildStamp` へ渡す値
    [[nodiscard]] std::uint32_t StructureVersion() const noexcept { return m_version; }

    /**
     * @brief この View が掴んでいるプールから成分を引く
     *
     * @note **`Registry::GetComponent` を近傍ループで呼ばないため。** あちらは
     *       呼ぶたびにプール一覧を線形探索する。ここは構築時に掴んだ
     *       ポインタを使うので、疎配列 1 回で済む。
     * @return 持っていなければ `nullptr`
     */
    template <class T>
    [[nodiscard]] T* Find(Entity entity) const noexcept {
      constexpr std::size_t index = IndexOfType<T>();
      static_assert(index < sizeof...(Ts),
                    "View::Find can only look up a component this view was built with");
      auto* const pool = std::get<index>(m_pools);
      return (pool != nullptr) ? pool->Find(entity) : nullptr;
    }

    /**
     * @brief 基準プールの dense 配列そのもの。**単一型の View でのみ使える**
     *
     * @details
     *  グリッドには基準プールの dense 添字が入る (1-5 論点3)。返ってきた添字から
     *  成分を引くのは**直接添字**で、疎配列のランダムアクセスが要らない。
     *  これが案1 を採った理由そのものなので、その口をここに置く。
     *
     *  @warning 添字は「この View の基準プール」のもの。**同じ View で組んだ
     *           グリッドの添字だけを渡すこと。**
     *  @return 確保に失敗していれば `nullptr`
     */
    [[nodiscard]] auto BaseComponents() const noexcept
      requires (sizeof...(Ts) == 1)
    {
      return std::get<0>(m_pools) != nullptr ? std::get<0>(m_pools)->GetData() : nullptr;
    }

  private:
    friend class Iterator;

    /// `T` が `Ts...` の何番目か。見つからなければ `sizeof...(Ts)`
    template <class T>
    [[nodiscard]] static constexpr std::size_t IndexOfType() noexcept {
      const bool matches[] = { std::is_same_v<T, Ts>... };
      for (std::size_t i = 0; i < sizeof...(Ts); ++i) {
        if (matches[i]) { return i; }
      }
      return sizeof...(Ts);
    }

    template <std::size_t... Is>
    void Assure(std::index_sequence<Is...>) noexcept {
      ((std::get<Is>(m_pools) = m_registry->template TryAssure<Ts>()), ...);
    }

    template <std::size_t... Is>
    [[nodiscard]] bool AllPoolsReady(std::index_sequence<Is...>) const noexcept {
      return ((std::get<Is>(m_pools) != nullptr) && ...);
    }

    /// いちばん小さいプールの番号 (R-22)
    template <std::size_t... Is>
    [[nodiscard]] std::size_t SmallestPool(std::index_sequence<Is...>) const noexcept {
      std::size_t best     = 0;
      std::size_t bestSize = static_cast<std::size_t>(-1);
      (((std::get<Is>(m_pools)->GetSize() < bestSize)
            ? (bestSize = std::get<Is>(m_pools)->GetSize(), best = Is)
            : 0u), ...);
      return best;
    }

    template <std::size_t... Is>
    [[nodiscard]] ISparseSet* PoolAt(std::size_t which,
                                     std::index_sequence<Is...>) const noexcept {
      ISparseSet* found = nullptr;
      (((Is == which) ? (found = std::get<Is>(m_pools)) : nullptr), ...);
      return found;
    }

    [[nodiscard]] Entity EntityAtBase(std::size_t denseIndex) const noexcept {
      return m_baseEntities[denseIndex];
    }

    /**
     * @brief 各成分へのポインタを集める。**1 つでも欠けたら false**
     * @note  `Has` と `Find` を別々に呼ぶと疎配列を 2 回引く。
     *        `Find` は不在で `nullptr` を返すので 1 回で足りる
     */
    [[nodiscard]] bool Fetch(std::size_t denseIndex, Entity entity,
                             PtrTuple& out) const noexcept {
      return FetchImpl(denseIndex, entity, out, std::index_sequence_for<Ts...>{});
    }

    template <std::size_t... Is>
    [[nodiscard]] bool FetchImpl(std::size_t denseIndex, Entity entity, PtrTuple& out,
                                 std::index_sequence<Is...>) const noexcept {
      ((std::get<Is>(out) = FetchOne<Is>(denseIndex, entity)), ...);
      return ((std::get<Is>(out) != nullptr) && ...);
    }

    /**
     * @brief 1 つの成分を引く
     * @note  **基準プールだけは dense 添字で直接引く。** 疎配列を引かずに済み、
     *        単一成分の `View<T>` では疎配列アクセスが 1 回も起きない
     *        (旧 `GetData()` の直行と同じ費用になる)
     */
    template <std::size_t I>
    [[nodiscard]] auto FetchOne(std::size_t denseIndex, Entity entity) const noexcept
        -> std::tuple_element_t<I, std::tuple<Ts...>>* {
      auto* const pool = std::get<I>(m_pools);
      if (I == m_baseIndex) {
        return pool->GetData() + denseIndex;
      }
      return pool->Find(entity);
    }

    Registry*     m_registry;
    PoolTuple     m_pools{};
    ISparseSet*   m_base         = nullptr;
    /// 基準プールの「dense 添字 -> Entity」。**構築時に 1 回だけ引く**
    const Entity* m_baseEntities = nullptr;
    std::size_t   m_baseIndex    = 0;
    std::size_t   m_begin     = 0;
    std::size_t   m_end       = 0;
    /// 構築時の構造版 (R-24)。**Debug / Release どちらにも置く** —
    /// `#ifdef` でメンバを増減させると構成ごとに `sizeof` が変わる (1-4 の教訓)
    std::uint32_t m_version   = 0;
  };

  // ---------------------------------------------------------------------------
  // Registry::View<Ts...>
  //
  //  1-4 の `CommandBuffer` と同じ形。`View` は `Registry` の中身
  //  (`TryAssure` / `StructureVersion`) を使うので、依存を
  //  **View.h -> Registry.h の一方向**にし、両方が完全になったここで定義する。
  // ---------------------------------------------------------------------------
  template <class... Ts>
  [[nodiscard]] inline ::GLFD::ECS::View<Ts...> Registry::View() noexcept {
    return ::GLFD::ECS::View<Ts...>(*this);
  }

}
