#pragma once

/**
 * @file  JsonDiagnostics.h
 * @brief 診断を人が読める 1 行に組み立てる(フェーズ 2-4)
 *
 * @details
 *  ## これは何か
 *  「ロードは成功したが一部おかしい」を**見えるようにする**ための整形だけを行う。
 *  新しい診断機構は作らない。材料はすべて既存のもの
 *  (`ArchiveContext::Issues()` / `ParseError` / `ComputeLocation`)。
 *
 *  ## 層の規律 — **`Logger` に依存しない**
 *  ここが提供するのは**文字列を組み立てる関数**であり、それをどこへ出すかは
 *  呼び出し側の関心事である。`Logger.h` を include した瞬間、JSON が
 *  エンジンの出力機構に縛られ、テストからも使いにくくなる。
 *  1-4 で `WriteValue` を自由関数にし、2-1 で `Query` を `Value` の
 *  メンバにしなかったのと同じ判断(T-53 が `/showIncludes` で機械的に検証する)。
 *
 *  ## なぜ「1 行ずつ」なのか
 *  `GLFD::Core::Logger` は **1 回の呼び出しにつき 1023 バイト**
 *  (`LogFmt` の `char buffer[1024]` + `snprintf`)で、超過分は黙って切れる。
 *  さらに時刻とレベルの前置きが付くのは**行頭だけ**なので、`\n` を詰めた
 *  1 本の文字列を渡すと 2 行目以降が素通しになる。
 *  したがって**行ごとに呼ぶ形しか成立しない**。この制約はログ側の仕様であり、
 *  ここでは行を作るところまでを担当する。
 *
 *  ## 確保しない
 *  すべて呼び出し側の `char` バッファへ書く。**常にヌル終端**し、
 *  容量が足りなければ切り詰める(戻り値で分かる)。
 */

#include <cassert>
#include <cstddef>
#include <cstdint>

#include <charconv>

#include "Core/StringView.h"
#include "Core/Json/JsonArchive.h"
#include "Core/Json/JsonTypes.h"
#include "Core/Json/JsonValue.h"

namespace GLFD::Json {

  /// 1 行ぶんの推奨バッファ長。`Logger` の 1023 バイトに収まる
  inline constexpr size_t kDiagnosticLineCapacity = 512;

  namespace Detail {

    /**
     * @brief 上限つきの行組み立て
     * @note  溢れても書き続けられる(以降を捨てるだけ)ので、呼び出し側に
     *        分岐が散らばらない。**必ずヌル終端する。**
     */
    class LineBuilder {
    public:
      LineBuilder(char* out, size_t capacity) noexcept
        : m_out(out), m_capacity(capacity) {
        // 容量 0 では終端すら書けない。呼び出し側の明確な誤りとして扱う
        assert(out != nullptr && capacity > 0 && "LineBuilder needs room for a terminator");
        m_out[0] = '\0';
      }

      void Append(StringView text) noexcept {
        for (size_t i = 0; i < text.Size(); ++i) { AppendChar(text[i]); }
      }

      void Append(const char* text) noexcept {
        if (text == nullptr) { return; }
        for (size_t i = 0; text[i] != '\0'; ++i) { AppendChar(text[i]); }
      }

      void AppendChar(char c) noexcept {
        if (m_size + 1u < m_capacity) { m_out[m_size++] = c; }
        else { m_truncated = true; }
      }

      void AppendUInt(std::uint32_t value) noexcept {
        // CRT の書式化を使わずに 10 進を書く(`PathStack::WriteDecimal` と同じ理由)
        char   temp[10];
        size_t count = 0;
        do {
          temp[count++] = static_cast<char>('0' + (value % 10u));
          value /= 10u;
        } while (value != 0u);
        while (count != 0u) { AppendChar(temp[--count]); }
      }

