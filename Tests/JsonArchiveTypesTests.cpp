/**
 * @file  JsonArchiveTypesTests.cpp
 * @brief フェーズ 1-6: 組み込みスカラ / enum のテスト
 *
 * @details
 *  対応する要件:
 *   - T-29 範囲チェックの網羅マトリクス(**本フェーズの中核テスト**)
 *   - T-31 `enum` / `enum class`
 *
 *  1-5b の `JsonArchiveTests` とは別スイートにしてある。T-35(非回帰)で
 *  既存スイートのチェック数が動かないことを確認するため、
 *  1-6 で増える検証は新しいスイートへ入れる。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

#include "Core/Json/JsonArchive.h"
#include "Core/Json/JsonArchiveTypes.h"
#include "Core/Json/JsonDocument.h"
#include "Core/Json/JsonReadArchive.h"
#include "Core/Json/JsonWriteArchive.h"
#include "Core/Json/JsonWriter.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::Document;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::JsonWriter;
using GLFD::Json::ParseFlags;
using GLFD::Json::ReadArchive;
using GLFD::Json::WriteArchive;
using GLFD::Test::MockMemoryResource;

namespace {

  using CompactWriter = JsonWriter<JsonStringBuffer>;

  [[nodiscard]] bool ViewEquals(StringView view, const char* expected) {
    const size_t length = std::strlen(expected);
    return view.Size() == length
           && (length == 0 || std::memcmp(view.Data(), expected, length) == 0);
  }

  // ---------------------------------------------------------------------------
  // 1 つの値を読むだけの最小の足場
  // ---------------------------------------------------------------------------

  /// `{"v": <json>}` を作って `v` を `out` へ読む
  template <class T>
  struct ReadResult {
    bool          ok    = false;
    bool          fatal = false;
    std::uint32_t issueCount = 0;
    ArchiveErrorKind firstKind = ArchiveErrorKind::TypeMismatch;
  };

  template <class T>
  ReadResult<T> ReadValue(MockMemoryResource& mock, const char* numberLiteral, T& out) {
    char json[128];
    std::snprintf(json, sizeof(json), "{\"v\":%s}", numberLiteral);

    ReadResult<T> result;

    Document doc(&mock);
    if (!doc.Parse(StringView(json), ParseFlags::None).IsOk()) {
      return result;   // パース自体が失敗したら ok=false のまま返す
    }

    JsonArena      arena(&mock);
    ArchiveContext ctx(arena);
    {
      ReadArchive ar(doc.Root(), ctx);
      result.ok = ar.Member("v", out);
    }
    result.fatal      = ctx.HasFatal();
    result.issueCount = ctx.IssueCount();
    if (ctx.IssueCount() != 0) {
      result.firstKind = ctx.Issues()[0].kind;
    }
    return result;
  }

  /// 値ひとつを書き出して JSON テキストを得る
  template <class T>
  bool WriteValue(JsonArena& arena, T& value, JsonStringBuffer& out) {
    CompactWriter  writer(out);
    ArchiveContext ctx(arena, 0);
    WriteArchive<CompactWriter> ar(writer, ctx);
    const bool ok = ar.Member("v", value);
    return ar.Finish() && ok;
  }

  // ---------------------------------------------------------------------------
  // T-29 範囲チェックの網羅マトリクス
  // ---------------------------------------------------------------------------

  /// `T` の最大値を 1 超える JSON リテラル
  template <class T>
  const char* OverflowLiteral(char* scratch, size_t capacity) {
    if constexpr (std::is_same_v<T, std::int64_t>) {
      return "9223372036854775808";           // INT64_MAX + 1 (uint64 として読まれる)
    }
    else if constexpr (std::is_same_v<T, std::uint64_t>) {
      return "18446744073709551616";          // UINT64_MAX + 1 (double へ落ちる)
    }
    else if constexpr (std::is_signed_v<T>) {
      std::snprintf(scratch, capacity, "%lld",
                    static_cast<long long>((std::numeric_limits<T>::max)()) + 1);
      return scratch;
    }
    else {
      std::snprintf(scratch, capacity, "%llu",
                    static_cast<unsigned long long>((std::numeric_limits<T>::max)()) + 1ull);
      return scratch;
    }
  }

  /// `T` の最小値を 1 下回る JSON リテラル
  template <class T>
  const char* UnderflowLiteral(char* scratch, size_t capacity) {
    if constexpr (std::is_same_v<T, std::int64_t>) {
      return "-99999999999999999999";         // 明確に範囲外 (double でも収まらない)
    }
    else if constexpr (!std::is_signed_v<T>) {
      return "-1";                            // 符号なしの最小値 0 の 1 つ下
    }
    else {
      std::snprintf(scratch, capacity, "%lld",
                    static_cast<long long>((std::numeric_limits<T>::min)()) - 1);
      return scratch;
    }
  }

  /**
   * @brief 1 つの整数幅について T-29 の全条件を確認する
   *
   * @details
   *  範囲チェックは 2 段構えである。どちらが欠けてもこの表のどこかが落ちる:
   *   1段目 `Value::TryGet{Int64,UInt64}` — 符号をまたぐ表現可能性と Double -> 整数
   *   2段目 `Detail::Narrow{Signed,Unsigned}` — 幅の範囲
   */
  template <class T>
  void CheckWidth(const char* name) {
    MockMemoryResource mock;

    constexpr T kMax = (std::numeric_limits<T>::max)();
    constexpr T kMin = (std::numeric_limits<T>::min)();
    constexpr T kSentinel = static_cast<T>(42);

    char scratch[64];

    // --- 最大値が読めること ---
    {
      char literal[64];
      if constexpr (std::is_signed_v<T>) {
        std::snprintf(literal, sizeof(literal), "%lld", static_cast<long long>(kMax));
      }
      else {
        std::snprintf(literal, sizeof(literal), "%llu", static_cast<unsigned long long>(kMax));
      }
      T out = kSentinel;
      const auto result = ReadValue(mock, literal, out);
      if (!result.ok || out != kMax || result.issueCount != 0) {
        std::printf("    [%s] max failed (literal=%s)\n", name, literal);
      }
      CHECK(result.ok);
      CHECK(out == kMax);
      CHECK(result.issueCount == 0);
    }

    // --- 最小値が読めること ---
    {
      char literal[64];
      if constexpr (std::is_signed_v<T>) {
        std::snprintf(literal, sizeof(literal), "%lld", static_cast<long long>(kMin));
      }
      else {
        std::snprintf(literal, sizeof(literal), "%llu", static_cast<unsigned long long>(kMin));
      }
      T out = kSentinel;
      const auto result = ReadValue(mock, literal, out);
      if (!result.ok || out != kMin) {
        std::printf("    [%s] min failed (literal=%s)\n", name, literal);
      }
      CHECK(result.ok);
      CHECK(out == kMin);
    }

    // --- 最大値 + 1 は RangeOverflow。out は不変 ---
    {
      const char* const literal = OverflowLiteral<T>(scratch, sizeof(scratch));
      T out = kSentinel;
      const auto result = ReadValue(mock, literal, out);
      if (result.ok || out != kSentinel
          || result.firstKind != ArchiveErrorKind::RangeOverflow) {
        std::printf("    [%s] overflow not detected (literal=%s)\n", name, literal);
      }
      CHECK(!result.ok);
      CHECK(out == kSentinel);
      CHECK(result.issueCount == 1);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
      CHECK(!result.fatal);          // RangeOverflow は Fatal ではない
    }

    // --- 最小値 - 1 は RangeOverflow。out は不変 ---
    {
      const char* const literal = UnderflowLiteral<T>(scratch, sizeof(scratch));
      T out = kSentinel;
      const auto result = ReadValue(mock, literal, out);
      if (result.ok || out != kSentinel
          || result.firstKind != ArchiveErrorKind::RangeOverflow) {
        std::printf("    [%s] underflow not detected (literal=%s)\n", name, literal);
      }
      CHECK(!result.ok);
      CHECK(out == kSentinel);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
    }

    // --- 負値を符号なし型へ ---
    if constexpr (!std::is_signed_v<T>) {
      T out = kSentinel;
      const auto result = ReadValue(mock, "-7", out);
      CHECK(!result.ok);
      CHECK(out == kSentinel);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
    }

    // --- UInt64 の INT64_MAX 超を符号付き型へ ---
    if constexpr (std::is_signed_v<T>) {
      T out = kSentinel;
      const auto result = ReadValue(mock, "18446744073709551615", out);
      CHECK(!result.ok);
      CHECK(out == kSentinel);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
    }

    // --- 小数部を持つ Double を整数型へ (R2-5a) ---
    {
      T out = kSentinel;
      const auto result = ReadValue(mock, "1.5", out);
      CHECK(!result.ok);
      CHECK(out == kSentinel);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
    }

    // --- 数値ですらない場合は TypeMismatch (RangeOverflow と切り分かれること) ---
    {
      T out = kSentinel;
      const auto result = ReadValue(mock, "\"x\"", out);
      CHECK(!result.ok);
      CHECK(out == kSentinel);
      CHECK(result.firstKind == ArchiveErrorKind::TypeMismatch);
    }

    // --- 往復 ---
    {
      MockMemoryResource roundTripMock;
      JsonArena          arena(&roundTripMock);
      JsonStringBuffer   buffer(arena);

      T source = kMax;
      CHECK(WriteValue(arena, source, buffer));

      Document doc(&roundTripMock);
      CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());

      JsonArena      readArena(&roundTripMock);
      ArchiveContext ctx(readArena);
      T loaded = kSentinel;
      {
        ReadArchive ar(doc.Root(), ctx);
        CHECK(ar.Member("v", loaded));
      }
      CHECK(loaded == kMax);
      CHECK(!ctx.HasIssues());
    }
  }

  void TestIntegerMatrix() {
    GLFD::Test::BeginCase("T-29: range-check matrix over all eight integer widths");

    CheckWidth<std::int8_t>("int8");
    CheckWidth<std::int16_t>("int16");
    CheckWidth<std::int32_t>("int32");
    CheckWidth<std::int64_t>("int64");
    CheckWidth<std::uint8_t>("uint8");
    CheckWidth<std::uint16_t>("uint16");
    CheckWidth<std::uint32_t>("uint32");
    CheckWidth<std::uint64_t>("uint64");
  }

  // ---------------------------------------------------------------------------
  // T-29 補足: bool / float / double / StringView
  // ---------------------------------------------------------------------------

  /**
   * @brief int64 の境界のすぐ外側は double へ丸め戻る、という挙動を固定する
   *
   * @details
   *  `JsonReader` は int64 / uint64 に収まらない整数リテラルを `Double` へ落とす
   *  (R1-7)。`INT64_MIN - 1` は double では `INT64_MIN` ちょうどに丸まるため、
   *  **範囲外として弾かれず INT64_MIN として読めてしまう**。
   *  欠陥ではなく JSON に整数幅の概念が無いことの帰結だが、T-29 の表で
   *  「最小値 - 1 は必ず失敗する」と読まれると誤解になるのでここで固定する。
   */
  void TestInt64RoundingBoundary() {
    GLFD::Test::BeginCase("int64 boundary: INT64_MIN-1 rounds back to INT64_MIN via double");

    MockMemoryResource mock;

    {
      std::int64_t out = 42;
      const auto result = ReadValue(mock, "-9223372036854775809", out);
      CHECK(result.ok);
      CHECK(out == (std::numeric_limits<std::int64_t>::min)());
      CHECK(result.issueCount == 0);
    }

    // 一方 INT64_MAX + 1 は uint64 として読めるため、符号付きへは入らない
    {
      std::int64_t out = 42;
      const auto result = ReadValue(mock, "9223372036854775808", out);
      CHECK(!result.ok);
      CHECK(out == 42);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
    }

    // UINT64_MAX + 1 は double でも収まらないので弾かれる
    {
      std::uint64_t out = 42;
      const auto result = ReadValue(mock, "18446744073709551616", out);
      CHECK(!result.ok);
      CHECK(out == 42u);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
    }
  }

  void TestOtherScalars() {
    GLFD::Test::BeginCase("scalars other than integers keep the same failure rules");

    MockMemoryResource mock;

    // bool は厳格。0 / 1 を読み替えない
    {
      bool out = true;
      const auto result = ReadValue(mock, "0", out);
      CHECK(!result.ok);
      CHECK(out == true);
      CHECK(result.firstKind == ArchiveErrorKind::TypeMismatch);
    }
    {
      bool out = false;
      const auto result = ReadValue(mock, "true", out);
      CHECK(result.ok);
      CHECK(out == true);
    }

    // float は丸めを許容する (R2-5a)
    {
      float out = 0.0f;
      const auto result = ReadValue(mock, "0.1", out);
      CHECK(result.ok);
      CHECK(out == 0.1f);
    }
    // float の範囲外は RangeOverflow
    {
      float out = 1.0f;
      const auto result = ReadValue(mock, "1e300", out);
      CHECK(!result.ok);
      CHECK(out == 1.0f);
      CHECK(result.firstKind == ArchiveErrorKind::RangeOverflow);
    }
    // double は整数からも読める(往復一致する場合のみ)
    {
      double out = 0.0;
      const auto result = ReadValue(mock, "5", out);
      CHECK(result.ok);
      CHECK(out == 5.0);
    }
    // 文字列は **ArchiveContext のアリーナへ複製される** (§5.4)。
    // したがって読み取った StringView はコンテキストと同じ寿命を持つ。
    // ReadValue() はアリーナをローカルに作って捨てるため、ここでは使えない
    // (使うと解放済み領域を指す。この寿命こそが「アリーナへコピーする」設計の意味)
    {
      JsonArena arena(&mock);

      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"v\":\"abc\"}"), ParseFlags::None).IsOk());

      ArchiveContext ctx(arena);
      StringView     out;
      {
        ReadArchive ar(doc.Root(), ctx);
        CHECK(ar.Member("v", out));
      }
      CHECK(ViewEquals(out, "abc"));

      // DOM を捨てても内容が残る(複製されている証拠)
      doc.Clear();
      CHECK(ViewEquals(out, "abc"));
    }
    {
      JsonArena arena(&mock);

      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"v\":123}"), ParseFlags::None).IsOk());

      ArchiveContext ctx(arena);
      StringView     out("keep");
      {
        ReadArchive ar(doc.Root(), ctx);
        CHECK(!ar.Member("v", out));
      }
      CHECK(ViewEquals(out, "keep"));      // 失敗時は out 不変
      CHECK(ctx.IssueCount() == 1);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
    }
  }

}

