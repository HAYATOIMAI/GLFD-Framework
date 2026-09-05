#pragma once

/**
 * @file  JsonValue.h
 * @brief DOM のノード表現(Layer 2)
 *
 * @details
 *  要件書 §4.1 に対応する。
 *
 *  ## 生存期間の契約(最重要 / R0-5)
 *  `Value` は**何も所有しない**。文字列・配列・オブジェクトの実体は
 *  すべて `JsonArena` 上にあり、`Value` はそこへのポインタと要素数を持つだけである。
 *  したがって **アリーナ(通常は `Document`)より長生きした瞬間にダングリングする。**
 *  `Document::Clear()` / デストラクタ / `JsonArena::Reset()` のいずれかが走った時点で、
 *  それ以前に取得した全ての `Value` / `Member` / `StringView` が無効になる。
 *
 *  ## 所有権を持たないことの帰結
 *  `Value` は `JsonArena` へのポインタを**持たない**(24 バイト予算を圧迫するため)。
 *  変更系 API はアリーナを引数で受け取る。
 *
 *  ## スレッド安全性
 *  単一スレッド前提。読み取り専用であれば複数スレッドから同時に触れるが、
 *  同一の `Value` 木への並行変更は同期されない (N-3)。
 *
 *  ## 要件書 §4.1 との差分(意図的)
 *  §4.1 では確保を伴う構築 API が `void` / `Value&` を返す形で書かれているが、
 *  アリーナの確保は失敗し得る (R0-3) ため、**`[[nodiscard]] bool` / `Value*` を返す**
 *  形に変更している。エンジンの API 規約「失敗し得る確保系 API は
 *  `[[nodiscard]] ... noexcept`」に従ったもので、失敗を握り潰さないための変更である。
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "Core/StringView.h"
#include "Core/Json/JsonArena.h"
#include "Core/Json/JsonTypes.h"

namespace GLFD::Json {

  struct Member;

  /**
   * @brief JSON の値ひとつを表すタグ付きユニオン
   *
   * @details
   *  ## レイアウト(x64 実測: 24 バイト / align 8)
   *  | offset | メンバ | 用途 |
   *  |---|---|---|
   *  | 0..7   | `m_payload`  | スカラの値、または String / Array / Object の先頭ポインタ |
   *  | 8..11  | `m_size`     | String: バイト数 / Array: 要素数 / Object: メンバ数 |
   *  | 12..15 | `m_capacity` | Array / Object の確保済み容量 |
   *  | 16     | `m_type`     | タグ(+ 7 バイトの末尾パディング) |
   *
   *  ペイロードを先頭に置いているのは、タグ先頭でも同じ 24 バイトになる一方、
   *  タグ先頭だと 12..15 に内部の穴ができるため。この配置なら穴は末尾だけで、
   *  `memcpy` 移送 (R2-5) が未初期化バイトを跨がない。
   *
   *  16 バイト化(容量を配列本体の直前へ追い出す / NaN-boxing)は
   *  **フェーズ 3 の最適化候補**であり、ここでは追求しない (R2-1)。
   *
   *  ## 既定構築
   *  `Null`。**不正な状態を構築できない**ことを不変条件とする。
   */
  class Value {
  public:
    /// 配列 / オブジェクトを逐次構築するときの最小容量
    static constexpr std::uint32_t kMinCapacity = 4;

    /// 既定構築は Null
    constexpr Value() noexcept = default;

    // -------------------------------------------------------------------------
    // 型の問い合わせ
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr ValueType Type() const noexcept { return m_type; }

    [[nodiscard]] constexpr bool IsNull()   const noexcept { return m_type == ValueType::Null; }
    [[nodiscard]] constexpr bool IsBool()   const noexcept { return m_type == ValueType::Bool; }
    [[nodiscard]] constexpr bool IsInt64()  const noexcept { return m_type == ValueType::Int64; }
    [[nodiscard]] constexpr bool IsUInt64() const noexcept { return m_type == ValueType::UInt64; }
    [[nodiscard]] constexpr bool IsDouble() const noexcept { return m_type == ValueType::Double; }
    [[nodiscard]] constexpr bool IsString() const noexcept { return m_type == ValueType::String; }
    [[nodiscard]] constexpr bool IsArray()  const noexcept { return m_type == ValueType::Array; }
    [[nodiscard]] constexpr bool IsObject() const noexcept { return m_type == ValueType::Object; }

    [[nodiscard]] constexpr bool IsNumber() const noexcept {
      return m_type == ValueType::Int64 || m_type == ValueType::UInt64
          || m_type == ValueType::Double;
    }

    // -------------------------------------------------------------------------
    // 安全系アクセサ
    //
    //   変換規則(§5 論点 7 で確定):
    //   | 変換           | 規則 |
    //   |---|---|
    //   | 整数 → 整数    | 値が完全に表現できる場合のみ成功。範囲外は失敗 |
    //   | Double → 整数  | 小数部が無く範囲内なら成功。小数部があれば失敗 |
    //   | 整数 → Double  | 往復一致する場合のみ成功(|v| <= 2^53) |
    //   | 数値 → float   | **丸めを許容**。範囲外(inf になる)場合のみ失敗 |
    //   | 型不一致       | 常に失敗 |
    //
    //   float だけ丸めを許すのは、float が本質的に近似表現であり、手書き config の
    //   `0.1` を拒否すると float パラメータが一切読めなくなるため。一方
    //   Double <-> 整数 の欠落は「書き手の意図と型が食い違っている」兆候なので弾く。
    //
    //   **失敗時は out を一切変更しない。**
    // -------------------------------------------------------------------------

    [[nodiscard]] bool TryGetBool(bool& out) const noexcept {
      if (m_type != ValueType::Bool) { return false; }
      out = m_payload.b;
      return true;
    }

    [[nodiscard]] bool TryGetInt64(std::int64_t& out) const noexcept;
    [[nodiscard]] bool TryGetUInt64(std::uint64_t& out) const noexcept;
    [[nodiscard]] bool TryGetInt(std::int32_t& out) const noexcept;
    [[nodiscard]] bool TryGetUInt(std::uint32_t& out) const noexcept;
    [[nodiscard]] bool TryGetDouble(double& out) const noexcept;
    [[nodiscard]] bool TryGetFloat(float& out) const noexcept;

    [[nodiscard]] bool TryGetString(StringView& out) const noexcept {
      if (m_type != ValueType::String) { return false; }
      out = StringView(m_payload.str, m_size);
      return true;
    }

    // -------------------------------------------------------------------------
    // 高速系アクセサ(検証済みデータ用)
    //
    //   ## 型不一致時の挙動 — これは API の契約であり、実装の裁量ではない
    //
    //   型が一致しない場合、**Debug では assert で停止**し、
    //   **Release では型ごとの既定値(0 / false / 空)を返す**。
    //   **未定義動作にはしない。**
    //
    //   @warning **この挙動は意図的であり、最適化のために削除してはならない。**
    //            「Release では assert が消えるのだから型チェックの分岐も消せる」
    //            という変形は**契約違反**である。分岐を消すとユニオンの非アクティブ
    //            メンバを読むことになり、検証済みでないデータを渡された瞬間に
    //            未定義動作へ落ちる。1 分岐は予測しやすく、それに見合う代償ではない。
    //            性能上の必要が生じた場合は、この契約自体を変更する合意を先に取ること。
    // -------------------------------------------------------------------------

    [[nodiscard]] bool GetBool() const noexcept {
      assert(IsBool() && "Value::GetBool: type mismatch");
      return IsBool() ? m_payload.b : false;
    }

    [[nodiscard]] std::int64_t GetInt64() const noexcept {
      assert(IsInt64() && "Value::GetInt64: type mismatch");
      return IsInt64() ? m_payload.i : 0;
    }

    [[nodiscard]] std::uint64_t GetUInt64() const noexcept {
      assert(IsUInt64() && "Value::GetUInt64: type mismatch");
      return IsUInt64() ? m_payload.u : 0u;
    }

    [[nodiscard]] double GetDouble() const noexcept {
      assert(IsDouble() && "Value::GetDouble: type mismatch");
      return IsDouble() ? m_payload.d : 0.0;
    }

    [[nodiscard]] std::int32_t  GetInt()   const noexcept;
    [[nodiscard]] std::uint32_t GetUInt()  const noexcept;
    [[nodiscard]] float         GetFloat() const noexcept;

    [[nodiscard]] StringView GetString() const noexcept {
      assert(IsString() && "Value::GetString: type mismatch");
      return IsString() ? StringView(m_payload.str, m_size) : StringView();
    }

    // -------------------------------------------------------------------------
    // 配列
    // -------------------------------------------------------------------------

    /// 要素数。配列でなければ Debug で assert し 0 を返す
    [[nodiscard]] std::uint32_t Size() const noexcept {
      assert(IsArray() && "Value::Size: not an array");
      return IsArray() ? m_size : 0u;
    }

    [[nodiscard]] bool Empty() const noexcept { return Size() == 0u; }

    [[nodiscard]] const Value& operator[](std::uint32_t index) const noexcept {
      assert(IsArray() && "Value::operator[](index): not an array");
      assert(index < m_size && "Value::operator[](index): index out of range");
      return m_payload.items[index];
    }

    [[nodiscard]] Value& operator[](std::uint32_t index) noexcept {
      assert(IsArray() && "Value::operator[](index): not an array");
      assert(index < m_size && "Value::operator[](index): index out of range");
      return m_payload.items[index];
    }

    [[nodiscard]] const Value* Begin() const noexcept {
      return IsArray() ? m_payload.items : nullptr;
    }
    [[nodiscard]] const Value* End() const noexcept {
      return IsArray() ? m_payload.items + m_size : nullptr;
    }

    // -------------------------------------------------------------------------
    // オブジェクト
    // -------------------------------------------------------------------------

    /// メンバ数。**キー重複もそれぞれ 1 件として数える** (R2-3)
    [[nodiscard]] std::uint32_t MemberCount() const noexcept {
      assert(IsObject() && "Value::MemberCount: not an object");
      return IsObject() ? m_size : 0u;
    }

    /// 線形走査 (R2-2)。重複キーでは**最初に一致したもの**を返す (R2-3)
    [[nodiscard]] const Value* Find(StringView key) const noexcept;
    [[nodiscard]] Value*       Find(StringView key) noexcept;

    [[nodiscard]] bool Has(StringView key) const noexcept { return Find(key) != nullptr; }

    /**
     * @brief キーで引く。不在なら Debug で assert し、共有の不変 Null を返す
     * @note  戻り値が `const Value&` で非 const 版を持たないため、
     *        呼び出し側から共有 Null へ書き込む経路は型として存在しない
     */
    [[nodiscard]] const Value& operator[](StringView key) const noexcept;

    [[nodiscard]] const Member* MemberBegin() const noexcept;
    [[nodiscard]] const Member* MemberEnd()   const noexcept;

    // -------------------------------------------------------------------------
    // 構築(確保を伴うものは失敗し得るので bool / Value* を返す)
    // -------------------------------------------------------------------------

    void SetNull() noexcept                  { *this = Value{}; }
    void SetBool(bool v) noexcept            { Assign(ValueType::Bool);   m_payload.b = v; }
    void SetInt64(std::int64_t v) noexcept   { Assign(ValueType::Int64);  m_payload.i = v; }
    void SetUInt64(std::uint64_t v) noexcept { Assign(ValueType::UInt64); m_payload.u = v; }
    void SetDouble(double v) noexcept        { Assign(ValueType::Double); m_payload.d = v; }

    /// 文字列をアリーナへコピーして保持する
    [[nodiscard]] bool SetString(StringView v, JsonArena& arena) noexcept;

    /// コピーせず参照だけを持つ。**寿命の保証は呼び出し側の責任**
    [[nodiscard]] bool SetStringRef(StringView v) noexcept;

    /// 配列にする。capacity 件ぶんを先に確保する(0 なら確保しない)
    [[nodiscard]] bool SetArray(std::uint32_t capacity, JsonArena& arena) noexcept;

    /// オブジェクトにする。capacity 件ぶんを先に確保する(0 なら確保しない)
    [[nodiscard]] bool SetObject(std::uint32_t capacity, JsonArena& arena) noexcept;

    /**
     * @brief 配列の末尾に Null 要素を足し、その参照を返す
     * @return 追加された要素。確保に失敗したら nullptr
     * @note  容量不足なら 2 倍に伸長する。**旧領域はアリーナ上に放棄される**
     *        (個別解放できないため)。要素数が事前に分かる場合は
     *        `SetArray(capacity, arena)` で実寸を確保すれば伸長は一度も起きない。
     *        `WriteArchive`(1-5)は要素数が事前に分かる場面がほとんどなので、
     *        主経路では放棄は発生しない想定である
     */
    [[nodiscard]] Value* PushBack(JsonArena& arena) noexcept;

    /**
     * @brief オブジェクトにメンバを足し、その値の参照を返す
     * @note  key は**アリーナへコピーされる**。伸長規則は PushBack と同じ
     */
    [[nodiscard]] Value* AddMember(StringView key, JsonArena& arena) noexcept;

    /// AddMember のキー非コピー版。**key の寿命保証は呼び出し側の責任**
    [[nodiscard]] Value* AddMemberRef(StringView key, JsonArena& arena) noexcept;

    /**
     * @brief 要素列を丸ごと配列として設定する(実寸確保 + `memcpy` 一括移送)
     * @param items 移送元。`nullptr` は count == 0 のときのみ許される
     * @param count 要素数
     * @return 確保に失敗したら false(この場合 `*this` は変更されない)
     * @note  要素数が確定してから一度だけ確保するので、伸長も放棄領域も発生しない。
     *        DOM ビルダが `OnArrayEnd(n)` で使う主経路であり、
     *        要素数が事前に分かる `WriteArchive` からも使える。
     *        移送は `memcpy` で行う (R2-5)。**要素は浅くコピーされる** —
     *        入れ子の配列 / オブジェクトはアリーナ上の同じ実体を指したままになる
     */
    [[nodiscard]] bool AssignArray(const Value* items, std::uint32_t count,
                                   JsonArena& arena) noexcept;

    /// AssignArray のオブジェクト版。キーは**複製しない**(移送元のものをそのまま指す)
    [[nodiscard]] bool AssignObject(const Member* members, std::uint32_t count,
                                    JsonArena& arena) noexcept;

  private:
    void Assign(ValueType type) noexcept {
      m_payload.u = 0;
      m_size      = 0;
      m_capacity  = 0;
      m_type      = type;
    }

    [[nodiscard]] bool ReserveArray(std::uint32_t capacity, JsonArena& arena) noexcept;
    [[nodiscard]] bool ReserveObject(std::uint32_t capacity, JsonArena& arena) noexcept;

    /// 数値を double へ「丸めを許して」変換する。数値でなければ false
    [[nodiscard]] bool ToDoubleLossy(double& out) const noexcept;

    union Payload {
      constexpr Payload() noexcept : u(0) {}
      bool          b;
      std::int64_t  i;
      std::uint64_t u;
      double        d;
      const char*   str;      ///< String:  文字列の先頭
      Value*        items;    ///< Array:   要素の先頭
      Member*       members;  ///< Object:  メンバの先頭
    };

    Payload       m_payload;                     // 0..7
    std::uint32_t m_size     = 0;                // 8..11
    std::uint32_t m_capacity = 0;                // 12..15
    ValueType     m_type     = ValueType::Null;  // 16 (+ 7 バイトのパディング)
  };

  // ---------------------------------------------------------------------------
  // このフェーズの設計制約そのもの
  // ---------------------------------------------------------------------------

  static_assert(sizeof(Value) <= 24, "Value must stay compact");
  static_assert(std::is_trivially_copyable_v<Value>, "Value must be memcpy-movable (R2-5)");
  static_assert(std::is_trivially_destructible_v<Value>,
                "Value must not need destruction (arena-owned)");

  /**
   * @brief オブジェクトのメンバ 1 件
   * @note  key はアリーナ上、または呼び出し側が寿命を保証する領域を指す
   */
  struct Member {
    StringView key;
    Value      value;
  };

  static_assert(std::is_trivially_copyable_v<Member>, "Member must be memcpy-movable (R2-5)");
  static_assert(std::is_trivially_destructible_v<Member>, "Member must not need destruction");

  /**
   * @note `Value` は上限 (<= 24) で縛るのに対し、`Member` は**等号で固定**する。
   *       1-5 の `WriteArchive` がメンバ配列を大量に確保するため、この値が
   *       そのままメモリ使用量に効く。またフェーズ 3 で `Value` を 16 バイト化した際、
   *       `Member` が 32 バイトへ落ちることをこの assert で検出できる
   *       (落ちたら期待値を更新し、削減量を記録すること)。
   */
  static_assert(sizeof(Member) == 40, "Member = StringView(16) + Value(24)");

  /**
   * @brief 不在キーを引いたときに返す共有の不変 Null
   * @note  `inline constexpr` なので定数初期化され、関数ローカル static のような
   *        スレッドセーフガードの実行時コストがかからない。const なので
   *        呼び出し側から書き換えられない
   */
  inline constexpr Value kNullValue{};

  // ---------------------------------------------------------------------------
  // Member の完全定義が必要な inline 実装
  // ---------------------------------------------------------------------------

  inline const Value* Value::Find(StringView key) const noexcept {
    if (m_type != ValueType::Object) {
      return nullptr;
    }
    // R2-2: 線形走査。R2-3: 重複キーでは最初の一致を返す
    const Member* const members = m_payload.members;
    for (std::uint32_t i = 0; i < m_size; ++i) {
      if (members[i].key == key) {
        return &members[i].value;
      }
    }
    return nullptr;
  }

  inline Value* Value::Find(StringView key) noexcept {
    const Value* const found = static_cast<const Value*>(this)->Find(key);
    return const_cast<Value*>(found);
  }

  inline const Value& Value::operator[](StringView key) const noexcept {
    const Value* const found = Find(key);
    assert(found != nullptr && "Value::operator[](key): key not found");
    return (found != nullptr) ? *found : kNullValue;
  }

  inline const Member* Value::MemberBegin() const noexcept {
    return IsObject() ? m_payload.members : nullptr;
  }

  inline const Member* Value::MemberEnd() const noexcept {
    return IsObject() ? m_payload.members + m_size : nullptr;
  }

}
