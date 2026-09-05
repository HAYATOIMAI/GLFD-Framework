#pragma once

/**
 * @file  JsonArchiveTypes.h
 * @brief 組み込み型のアーカイブ対応表 (§5.4)
 *
 * @details
 *  **依存は `JsonArchive.h` のみ。** `ReadArchive` / `WriteArchive` の具体型に
 *  触れると R3-16 のヘッダ分割が無意味になるため、呼び出し面にしか依存しない。
 *  T-23 で `/showIncludes` により機械的に検証する。
 *
 *  ここが持つのは「この型は組み込みスカラである」という**関門**だけである。
 *  実際の読み書きは各アーカイブの `Scalar()` が `std::is_integral_v` などで
 *  分岐して行うため、幅ごとの重複が生じない。
 *
 *  組み込み型を `Serialize` の**オーバーロード**で提供しない理由:
 *  `ar` の型が `GLFD::Json` に属するため、`Serialize(ar, v)` はどんな `v` に対しても
 *  `GLFD::Json` を ADL の対象に含む。そこへ `Serialize(Ar&, int&)` を置くと、
 *  無関係な呼び出しにも常に候補として見えてしまう。特性テーブルなら曖昧さが無い。
 *
 *  @note **分割の基準は「追加の依存が発生するか」であって、スカラかコンテナかではない**
 *        【1-6 で確定】。T-33 が `/showIncludes` で測っているのは依存だからである。
 *        このヘッダには追加の include を必要としない型だけを置く:
 *        `bool` / 各整数幅 / `float` / `double` / `StringView` / `enum` / **`T[N]`**。
 *        `T[N]` は言語機能なので依存を増やさない。`float[3]`(位置・色・回転)は
 *        ゲームの構造体に頻出するため、これを `JsonArchiveContainers.h` 側へ置くと
 *        **最も頻出する型のせいでセーブ専用の翻訳単位が STL を引き込む**ことになる。
 *        `DynamicArray<T>`(`DynamicArray.h` が要る)と `std::optional<T>`(`<optional>`
 *        が要る)は `JsonArchiveContainers.h` に置く。
 *        `HashMap<K,V>` は §5.4.2 により恒久的にスコープ外。
 *
 *  @note `char` / `long` / `unsigned long` は含めない。`char` は文字か数値かが
 *        曖昧で、`long` は幅が処理系依存のため、セーブデータの型としては
 *        `std::int32_t` などの固定幅を明示させる方が安全である。
 */

#include <cstdint>
#include <type_traits>

#include "Core/StringView.h"
#include "Core/Json/JsonArchive.h"

namespace GLFD::Json {

  namespace Detail {
    /// `ArchiveScalar` 特殊化の中身。関門であることを 1 箇所で表す
    struct ScalarTag {
      static constexpr bool kIsScalar = true;
    };
  }

  template <> struct ArchiveScalar<bool>          : Detail::ScalarTag {};

  template <> struct ArchiveScalar<std::int8_t>   : Detail::ScalarTag {};
  template <> struct ArchiveScalar<std::int16_t>  : Detail::ScalarTag {};
  template <> struct ArchiveScalar<std::int32_t>  : Detail::ScalarTag {};
  template <> struct ArchiveScalar<std::int64_t>  : Detail::ScalarTag {};

  template <> struct ArchiveScalar<std::uint8_t>  : Detail::ScalarTag {};
  template <> struct ArchiveScalar<std::uint16_t> : Detail::ScalarTag {};
  template <> struct ArchiveScalar<std::uint32_t> : Detail::ScalarTag {};
  template <> struct ArchiveScalar<std::uint64_t> : Detail::ScalarTag {};

  template <> struct ArchiveScalar<float>         : Detail::ScalarTag {};
  template <> struct ArchiveScalar<double>        : Detail::ScalarTag {};

  template <> struct ArchiveScalar<StringView>    : Detail::ScalarTag {};

  // ---------------------------------------------------------------------------
  // 組み込み配列 T[N] (§5.4)
  // ---------------------------------------------------------------------------