// ---------------------------------------------------------------------------
// T-31 enum
// ---------------------------------------------------------------------------

namespace EnumTest {

  /// 値が暗黙 (0,1,2)。ヘッダの @warning が指す「危険」な形
  enum class LogLevel { Info, Warning, Error };

  /// 基底型と値を明示した形。セーブデータに乗せるならこちらにすべき
  enum class Element : std::int16_t {
    None  = 0,
    Fire  = 100,
    Water = 200,
    Wind  = 300,
  };

  /// 基底型を明示した非スコープ列挙(既存の KeyCode と同じ形)
  enum Slot : std::uint8_t {
    SlotHead = 1,
    SlotBody = 2,
    SlotFoot = 3,
  };

  struct Config {
    LogLevel level   = LogLevel::Info;
    Element  element = Element::None;
    Slot     slot    = SlotHead;
  };

  template <class Ar>
  void Serialize(Ar& ar, Config& v) {
    (void)ar.Member("level", v.level, LogLevel::Info);
    (void)ar.Member("element", v.element, Element::None);
    (void)ar.Member("slot", v.slot, SlotHead);
  }

  /// 文字列表現の opt-in (R3-2)。**完全特殊化は部分特殊化より優先される**
  enum class Difficulty : std::int32_t { Easy = 0, Normal = 1, Hard = 2 };

}