      void AppendInt64(std::int64_t value) noexcept {
        if (value < 0) {
          AppendChar('-');
          // 最小値の符号反転を避けるため、いったん符号なしへ移す
          AppendUInt64(static_cast<std::uint64_t>(-(value + 1)) + 1u);
          return;
        }
        AppendUInt64(static_cast<std::uint64_t>(value));
      }

      void AppendUInt64(std::uint64_t value) noexcept {
        char   temp[20];
        size_t count = 0;
        do {
          temp[count++] = static_cast<char>('0' + (value % 10u));
          value /= 10u;
        } while (value != 0u);
        while (count != 0u) { AppendChar(temp[--count]); }
      }

      /// `to_chars` の最短往復表現(`JsonWriter` と同じ流儀。CRT の書式化を使わない)
      void AppendDouble(double value) noexcept {
        char       temp[32];
        const auto result = std::to_chars(temp, temp + sizeof(temp), value);
        if (result.ec != std::errc{}) { Append("<double>"); return; }
        for (char* p = temp; p != result.ptr; ++p) { AppendChar(*p); }
      }

      /// 現在の長さが `column` に満たなければ空白で埋める(列を揃える)
      void PadTo(size_t column) noexcept {
        while (m_size < column) { AppendChar(' '); }
      }

      [[nodiscard]] size_t Size()      const noexcept { return m_size; }
      [[nodiscard]] bool   Truncated() const noexcept { return m_truncated; }

      /// ヌル終端して長さを返す
      [[nodiscard]] size_t Finish() noexcept {
        m_out[m_size] = '\0';
        return m_size;
      }

    private:
      char*  m_out;
      size_t m_capacity;
      size_t m_size      = 0;
      bool   m_truncated = false;
    };

  }

  /**
   * @brief パースの失敗を `file(line,col): Code` 形式の 1 行にする
   *
   * @param out      出力先。**必ずヌル終端される**
   * @param capacity `out` の容量(1 以上)
   * @param fileName 表示するファイル名。空なら省略される
   * @param code     `ParseError::code`
   * @param offset   `ParseError::offset`
   * @param source   パース元のテキスト。**空なら位置を出さない**
   * @return 書き込んだ長さ(終端を含まない)
   *
   * @details
   *  MSVC の診断と同じ `file(line,col): message` 形式にしてある。
   *  貼れば場所が伝わり、grep しやすく、人が読んで分かる。
   *
   *  `source` が空のときは `file: Code` に落ちる。これは飾りではなく、
   *  **`Document::Parse(StringView)` 経路では `Document::Source()` が空**
   *  (呼び出し側のバッファを指し続けない設計)であることに対応している。
   *  その経路の呼び出し側は元テキストを手に持っているので、必要なら
   *  自分で渡せばよい。
   *
   *  `column` は `ComputeLocation` がコードポイント単位で数える (R1-2)。
   *  日本語コメントを含む手書き config でもエディタの桁と大きくずれない。
   */
  [[nodiscard]] inline size_t FormatParseError(char* out, size_t capacity,
                                               StringView fileName, ErrorCode code,
                                               size_t offset, StringView source) noexcept {
    Detail::LineBuilder line(out, capacity);

    if (!fileName.Empty()) { line.Append(fileName); }

    if (!source.Empty()) {
      const SourceLocation location = ComputeLocation(source, offset);
      line.AppendChar('(');
      line.AppendUInt(location.line);
      line.AppendChar(',');
      line.AppendUInt(location.column);
      line.AppendChar(')');
    }

    if (!fileName.Empty() || !source.Empty()) { line.Append(": "); }
    line.Append(ToString(code));   // ToString(ErrorCode) は StringView を返す
    return line.Finish();
  }

