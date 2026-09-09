#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace GLFD::ECS {

  /**
   * @brief エンティティのハンドル (ECS 1-1 / R-1)
   *
   * @details
   *  下位 32 bit が index、上位 32 bit が generation。
   *  index は疎配列の添字、generation は「同じ index の何代目か」を表す。
   *
   *  ## なぜ 64 bit なのか
   *  32 bit を index と generation に分ける配分(例: 20 / 12)は、
   *  **世代が 4096 で一周した時点で、その index に対するダングリング検出が
   *  効かなくなる**。毎秒数十体が生成・破棄されるゲームでは、1 つの index が
   *  4096 回再利用されるのは数十分でありえる。64 bit にすれば配分の判断を
   *  将来へ持ち越さずに済む。
   *
   *  ## IsValid() と Registry::IsAlive() は別物 (R-3)
   *   - `IsValid()` は「**無効値ではない**」ことしか言わない
   *   - **生きているかは `Registry::IsAlive(Entity)` が答える**
   *     (index が範囲内で、かつ generation が現世代と一致すること)
   *
   *  破棄されたハンドルは `IsValid() == true` のまま `IsAlive() == false` になる。
   *  この 2 つを混同すると世代カウンタを入れた意味が無くなる。
   *
   *  @note `uint32_t` への暗黙変換は**持たせない**。持たせると
   *        「dense 添字を Entity として使う」種類の誤りが型で止まらなくなる。
   *
   *  @note **デバッガでは 64 bit の数値 1 つに見える。** index と generation が
   *        分かれて見えないのは実際に不便だが、`.natvis` を 1 本用意すれば
   *        解消する。このフェーズでは作っていない。次に困ったら検討すること。
   *
   *  @note 旧 `NullEntity = UINT32_MAX` は**廃止した** (R-4)。無効値をそのまま
   *        添字に使うと `m_sparse[4294967295]` への書き込みになり、
   *        「上限をわずかに超える」より遥かに悪い (ECS-0a)。
   */
  class Entity {
  public:
    /// 既定構築は**無効値**
    constexpr Entity() noexcept : m_value(kInvalidValue) {}

    [[nodiscard]] static constexpr Entity Invalid() noexcept { return Entity{}; }

    /**
     * @brief index と generation からハンドルを組み立てる
     *
     * @warning **`Registry` 以外から呼ばないこと。** 生存管理は `Registry` が
     *          持っている。ここで組み立てたハンドルが生きているかどうかは
     *          `Registry` にしか分からない。
     *          テストが「壊れたハンドル」を作るためにも使うので公開している。
     */
    [[nodiscard]] static constexpr Entity Make(std::uint32_t index,
                                               std::uint32_t generation) noexcept {
      return Entity{ (static_cast<std::uint64_t>(generation) << 32)
                     | static_cast<std::uint64_t>(index) };
    }

    [[nodiscard]] constexpr std::uint32_t Index() const noexcept {
      return static_cast<std::uint32_t>(m_value & 0xFFFFFFFFull);
    }

    [[nodiscard]] constexpr std::uint32_t Generation() const noexcept {
      return static_cast<std::uint32_t>(m_value >> 32);
    }

    /// @brief 無効値でないこと。**生きているかは `Registry::IsAlive` が答える** (R-3)
    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_value != kInvalidValue; }

    friend constexpr bool operator==(Entity lhs, Entity rhs) noexcept {
      return lhs.m_value == rhs.m_value;
    }
    friend constexpr bool operator!=(Entity lhs, Entity rhs) noexcept {
      return lhs.m_value != rhs.m_value;
    }

  private:
    /// index も generation も全ビット 1。**どの生きたハンドルとも一致しない**
    static constexpr std::uint64_t kInvalidValue = ~std::uint64_t{ 0 };

    constexpr explicit Entity(std::uint64_t value) noexcept : m_value(value) {}

    std::uint64_t m_value;
  };

  static_assert(sizeof(Entity) == 8, "Entity must stay 8 bytes (index 32 + generation 32)");
  static_assert(std::is_trivially_copyable_v<Entity>,
                "Entity is passed by value everywhere; it must stay trivially copyable");
  static_assert(Entity{}.Index() == 0xFFFFFFFFu,
                "the invalid handle must not look like a usable index");

  /**
   * @brief 同時に存在できるエンティティ数の上限。**疎配列の構造的な限界** (1-2)
   *
   * @details
   *  `SparseSet` はこの数だけ疎配列を**固定で確保する**(型ごとに)。
   *  したがってこの値はコンポーネント型の数に掛かる。
   *
   *  ## 65,536 にした根拠
   *   - **動いている数字が 20,000**(Boid デモの実績)。その 3.3 倍の余裕
   *   - 疎配列は `uint32_t` なので **1 型あたり 256 KB**。20 型でも 5 MB。
   *     1,000,000 のときは 20 型で 122 MB(実測)だった
   *   - 2 の冪にしたのは `Index()` が 16 bit に収まるという副次的な利点のため。
   *     **依存はしていない**(50,000 でも成立する)
   *
   *  @note **後から上げる方が簡単である。** 倍の 131,072 にしても 20 型で 10 MB。
   *        下げる方は `kMaxConfigurableEntityCount` との関係を崩すので難しい。
   *        この方向に余裕を取ってある。
   *
   *  @note config に書ける上限 (`kMaxConfigurableEntityCount`) は**この値の半分**に
   *        してある。境界そのものを config から触らせないため (ECS-0b)。
   *        大小関係は `BoidDemoScene.cpp` の `static_assert` が守っている。
   *
   *  @note 可変長の疎配列は**採らない**。65,536 なら固定でも 20 型 5 MB で、
   *        伸長時の再配置と `Has` の境界判定の複雑さに見合わない
   *        (旧 R-31 は削除)。
   */
  constexpr std::size_t MaxEntities = 65536;

}