namespace GLFD::Json {

  /**
   * @brief `Difficulty` を文字列として保存する例(T-31 / R3-2)
   *
   * @details
   *  この完全特殊化があるだけで、既定の「基底整数型として保存する」部分特殊化を
   *  上書きできる。アーカイブ機構には一切手を入れていない。
   *  手書き config に人間が書く列挙は、この形にすると値の意味が読んで分かる。
   */
  template <>
  struct JsonSerializer<EnumTest::Difficulty> {
    template <class Ar>
    static bool Serialize(Ar& ar, EnumTest::Difficulty& value) {
      if constexpr (Ar::IsReading()) {
        StringView text;
        if (!Detail::ArchiveOne(ar, text)) {
          return false;
        }
        if      (text == StringView("easy"))   { value = EnumTest::Difficulty::Easy; }
        else if (text == StringView("normal")) { value = EnumTest::Difficulty::Normal; }
        else if (text == StringView("hard"))   { value = EnumTest::Difficulty::Hard; }
        else {
          // 現在のパスが自動で添えられる。手書き config の綴り間違いを黙って
          // 落とさないことが R3-9 の目的なので、必ず記録する
          ar.Context().Report(ArchiveErrorKind::TypeMismatch,
                              StringView("unknown difficulty (expected easy/normal/hard)"));
          return false;
        }
        return true;
      }
      else {
        StringView text("normal");
        switch (value) {
          case EnumTest::Difficulty::Easy:   text = StringView("easy");   break;
          case EnumTest::Difficulty::Normal: text = StringView("normal"); break;
          case EnumTest::Difficulty::Hard:   text = StringView("hard");   break;
        }
        return Detail::ArchiveOne(ar, text);
      }
    }
  };

}