  /**
   * @brief Issue 群のヘッダ行(`file: 3 issue(s), 1 fatal`)
   * @note  件数を先に出すのは、続く行が何行あるかを読む前に知るため
   */
  [[nodiscard]] inline size_t FormatIssueHeader(char* out, size_t capacity,
                                                StringView fileName,
                                                std::uint32_t issueCount,
                                                std::uint32_t fatalCount) noexcept {
    Detail::LineBuilder line(out, capacity);
    if (!fileName.Empty()) {
      line.Append(fileName);
      line.Append(": ");
    }
    line.AppendUInt(issueCount);
    line.Append(" issue(s), ");
    line.AppendUInt(fatalCount);
    line.Append(" fatal");
    return line.Finish();
  }

  /// `FormatIssue` が使う列位置。パスは `Query` にそのまま貼れる形のまま出す
  inline constexpr size_t kIssueKindColumn   = 2;
  inline constexpr size_t kIssuePathColumn   = 23;
  inline constexpr size_t kIssueDetailColumn = 47;

  /**
   * @brief Issue 1 件を `  Kind  path  detail` 形式の 1 行にする
   *
   * @details
   *  **パスは加工しない。** 2-1 の `Query` にそのまま貼れることが、
   *  R3-9 でこの形式を選んだ理由そのものである。
   *  ルート(空パス)だけは読めないので `<root>` と表示する
   *  (`Query` に渡すなら空文字列、という対応はガイドに書く)。
   */
  [[nodiscard]] inline size_t FormatIssue(char* out, size_t capacity,
                                          const ArchiveIssue& issue) noexcept {
    Detail::LineBuilder line(out, capacity);
    line.PadTo(kIssueKindColumn);
    line.Append(ToString(issue.kind));

    line.PadTo(kIssuePathColumn);
    if (issue.path.Empty()) { line.Append("<root>"); }
    else                    { line.Append(issue.path); }

    if (!issue.detail.Empty()) {
      line.PadTo(kIssueDetailColumn);
      line.Append(issue.detail);
    }
    return line.Finish();
  }

  /**
   * @brief Issue を 1 行ずつ組み立てて `onLine` へ渡す
   *
   * @param ctx      読み込みに使った文脈。**この関数は何も確保しない**
   * @param fileName ヘッダに出すファイル名
   * @param onLine   `void(StringView line, bool isFatal)` を満たす呼び出し可能なもの。
   *                 `isFatal` でログのレベルを選べる(Fatal は「読めなかった」、
   *                 非 Fatal は「起動はしたが一部おかしい」)
   *
   * @details
   *  **Issue が 0 件なら 1 行も呼ばない。** 正常時に余計な出力をしないこと自体が
   *  要件である(異常が出たときに目立たなくなる)。
   *
   *  `onLine` に渡す `StringView` は**この関数のスタック上のバッファ**を指す。
   *  呼び出しから戻った後は無効なので、保持するならコピーすること。
   */
  template <class OnLine>
  void ForEachIssueLine(const ArchiveContext& ctx, StringView fileName, OnLine&& onLine) {
    const std::uint32_t count = ctx.IssueCount();
    if (count == 0u && !ctx.DiagnosticsTruncated()) { return; }

    char line[kDiagnosticLineCapacity];

    std::uint32_t fatalCount = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
      if (IsFatal(ctx.Issues()[i].kind, ctx.Flags())) { ++fatalCount; }
    }

    const size_t headerSize = FormatIssueHeader(line, sizeof(line), fileName, count, fatalCount);
    onLine(StringView(line, headerSize), fatalCount != 0u);

    for (std::uint32_t i = 0; i < count; ++i) {
      const ArchiveIssue& issue = ctx.Issues()[i];
      const size_t        size  = FormatIssue(line, sizeof(line), issue);
      onLine(StringView(line, size), IsFatal(issue.kind, ctx.Flags()));
    }

