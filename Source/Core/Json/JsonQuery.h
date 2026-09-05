#pragma once

/**
 * @file  JsonQuery.h
 * @brief 診断パスで DOM を引く読み取りユーティリティ(フェーズ 2-1)
 *
 * @details
 *  ## これは何か
 *  `ArchiveIssue::path`(R3-9)が出す `"window/width"` 形式の文字列を、
 *  **1バイトも書き換えずにそのまま渡して**該当の `Value` を引くための関数。
 *  ログに出たパスをコピーして貼れることが、この機能の存在理由である。
 *
 *  ```cpp
 *  for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
 *      const Value* v = doc.Query(ctx.Issues()[i].path);   // 問題の値そのもの
 *  }
 *  ```
 *
 *  ## なぜ `Value` のメンバではないのか
 *  `"/"` 区切りというパス形式は **R3-9(Layer 3 の診断)が決めた規約**であり、
 *  Layer 2 の DOM の性質ではない。`Value::Query` にすると、DOM の公開 API が
 *  知る必要のない規約を抱え、将来 JSON Pointer のエスケープを足したくなった
 *  ときに Layer 2 を変えることになる。1-4 で `WriteValue` を `JsonWriter` の
 *  メンバにせず自由関数にしたのと同じ判断である。
 *  `Document::Query` だけは要件書と R3-9 が約束済みの形なので転送を置いてある
 *  (宣言は `JsonDocument.h`、実体は `JsonDocument.cpp`。**ヘッダからこの
 *  ファイルを include しない** — すると Layer 2 が規約ヘッダを引き込む)。
 *
 *  ## 性質
 *  - **確保を一切行わない。** `StringView` のスライスだけで降りる (T-45)
 *  - 見つからなければ `nullptr`。`assert` しない(存在確認が正当な用途である)
 *  - `kNullValue` は返さない。返すと「存在しない」と「`null` が入っている」を
 *    区別できなくなる
 *  - 返り値の寿命は **`Document` のアリーナに従う** (R0-5)。`Clear()` /
 *    `Parse()` の再実行 / 破棄のいずれかで無効になる
 *
 *  ## 引けないキー(**意図的な制約。バグではない**)
 *  `Query` ではなく**診断パス形式そのものの制約**で、`Query` はそれを
 *  引き継いでいる。必要になった時点で RFC 6901 のエスケープを検討する。
 *  いずれも**引けなかったことが `nullptr` で分かる**。
 *
 *  | キー | 位置 | 結果 |
 *  |---|---|---|
 *  | `""`(空キー) | ルート直下 | `Materialize` の出力が空文字列でルートと同じ。**ルートが返る** |
 *  | `""`(空キー) | ネストした先 | `{"a":{"":1}}` の出力は `"a/"`。末尾の空セグメントで `nullptr` |
 *  | `"..."` | どこでも | 截断の印(R3-9)と衝突する。`nullptr` |
 *
 *  ## `/` を含むキー — **唯一、誤った値が返り得るケース**
 *  上の3つと違い、これは `nullptr` にならない。**静かに別のノードを返す。**
 *
 *  ```
 *  {"a/b": 1, "a": {"b": 2}}     診断が "a/b" を報告 -> Query は 2 を返す
 *  ```
 *
 *  `nullptr` なら「引けなかった」と分かるが、これは分からない。**検出する手段は
 *  無い** — パスの側にエスケープが無い以上、2つを区別する情報がそもそも存在しない。
 *  防波堤は「**キー名に `/` を使わない**」という運用の規律だけである。
 *  手書きの config では特にそうで、`Resource/GameConfig.jsonc` の冒頭にも
 *  同じ指針を書いてある。
 *
 *  ## `MissingRequired` だけは `nullptr` になる
 *  必須フィールドの欠損は「JSON に無いメンバ」のパスを報告する。したがって
 *  `Query` は `nullptr` を返す。**これは欠点ではなく有用な性質で**、
 *  「パスが引けない = そのフィールドが JSON に無い」と読める。
 */

#include <cstddef>
#include <cstdint>

#include "Core/StringView.h"
#include "Core/Json/JsonValue.h"

namespace GLFD::Json {

  namespace Detail {

    /**
     * @brief 截断の印。`PathStack::Materialize` がパス末尾に書く (R3-9)
     * @note  `"."` と `".."` は予約しない。相対パスの意味論を持ち込まない以上
     *        それらは普通のキーであり、予約するのは実際に書かれる印だけでよい
     */
    inline constexpr StringView kQueryTruncationMarker("...");