namespace {

  void TestEnums() {
    GLFD::Test::BeginCase("T-31: enums round-trip through their underlying type");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // 書き出しは基底整数型
    {
      JsonStringBuffer buffer(arena);
      CompactWriter    writer(buffer);
      ArchiveContext   ctx(arena, 1);
      EnumTest::Config config;
      config.level   = EnumTest::LogLevel::Error;     // 2
      config.element = EnumTest::Element::Water;      // 200
      config.slot    = EnumTest::SlotFoot;            // 3
      {
        WriteArchive<CompactWriter> ar(writer, ctx);
        EnumTest::Serialize(ar, config);
        CHECK(ar.Finish());
      }
      CHECK(ViewEquals(buffer.View(),
                       "{\"$version\":1,\"level\":2,\"element\":200,\"slot\":3}"));
    }

    // 往復
    {
      JsonStringBuffer buffer(arena);
      CompactWriter    writer(buffer);
      ArchiveContext   ctx(arena, 1);
      EnumTest::Config source;
      source.level   = EnumTest::LogLevel::Warning;
      source.element = EnumTest::Element::Wind;
      source.slot    = EnumTest::SlotBody;
      {
        WriteArchive<CompactWriter> ar(writer, ctx);
        EnumTest::Serialize(ar, source);
        CHECK(ar.Finish());
      }

      Document doc(&mock);
      CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());
      ArchiveContext   readCtx(arena);
      EnumTest::Config loaded;
      {
        ReadArchive ar(doc.Root(), readCtx);
        EnumTest::Serialize(ar, loaded);
      }
      CHECK(!readCtx.HasIssues());
      CHECK(loaded.level == EnumTest::LogLevel::Warning);
      CHECK(loaded.element == EnumTest::Element::Wind);
      CHECK(loaded.slot == EnumTest::SlotBody);
    }

