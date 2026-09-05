#pragma once

/**
 * @file  JsonArchiveContainers.h
 * @brief 追加の依存を必要とするコンテナ型のアーカイブ対応 (§5.4)
 *
 * @details
 *  **このヘッダは `DynamicArray.h`(`<memory>` `<stdexcept>` `<algorithm>`
 *  `<iterator>` を引き込む)と `<optional>` に依存する。** したがって
 *  `JsonArchiveTypes.h` とは分けてあり、**利用者が明示的に include したときだけ**
 *  これらが入る(R3-16)。T-33 で `/showIncludes` により機械的に検証する。
 *
 *  分割の基準は「スカラかコンテナか」ではなく **「追加の依存が発生するか」**
 *  である【1-6 で確定】。T-33 が測っているのは依存だからである。
 *  依存を増やさない `T[N]` は `JsonArchiveTypes.h` 側にある。
 *
 *  @note `HashMap<K,V>` は §5.4.2 により恒久的にスコープ外。
 */

#include <cstdint>
#include <optional>
#include <type_traits>

#include "Core/DynamicArray.h"
#include "Core/Json/JsonArchive.h"

namespace GLFD::Json {

  namespace Detail {

    /**
     * @brief 外からメモリリソースを差し替えられる型
     *
     * @note **`DynamicArray` に限定していない。** ここで見ているのは「型が何か」
     *       ではなく「**リソースを受け取れるか**」である。`SetResource` を持つ型は
     *       すべて対象になるので、将来 `IMemoryResource` を受け取る自作コンテナが
     *       増えても、そのまま入れ子にできる。
     *       現時点でこれを満たすのは `DynamicArray<T>` だけである。
     */
    template <class T>
    concept ReceivesMemoryResource = requires(T& v, GLFD::Memory::IMemoryResource* resource) {
      v.SetResource(resource);
    };

  }

  // ---------------------------------------------------------------------------
  // DynamicArray<T>
  // ---------------------------------------------------------------------------

  /**
   * @brief 可変長配列を JSON 配列として読み書きする (R3-13 / R3-20)
   *
   * @details
   *  読み側は `BeginArray` が返す**実要素数**まで `TryResize` で伸ばしてから
   *  要素を読む。**投擲 API は一切呼ばない。**
   *  `TryResize` の失敗は `ContainerFailure` として記録し `HasFatal()` を立てる。
   *
   *  失敗の内訳は 2 つあるが、アーカイブ層から見れば「要求サイズを用意できなかった」
   *  という 1 つの事象なので区別しない:
   *   - 確保失敗(メモリ不足)
   *   - **`IMemoryResource` が未設定**(`DynamicArray<T> a;` と書けてしまうため
   *     現実に起こる)。`TryReallocate` の先頭に `if (!m_resource) return false;` が
   *     あるのでクラッシュせず `false` が返る
   *
   *  @note 空配列は `TryResize(0)` が確保を伴わないため、**リソース未設定でも成功する**。
   *        これは意図的な非対称である。空配列で不要な `ContainerFailure` を
   *        出さないための挙動であり、変更しないこと。
   */
  template <class T>
  struct JsonSerializer<GLFD::DynamicArray<T>> {
    // ---------------------------------------------------------------------
    //  **クラススコープに置くこと。**
    //  `Serialize` 本体に書くと関数が実体化されるまで発火せず、
    //  `ar.Member(...)` を書いた行ではなく無関係な場所で落ちる。
    //  クラススコープなら、コンパイラが
    //  「see reference to class template instantiation
    //    'JsonSerializer<DynamicArray<Foo>>' being compiled」を出すため、
    //  どの `T` が問題かを診断から追える。
    // ---------------------------------------------------------------------
    static_assert(
      GLFD::DynamicArray<T>::kNothrowRelocate,
      "DynamicArray<T> can only be archived when T is nothrow-move-constructible. "
      "Otherwise the element's own exception propagates out of TryResize/TryPushBack, "
      "which is std::terminate in the engine build (no /EH) and emits C4530 (R3-14). "
      "Fix: add 'noexcept' to T's move constructor.");