    /**
     * @brief セグメントを配列の添字として**厳格に**解釈する
     *
     * @details
     *  R1-16 で `from_chars` が不正トークンを素通しした経験があるため、
     *  ここでは丸投げせず自前で走査する。以下はすべて false を返す。
     *
     *  | 入力 | 落ちる場所 |
     *  |---|---|
     *  | `""` | 長さ 0 |
     *  | `"01"` / `"00"` | 先頭ゼロ(`"0"` 単独は正当) |
     *  | `"+1"` / `"-1"` / `" 3"` / `"3 "` / `"3x"` / `"x3"` | 数字以外のバイト |
     *  | `"4294967296"` | `uint64` で積んでから `UINT32_MAX` と比較する。
     *                     **桁溢れで小さい値にならない** |
     *  | `"99999999999"` | 11 桁で早期に落ちる |
     */
    [[nodiscard]] inline bool ParseArrayIndex(StringView segment, std::uint32_t& out) noexcept {
      constexpr size_t        kMaxDigits = 10;              // UINT32_MAX = 4294967295
      constexpr std::uint64_t kMaxValue  = 0xFFFFFFFFull;

      const size_t size = segment.Size();
      if (size == 0 || size > kMaxDigits) { return false; }
      if (segment[0] == '0' && size > 1) { return false; }

      std::uint64_t value = 0;
      for (size_t i = 0; i < size; ++i) {
        const char c = segment[i];
        if (c < '0' || c > '9') { return false; }
        value = (value * 10u) + static_cast<std::uint64_t>(c - '0');
      }
      if (value > kMaxValue) { return false; }

      out = static_cast<std::uint32_t>(value);
      return true;
    }

  }

  /**
   * @brief パスで `Value` を引く。見つからなければ `nullptr`
   *
   * @param root 起点。`Document::Root()` とは限らず、部分木にも使える
   * @param path `"window/width"` 形式。**空なら `root` 自身**を返す
   * @return 見つかった値。無ければ `nullptr`(`kNullValue` は返さない)
   *
   * @details
   *  ## 解決規則 — 現在のノードの型がセグメントの解釈を決める
   *  | 現在のノード | セグメントの扱い |
   *  |---|---|
   *  | Object | **キー**として `Find`。重複キーでは最初の一致 (R2-3) |
   *  | Array  | **10進の添字**として厳格に解釈 |
   *  | それ以外(スカラ / Null) | 解決不能 → `nullptr` |
   *
   *  型が解釈を決めるので曖昧さは生じない。キー `"3"` を持つオブジェクトは
   *  `"3"` で引け、配列に `"name"` を渡せば `nullptr` になる。
   *
   *  ## 空セグメントは常に不正
   *  先頭 `/` / 末尾 `/` / `"//"` はすべて `nullptr`。**受理する形を
   *  `Materialize` の出力と1対1にする**ためで、親切に受け付けると
   *  「診断が出す形」と「`Query` が受ける形」がずれ始める。
   *  唯一の例外が「パス全体が空 = ルート」で、これは `Materialize` が
   *  ルート直下の Issue に対して空の `StringView` を返すことと対になっている。
   *
   *  ## 截断されたパスは解決しない
   *  `"..."` がどこかに現れたら `nullptr`。截断されたパスは
   *  **それ自体が「途中で追跡をやめた」という情報**であり、指す値は
   *  決まっていない。ここで何かを返せば、それは必ず利用者が求めたのとは
   *  別のノードである。
   *
   *  @note `Size()` / `MemberCount()` / `operator[]` は型不一致や範囲外で
   *        Debug に assert する (R2-6a) ため**一切使っていない**。
   *        存在しない添字を引くことが正当な用途であるこの関数では、
   *        `IsArray()` / `IsObject()` + 自前の範囲比較 + `Find` が唯一の書き方になる。
   */
  [[nodiscard]] inline const Value* Query(const Value& root, StringView path) noexcept {
    // 空パス = ルート自身。`Data()` は見ない(`Materialize` は
    // `Data() == nullptr` の空ビューを返すため)
    if (path.Size() == 0) { return &root; }

    const Value* current = &root;
    size_t       begin   = 0;

    for (;;) {
      const size_t     slash   = path.Find('/', begin);
      const size_t     end     = (slash == StringView::npos) ? path.Size() : slash;
      const StringView segment = path.SubStr(begin, end - begin);

      if (segment.Empty()) { return nullptr; }                              // 空セグメント
      if (segment == Detail::kQueryTruncationMarker) { return nullptr; }    // 截断の印

      if (current->IsObject()) {
        current = current->Find(segment);   // 不在なら nullptr。assert しない
        if (current == nullptr) { return nullptr; }
      }
      else if (current->IsArray()) {
        std::uint32_t index = 0;
        if (!Detail::ParseArrayIndex(segment, index)) { return nullptr; }

        // `Begin()` / `End()` は型不一致でも assert せず nullptr を返す
        const Value* const first = current->Begin();
        const Value* const last  = current->End();
        if (first == nullptr) { return nullptr; }                    // 空配列
        if (index >= static_cast<std::uint32_t>(last - first)) { return nullptr; }
        current = first + index;
      }
      else {
        return nullptr;   // スカラ / Null の先は解決できない
      }

      if (slash == StringView::npos) { return current; }
      begin = slash + 1u;
    }
  }

}