  /**
   * @brief 固定長配列を JSON 配列として読み書きする
   *
   * @details
   *  **依存を増やさないのでこのヘッダに置く**(1-6 の分割基準)。`float[3]`
   *  (位置・色・回転)はゲームの構造体に頻出するため、これを
   *  `JsonArchiveContainers.h` 側に置くと、最も頻出する型のせいで
   *  セーブ専用の翻訳単位が `DynamicArray.h` 経由で STL を引き込むことになる。
   *
   *  要素数が合わない場合は `ArrayLengthMismatch` を記録する。`Fatal` ではない。
   *   - JSON が短い: 先頭 `n` 個を読み、**残りは触らない**
   *     (構造体側の初期化子が既定値として残る。既定値なし `Member` と同じ考え方)
   *   - JSON が長い: 先頭 `N` 個を読み、超過分は無視する
   *
   * @warning **読み書きで長さが変わり得る。** 読み側は短い配列を許容するが、
   *          書き側は常にちょうど `N` 要素を出す。したがって手書き config の
   *          `float[4]` に `[1, 2]` と書いてあると、読んでエディタから書き戻した
   *          時点で `[1, 2, 0, 0]` になる。意図した挙動である。
   *
   * @note 既定値つきの `Member(key, v, def)` は配列に使えない(配列は代入できない)。
   *       2 引数の `Member(key, v)` を使い、既定値は構造体側の初期化子で持つこと。
   */
  template <class T, std::size_t N>
  struct JsonSerializer<T[N]> {
    static_assert(N > 0, "JsonSerializer<T[N]>: zero-length arrays are not supported");

    template <class Ar>
    static bool Serialize(Ar& ar, T (&value)[N]) {
      constexpr std::uint32_t kCount = static_cast<std::uint32_t>(N);

      std::uint32_t n = ar.BeginArray(kCount);

      if constexpr (Ar::IsReading()) {
        if (n != kCount) {
          ar.ReportArrayLengthMismatch();

          if (ar.IsOverlay()) {
            // 上書き読み (2-3) では**要素数が合わなければ 1 要素も書かない**。
            // 固定長配列には「既定値へ戻す」手段が無い(既定は構造体側の
            // 初期化子が持つ。R3-26)ため、部分的に書くと残りが**前のレイヤの
            // 値のまま混ざる**。`DynamicArray` を丸ごと置換にしたのと同じ理由で、
            // 添字ごとに混ざる形を避ける。上書きしたいなら全要素を書くこと
            n = 0;
          }
          else if (n > kCount) {
            n = kCount;   // 超過分は読まない
          }
        }
      }

      for (std::uint32_t i = 0; i < n; ++i) {
        (void)ar.Element(value[i]);
      }
      return ar.EndArray();
    }
  };

  // ---------------------------------------------------------------------------
  // enum / enum class (§5.4)
  // ---------------------------------------------------------------------------

  /**
   * @brief 列挙を基底整数型として読み書きする
   *
   * @details
   *  アーカイブ側には一切手を入れていない。基底整数型へ変換して
   *  `Detail::ArchiveOne` へ渡し直すだけなので、範囲チェック (R3-11) は
   *  基底型の規則がそのまま適用される。
   *
   *  **文字列表現は `JsonSerializer<E>` の完全特殊化による opt-in** (R3-2)。
   *  完全特殊化は部分特殊化より優先されるため、この既定を上書きできる。
   *  専用の機構は設けない。
   *
   * @warning 整数として保存された列挙は、**列挙子の値を変えると旧セーブデータの
   *          意味が変わる**。分かれ目はスコープ付きかどうかではなく、
   *          **値を明示しているか**である。
   *
   *          @code
   *            enum class LogLevel { Info, Warning, Error };               // 暗黙 0,1,2 -> 危険
   *            enum class ShaderType { Vertex, Pixel, Geometry, Compute }; // 暗黙      -> 危険
   *            enum class TransitionType { Push, Pop, Change };            // 暗黙      -> 危険
   *            enum KeyCode : int { Space = VK_SPACE, ... };               // 明示      -> 安全
   *          @endcode
   *
   *          前者は列挙子を途中に挿入しただけで、旧データの `1` が `Warning` から
   *          別の意味に変わる。
   *
   *          **指針: セーブデータや config に乗る列挙は、値を明示すること。**
   *          それが難しい場合や、手書き config に人間が書く列挙は、
   *          `JsonSerializer<E>` 特殊化による文字列表現を検討すること。
   *
   * @warning **基底型を明示していない `enum`(`enum Foo { A, B };`)は、範囲外の値を
   *          読み戻すと未定義動作になる。** 基底型が固定されていないため、
   *          列挙子の範囲を超える値への `static_cast` が規格上未定義だからである。
   *          `enum Foo : int { ... }` のように基底型を書けば、その型の全値に対して
   *          変換が定義される。上記の「値を明示する」指針と併せて守ること。
   */
  template <class E>
    requires std::is_enum_v<E>
  struct JsonSerializer<E> {
    template <class Ar>
    static bool Serialize(Ar& ar, E& value) {
      using Underlying = std::underlying_type_t<E>;

      if constexpr (Ar::IsReading()) {
        Underlying raw{};
        if (!Detail::ArchiveOne(ar, raw)) {
          return false;   // 範囲外なら RangeOverflow が記録済み。value は触らない
        }
        value = static_cast<E>(raw);
        return true;
      }
      else {
        Underlying raw = static_cast<Underlying>(value);
        return Detail::ArchiveOne(ar, raw);
      }
    }
  };

}