    if (ctx.DiagnosticsTruncated()) {
      Detail::LineBuilder note(line, sizeof(line));
      note.PadTo(kIssueKindColumn);
      note.Append("(some diagnostics were dropped; the rest of the file was not reported)");
      onLine(StringView(line, note.Finish()), false);
    }
    if (ctx.IssuesTruncated()) {
      Detail::LineBuilder note(line, sizeof(line));
      note.PadTo(kIssueKindColumn);
      note.Append("(the issue list itself ran out of memory)");
      onLine(StringView(line, note.Finish()), true);
    }
  }

  // ---------------------------------------------------------------------------
  // 実効値のダンプ (成果物 C)
  // ---------------------------------------------------------------------------

  /**
   * @brief テキストを行に切って `onLine` へ渡す
   *
   * @details
   *  `SaveToJson(..., pretty = true)` の出力をログへ流すための道具。
   *  **ファイルに書いてある値ではなく、エンジンが実際に使っている値**が出る
   *  (欠損は既定値で埋まり (R3-4)、未知フィールドは捨てられ (R3-5)、
   *  範囲外は拒否されて既定のまま残る (R3-11))。チューニング中に見たいのは後者。
   *
   *  1 行ずつ渡すのは `Logger` の 1023 バイト制限のため(ファイル冒頭を参照)。
   *  現在の `GameConfig` の pretty 出力は実測 1259 バイトで、1 回では出せない。
   *
   *  `\r\n` は行末の `\r` を落として渡す。空行もそのまま 1 行として渡す。
   */
  template <class OnLine>
  void ForEachLine(StringView text, OnLine&& onLine) {
    size_t begin = 0;
    while (begin <= text.Size()) {
      size_t end = begin;
      while (end < text.Size() && text[end] != '\n') { ++end; }

      size_t stop = end;
      if (stop > begin && text[stop - 1u] == '\r') { --stop; }
      onLine(text.SubStr(begin, stop - begin));

      if (end >= text.Size()) { break; }
      begin = end + 1u;
    }
  }

  // ---------------------------------------------------------------------------
  // リロード時の差分 (成果物 D)
  // ---------------------------------------------------------------------------

  namespace Detail {

    /// 差分の走査中にパスを組み立てる。R3-9 と同じ形式で、確保はしない
    class DiffPath {
    public:
      [[nodiscard]] size_t Mark() const noexcept { return m_size; }
      void Rewind(size_t mark) noexcept { m_size = mark; }

      void PushKey(StringView key) noexcept {
        Separate();
        for (size_t i = 0; i < key.Size(); ++i) { Put(key[i]); }
      }

      void PushIndex(std::uint32_t index) noexcept {
        Separate();
        char   temp[10];
        size_t count = 0;
        do {
          temp[count++] = static_cast<char>('0' + (index % 10u));
          index /= 10u;
        } while (index != 0u);
        while (count != 0u) { Put(temp[--count]); }
      }

      [[nodiscard]] StringView View() const noexcept { return StringView(m_buffer, m_size); }

    private:
      void Separate() noexcept { if (m_size != 0u) { Put('/'); } }
      void Put(char c) noexcept { if (m_size < sizeof(m_buffer)) { m_buffer[m_size++] = c; } }

      char   m_buffer[256] = {};
      size_t m_size        = 0;
    };

    /// 表示用の型名。数値は 3 種をまとめて `number` と呼ぶ
    [[nodiscard]] inline const char* TypeName(const Value& value) noexcept {
      switch (value.Type()) {
        case ValueType::Null:   return "null";
        case ValueType::Bool:   return "bool";
        case ValueType::Int64:
        case ValueType::UInt64:
        case ValueType::Double: return "number";
        case ValueType::String: return "string";
        case ValueType::Array:  return "array";
        case ValueType::Object: return "object";
      }
      return "<unknown>";
    }

    /// スカラを 1 つ書く。文字列は長ければ切る(差分は読むためのもの)
    inline void AppendScalar(LineBuilder& line, const Value& value) noexcept {
      constexpr size_t kMaxStringBytes = 32;
      switch (value.Type()) {
        case ValueType::Null:   line.Append("null"); break;
        case ValueType::Bool:   line.Append(value.GetBool() ? "true" : "false"); break;
        case ValueType::Int64:  line.AppendInt64(value.GetInt64()); break;
        case ValueType::UInt64: line.AppendUInt64(value.GetUInt64()); break;
        case ValueType::Double: line.AppendDouble(value.GetDouble()); break;
        case ValueType::String: {
          const StringView text = value.GetString();
          line.AppendChar('"');
          line.Append(text.SubStr(0, kMaxStringBytes));
          if (text.Size() > kMaxStringBytes) { line.Append("..."); }
          line.AppendChar('"');
          break;
        }
        case ValueType::Array:  line.Append("[...]"); break;
        case ValueType::Object: line.Append("{...}"); break;
      }
    }

    /// スカラ 2 つが同じ表示になるか。数値は型ではなく**表示**で比べる
    [[nodiscard]] inline bool SameScalarText(const Value& a, const Value& b) noexcept {
      char        textA[64];
      char        textB[64];
      LineBuilder builderA(textA, sizeof(textA));
      LineBuilder builderB(textB, sizeof(textB));
      AppendScalar(builderA, a);
      AppendScalar(builderB, b);
      const size_t sizeA = builderA.Finish();
      const size_t sizeB = builderB.Finish();
      return StringView(textA, sizeA) == StringView(textB, sizeB);
    }

    template <class OnLine>
    void DiffNode(const Value& before, const Value& after, DiffPath& path, OnLine& onLine);

    /// `+ path` / `- path` の 1 行
    template <class OnLine>
    void EmitPresence(char sign, const DiffPath& path, const Value& value, OnLine& onLine) {
      char        buffer[kDiagnosticLineCapacity];
      LineBuilder line(buffer, sizeof(buffer));
      line.PadTo(2);
      line.AppendChar(sign);
      line.AppendChar(' ');
      line.Append(path.View());
      line.PadTo(kIssuePathColumn + 8u);
      AppendScalar(line, value);
      onLine(StringView(buffer, line.Finish()));
    }

    template <class OnLine>
    void DiffObjects(const Value& before, const Value& after, DiffPath& path, OnLine& onLine) {
      const Member* const first = before.MemberBegin();
      const Member* const last  = before.MemberEnd();
      for (const Member* m = first; m != last; ++m) {
        const size_t mark = path.Mark();
        path.PushKey(m->key);
        const Value* const other = after.Find(m->key);
        if (other == nullptr) { EmitPresence('-', path, m->value, onLine); }
        else                  { DiffNode(m->value, *other, path, onLine); }
        path.Rewind(mark);
      }
      // 増えたメンバ
      const Member* const addedFirst = after.MemberBegin();
      const Member* const addedLast  = after.MemberEnd();
      for (const Member* m = addedFirst; m != addedLast; ++m) {
        if (before.Find(m->key) != nullptr) { continue; }
        const size_t mark = path.Mark();
        path.PushKey(m->key);
        EmitPresence('+', path, m->value, onLine);
        path.Rewind(mark);
      }
    }

    template <class OnLine>
    void DiffArrays(const Value& before, const Value& after, DiffPath& path, OnLine& onLine) {
      const Value* const beforeFirst = before.Begin();
      const Value* const afterFirst  = after.Begin();
      const std::uint32_t beforeSize =
        static_cast<std::uint32_t>(before.End() - beforeFirst);
      const std::uint32_t afterSize =
        static_cast<std::uint32_t>(after.End() - afterFirst);

      if (beforeSize != afterSize) {
        // 長さが変わったことだけを 1 行で出す。中身は共通部分を追う
        char        buffer[kDiagnosticLineCapacity];
        LineBuilder line(buffer, sizeof(buffer));
        line.PadTo(2);
        line.Append(path.View().Empty() ? StringView("<root>") : path.View());
        line.PadTo(kIssuePathColumn + 8u);
        line.AppendChar('[');
        line.AppendUInt(beforeSize);
        line.Append("] -> [");
        line.AppendUInt(afterSize);
        line.AppendChar(']');
        onLine(StringView(buffer, line.Finish()));
      }

      const std::uint32_t common = (beforeSize < afterSize) ? beforeSize : afterSize;
      for (std::uint32_t i = 0; i < common; ++i) {
        const size_t mark = path.Mark();
        path.PushIndex(i);
        DiffNode(beforeFirst[i], afterFirst[i], path, onLine);
        path.Rewind(mark);
      }
    }

    template <class OnLine>
    void DiffNode(const Value& before, const Value& after, DiffPath& path, OnLine& onLine) {
      const bool sameShape =
        (before.IsObject() && after.IsObject()) || (before.IsArray() && after.IsArray())
        || (before.IsNumber() && after.IsNumber()) || (before.Type() == after.Type());

      if (!sameShape) {
        // 型が変わった場合は**型名だけ**を出す。チューニングで起きるのは稀で、
        // 値まで整形しても読み手の役に立たない
        char        buffer[kDiagnosticLineCapacity];
        LineBuilder line(buffer, sizeof(buffer));
        line.PadTo(2);
        line.Append(path.View().Empty() ? StringView("<root>") : path.View());
        line.PadTo(kIssuePathColumn + 8u);
        line.Append(TypeName(before));
        line.Append(" -> ");
        line.Append(TypeName(after));
        onLine(StringView(buffer, line.Finish()));
        return;
      }

      if (before.IsObject()) { DiffObjects(before, after, path, onLine); return; }
      if (before.IsArray())  { DiffArrays(before, after, path, onLine);  return; }

      if (SameScalarText(before, after)) { return; }

      char        buffer[kDiagnosticLineCapacity];
      LineBuilder line(buffer, sizeof(buffer));
      line.PadTo(2);
      line.Append(path.View().Empty() ? StringView("<root>") : path.View());
      line.PadTo(kIssuePathColumn + 8u);
      AppendScalar(line, before);
      line.Append(" -> ");
      AppendScalar(line, after);
      onLine(StringView(buffer, line.Finish()));
    }

  }

  /**
   * @brief 2 つの DOM を並行に走査し、**変わったところだけ**を 1 行ずつ出す
   *
   * @param before 前の DOM(例: 差し替え前の `Document::Root()`)
   * @param after  後の DOM
   * @param onLine `void(StringView line)` を満たす呼び出し可能なもの
   *
   * @details
   *  出力するパスは **R3-9 と同じ形式**で、そのまま `Query` に貼れる。
   *
   *  | 変化 | 出力 |
   *  |---|---|
   *  | 同型スカラ | `simulation/maxSpeed   2 -> 6.5` |
   *  | 型が変わった | `simulation/maxSpeed   number -> string`(**型名だけ**) |
   *  | 配列長 | `boidProfiles   [3] -> [2]`(共通部分は中身も追う) |
   *  | メンバ増減 | `+ world/gravity` / `- world/damping` |
   *
   *  **数値は型ではなく表示で比べる。** `2` と `2.0` は JSON 上の型が違うが
   *  (`Int64` / `Double`)、float のフィールドにとっては同じ値であり、
   *  「変わった」と出す価値が無い。
   *
   *  **変化が無ければ 1 行も呼ばない。** テキスト比較ではなく DOM 比較にしてあるのは、
   *  確保を伴わず、整形の違い(空白・キーの順)に引きずられないため。
   *
   *  @note 確保はしない。パスは 256 バイトの固定バッファに組み立てるため、
   *        極端に深い / 長いキーでは末尾が切れる(診断であり、切れても害はない)
   */
  template <class OnLine>
  void DiffValues(const Value& before, const Value& after, OnLine&& onLine) {
    Detail::DiffPath path;
    Detail::DiffNode(before, after, path, onLine);
  }

}