    // 基底型の範囲を超えた値は RangeOverflow (基底型の規則がそのまま効く)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"element\":99999,\"slot\":300}"),
                      ParseFlags::None).IsOk());
      ArchiveContext   ctx(arena);
      EnumTest::Config loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        EnumTest::Serialize(ar, loaded);
      }
      // Element は int16 (最大 32767)、Slot は uint8 (最大 255)
      CHECK(ctx.IssueCount() == 2);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::RangeOverflow);
      CHECK(ctx.Issues()[1].kind == ArchiveErrorKind::RangeOverflow);
      CHECK(loaded.element == EnumTest::Element::None);   // out は不変
      CHECK(loaded.slot == EnumTest::SlotHead);
    }

    // 基底型に収まるが、定義されていない列挙子の値は **読めてしまう**。
    // これは許容する挙動であり、将来の判断材料としてここで固定しておく。
    // (基底型を明示した列挙なので static_cast は未定義動作にならない)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"element\":12345,\"slot\":250,\"level\":7}"),
                      ParseFlags::None).IsOk());
      ArchiveContext   ctx(arena);
      EnumTest::Config loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        EnumTest::Serialize(ar, loaded);
      }
      CHECK(!ctx.HasIssues());
      CHECK(static_cast<std::int16_t>(loaded.element) == 12345);
      CHECK(static_cast<std::uint8_t>(loaded.slot) == 250);
      CHECK(static_cast<int>(loaded.level) == 7);
    }
  }

  void TestEnumAsString() {
    GLFD::Test::BeginCase("T-31: JsonSerializer<E> specialization stores an enum as text");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // 書き
    {
      JsonStringBuffer buffer(arena);
      CompactWriter    writer(buffer);
      ArchiveContext   ctx(arena, 1);
      EnumTest::Difficulty value = EnumTest::Difficulty::Hard;
      {
        WriteArchive<CompactWriter> ar(writer, ctx);
        CHECK(ar.Member("difficulty", value));
        CHECK(ar.Finish());
      }
      CHECK(ViewEquals(buffer.View(), "{\"$version\":1,\"difficulty\":\"hard\"}"));
    }

    // 読み
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"difficulty\":\"easy\"}"), ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      EnumTest::Difficulty value = EnumTest::Difficulty::Normal;
      {
        ReadArchive ar(doc.Root(), ctx);
        CHECK(ar.Member("difficulty", value));
      }
      CHECK(value == EnumTest::Difficulty::Easy);
      CHECK(!ctx.HasIssues());
    }

    // 未知の文字列
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"difficulty\":\"lunatic\"}"), ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      EnumTest::Difficulty value = EnumTest::Difficulty::Normal;
      {
        ReadArchive ar(doc.Root(), ctx);
        CHECK(!ar.Member("difficulty", value));
      }
      CHECK(value == EnumTest::Difficulty::Normal);   // out は不変
      CHECK(ctx.IssueCount() == 1);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
      CHECK(ViewEquals(ctx.Issues()[0].path, "difficulty"));
    }
  }

  // ---------------------------------------------------------------------------
  // 静的検証
  // ---------------------------------------------------------------------------

  void TestStaticProperties() {
    GLFD::Test::BeginCase("static properties of the scalar / enum dispatch");

    namespace D = GLFD::Json::Detail;

    // 8 種の整数幅すべてがスカラとして関門を通る
    static_assert(D::IsArchiveScalar<std::int8_t>);
    static_assert(D::IsArchiveScalar<std::int16_t>);
    static_assert(D::IsArchiveScalar<std::int32_t>);
    static_assert(D::IsArchiveScalar<std::int64_t>);
    static_assert(D::IsArchiveScalar<std::uint8_t>);
    static_assert(D::IsArchiveScalar<std::uint16_t>);
    static_assert(D::IsArchiveScalar<std::uint32_t>);
    static_assert(D::IsArchiveScalar<std::uint64_t>);
    static_assert(D::IsArchiveScalar<bool>);
    static_assert(D::IsArchiveScalar<float>);
    static_assert(D::IsArchiveScalar<double>);
    static_assert(D::IsArchiveScalar<StringView>);

    // enum はスカラの関門を通らず、JsonSerializer の部分特殊化で拾われる
    static_assert(!D::IsArchiveScalar<EnumTest::LogLevel>);
    static_assert(D::HasCustomSerializer<ReadArchive, EnumTest::LogLevel>);
    static_assert(D::HasCustomSerializer<ReadArchive, EnumTest::Slot>);
    static_assert(D::Archivable<ReadArchive, EnumTest::LogLevel>);

    // 完全特殊化は部分特殊化より優先される
    static_assert(D::HasCustomSerializer<ReadArchive, EnumTest::Difficulty>);

    // char は意図的に対象外 (JsonArchiveTypes.h の @note)
    static_assert(!D::IsArchiveScalar<char>);
    static_assert(!D::Archivable<ReadArchive, char>);

    ++GLFD::Test::g_checkCount;   // 上の static_assert 群を 1 件として数える
    CHECK(true);
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonArchiveTypes (1-6)");

  TestIntegerMatrix();
  TestInt64RoundingBoundary();
  TestOtherScalars();
  TestEnums();
  TestEnumAsString();
  TestStaticProperties();

  return GLFD::Test::Summarize();
}