    template <class Ar>
    static bool Serialize(Ar& ar, GLFD::DynamicArray<T>& value) {
      const std::uint32_t oldSize = static_cast<std::uint32_t>(value.GetSize());

      std::uint32_t n = ar.BeginArray(oldSize);

      if constexpr (Ar::IsReading()) {
        // `existing` は「**既に構築済みで、リソース配布を済ませてある要素数**」。
        // `TryResize` で新しく既定構築された要素だけが未配布なので、下の伝播は
        // `[existing, n)` に限る。**要素を作り直したら必ずここも 0 へ戻すこと**
        std::uint32_t existing = oldSize;

        if (!ar.ArrayWasOpened()) {
          // 配列ではなかった。`TypeMismatch` は `BeginArray` が記録済みである。
          // **リサイズしない** — ここで縮めると、上書き読み (2-3) で
          // 直前のレイヤから読んだ内容が型の書き間違い 1 つで消える。
          // 空配列 `[]` は「開けている」ので下の `TryResize(0)` を通り、
          // 意図どおり空になる(この 2 つは区別しなければならない)
          n = 0;   // 走査もしない。EndArray は BeginArray と釣り合っている
        }
        else {
          if (ar.IsOverlay()) {
            // 上書き読み (2-3) では配列は**丸ごと置換**する。残すと、生き残った
            // 要素に前のレイヤの**同じ添字**の値が混ざる。配列の添字は要素の
            // 同一性を表さない(並び替えれば別の要素になる)ので、添字を鍵にして
            // 混ぜる設計に正当性がない。作り直せば「書いていない要素の
            // フィールドは構造体の既定値」の一言で説明できる
            value.Clear();
            existing = 0;   // 全要素が作り直される = 全部が未配布
          }

          if (!value.TryResize(static_cast<size_t>(n))) {
            ar.ReportContainerFailure();
            n = 0;   // 読める要素が無いので走査しない。EndArray は釣り合わせる
          }
          else if constexpr (Detail::ReceivesMemoryResource<T>) {
            // `TryResize` が**既定構築した要素だけ**がリソース未設定である。
            // 既存要素は作られた時点で配られているので触らない
            // (触ると `SetResource` の「空であること」の前提を破る)。
            //
            // 縮小 -> 再拡大の場合も、破棄された位置は再び `construct_at` を通るので
            // この範囲に含まれる。`Clear()` 後は `existing == 0` で全要素が対象。
            for (std::uint32_t i = existing; i < n; ++i) {
              value[static_cast<size_t>(i)].SetResource(value.GetResource());
            }
          }
        }
      }

      for (std::uint32_t i = 0; i < n; ++i) {
        (void)ar.Element(value[static_cast<size_t>(i)]);
      }
      return ar.EndArray();
    }
  };

  // ---------------------------------------------------------------------------
  // std::optional<T>
  // ---------------------------------------------------------------------------

  /**
   * @brief 値の有無を `null` で表す
   *
   * @details
   *  **`.value()` を呼ばない。** `throw` するのは `.value()` だけであり、
   *  `has_value()` / `operator*` / `emplace()` / `reset()` は非投擲である
   *  (エンジンの既存コードも `JobSystem` / `ThreadPool` / `HashMap` で
   *  すべて `operator bool` + `operator*` を使っており、`.value()` は 1 箇所も無い)。
   *
   *  **書き: 値なしは `null` を出力する。** 省略しないのは 2 つの理由による:
   *   1. キーが常に存在するので、読み側が `RequiredMember` を使っても
   *      `MissingRequired` にならない(R3-22 で `SkipDefaults` を
   *      `RequiredMember` に適用しないと決めたのと同じ罠を避ける)
   *   2. git diff で「値が消えた」ことが行の変化として見える(行ごと消えない)
   *
   *  省略したい場合は**専用の機構を足さずに** `SkipDefaults` が使える。
   *  `std::optional` は `operator==` を持つので R3-22 の `ArchiveComparable` を満たす:
   *  @code
   *    ar.Member("nickname", v.nickname, std::optional<StringView>{});
   *  @endcode
   *
   *  **読み**: `null` なら `reset()`、値があれば要素をディスパッチする。
   *  **キーが欠損した場合は R3-4 のとおり**アーカイブ側で処理され、ここへは来ない。
   *  欠損を無効値にしたい場合は上記のように既定値つき `Member` を使うこと。
   *  `optional` のためにディスパッチを特別扱いしない(R3-4 の一様性を保つ)。
   *
   *  @note 読み込み時に `T` は既定構築可能である必要がある(`emplace()` を使うため)。
   *  @note エンジンに自前 `Optional<T>` が入った場合は、この特殊化を 1 本足すだけで
   *        移行できる。`std::optional` は本ヘッダ以外のどこにも現れない。
   */
  template <class T>
  struct JsonSerializer<std::optional<T>> {
    template <class Ar>
    static bool Serialize(Ar& ar, std::optional<T>& value) {
      if constexpr (Ar::IsReading()) {
        if (ar.IsNull()) {
          value.reset();
          return true;
        }

        const bool hadValue = value.has_value();
        if (!hadValue) {
          value.emplace();
        }
        if (!Detail::ArchiveOne(ar, *value)) {
          if (!hadValue) {
            value.reset();   // 読めなかったので呼び出し前の状態へ戻す
          }
          return false;
        }
        return true;
      }
      else {
        if (!value.has_value()) {
          return ar.Null();
        }
        return Detail::ArchiveOne(ar, *value);
      }
    }
  };

}
