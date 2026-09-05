/**
 * @file  JsonReaderTests.cpp
 * @brief JsonReader の単体テスト(フェーズ 1-2b / 要件 T-1, T-2, T-6, T-6a, T-7)
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>

#include "TestHarness.h"
#include "MockMemoryResource.h"
#include "JsonTraceHandler.h"
#include "Core/Json/JsonReader.h"

using GLFD::StringView;
using GLFD::Json::ErrorCode;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonReader;
using GLFD::Json::ParseError;
using GLFD::Json::ParseFlags;
using GLFD::Json::ToString;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::JsonTraceHandler;
using GLFD::Test::MockMemoryResource;
using GLFD::Test::Summarize;

namespace {

  constexpr ParseFlags kNone    = ParseFlags::None;
  constexpr ParseFlags kJsonC   = ParseFlags::JsonC;
  constexpr ParseFlags kComment = ParseFlags::AllowComments;
  constexpr ParseFlags kComma   = ParseFlags::AllowTrailingComma;
  constexpr ParseFlags kNaNInf  = ParseFlags::AllowNaNInf;

  // ---------------------------------------------------------------------------
  // 失敗レポート(期待値と実測値を両方出す)
  // ---------------------------------------------------------------------------

  void Fail(int line, const char* message) {
    std::printf("    [FAIL] %s(%d): %s\n", __FILE__, line, message);
    ++GLFD::Test::g_failureCount;
  }

  void FailDetail(int line, const char* what, const char* expected, const char* actual,
                  StringView json) {
    std::printf("    [FAIL] %s(%d): %s\n"
                "             expected: %s\n"
                "             actual  : %s\n"
                "             input   : \"%.*s\"\n",
                __FILE__, line, what, expected, actual,
                static_cast<int>(json.Size() < 120 ? json.Size() : 120), json.Data());
    ++GLFD::Test::g_failureCount;
  }

  /// エラー内容を "Code@offset" の形に整える
  const char* FormatError(const ParseError& error, char* buffer, size_t capacity) {
    const StringView name = ToString(error.code);
    std::snprintf(buffer, capacity, "%.*s@%zu",
                  static_cast<int>(name.Size()), name.Data(), error.offset);
    return buffer;
  }

  // ---------------------------------------------------------------------------
  // 検証ヘルパ
  // ---------------------------------------------------------------------------

  /// パースが成功し、SAX イベント列が expected と厳密に一致すること (T-7)
  void ExpectTraceImpl(int line, StringView json, StringView expected, ParseFlags flags) {
    ++GLFD::Test::g_checkCount;

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;

    const ParseError error = reader.Parse(json, handler, flags);

    char scratch[128];
    if (!error.IsOk()) {
      FailDetail(line, "expected success", "<ok>", FormatError(error, scratch, sizeof(scratch)), json);
      return;
    }
    if (handler.Overflowed()) {
      Fail(line, "trace buffer overflowed");
      return;
    }
    if (handler.Trace() != expected) {
      char actual[512];
      const StringView trace = handler.Trace();
      std::snprintf(actual, sizeof(actual), "%.*s",
                    static_cast<int>(trace.Size() < 480 ? trace.Size() : 480), trace.Data());
      char want[512];
      std::snprintf(want, sizeof(want), "%.*s",
                    static_cast<int>(expected.Size() < 480 ? expected.Size() : 480), expected.Data());
      FailDetail(line, "trace mismatch", want, actual, json);
    }
  }

  /// パースが指定のコードとオフセットで失敗すること
  void ExpectErrorImpl(int line, StringView json, ErrorCode code, size_t offset, ParseFlags flags) {
    ++GLFD::Test::g_checkCount;

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;

    const ParseError error = reader.Parse(json, handler, flags);

    char actual[128];
    char want[128];
    const ParseError expected{ code, offset };
    if (error.code != code || error.offset != offset) {
      FailDetail(line, "error mismatch",
                 FormatError(expected, want, sizeof(want)),
                 FormatError(error, actual, sizeof(actual)), json);
      return;
    }
    // offset は必ず入力の範囲内(終端を含む)を指すこと
    if (error.offset > json.Size()) {
      Fail(line, "error offset is out of range");
    }
  }

  #define EXPECT_TRACE(json, trace, flags) ExpectTraceImpl(__LINE__, (json), (trace), (flags))
  #define EXPECT_ERROR(json, code, off, flags) ExpectErrorImpl(__LINE__, (json), (code), (off), (flags))

  // ---------------------------------------------------------------------------
  // 数値検証用のハンドラ(トレース文字列の書式に依存せず値そのものを見る)
  // ---------------------------------------------------------------------------

  enum class Scalar : std::uint8_t { None, Null, Bool, Int64, UInt64, Double, String };

  class NumberProbe final {
  public:
    bool OnNull()                 { return Record(Scalar::Null); }
    bool OnBool(bool v)           { m_bool = v;   return Record(Scalar::Bool); }
    bool OnInt64(std::int64_t v)  { m_int = v;    return Record(Scalar::Int64); }
    bool OnUInt64(std::uint64_t v){ m_uint = v;   return Record(Scalar::UInt64); }
    bool OnDouble(double v)       { m_double = v; return Record(Scalar::Double); }

    bool OnString(StringView s, bool needsCopy) {
      m_string = s;
      m_needsCopy = needsCopy;
      return Record(Scalar::String);
    }

    bool OnKey(StringView, bool)      { return true; }
    bool OnObjectBegin()              { return true; }
    bool OnObjectEnd(std::uint32_t)   { return true; }
    bool OnArrayBegin()               { return true; }
    bool OnArrayEnd(std::uint32_t)    { return true; }

    [[nodiscard]] Scalar        Kind()      const noexcept { return m_kind; }
    [[nodiscard]] bool          Bool()      const noexcept { return m_bool; }
    [[nodiscard]] std::int64_t  Int()       const noexcept { return m_int; }
    [[nodiscard]] std::uint64_t UInt()      const noexcept { return m_uint; }
    [[nodiscard]] double        Double()    const noexcept { return m_double; }
    [[nodiscard]] StringView    String()    const noexcept { return m_string; }
    [[nodiscard]] bool          NeedsCopy() const noexcept { return m_needsCopy; }

  private:
    bool Record(Scalar kind) { m_kind = kind; return true; }

    Scalar        m_kind      = Scalar::None;
    bool          m_bool      = false;
    std::int64_t  m_int       = 0;
    std::uint64_t m_uint      = 0;
    double        m_double    = 0.0;
    StringView    m_string    = {};
    bool          m_needsCopy = false;
  };

  /// double を bit 単位で比較する(-0.0 と 0.0 を区別するため)
  [[nodiscard]] bool SameBits(double a, double b) {
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
  }

  [[nodiscard]] bool IsNegative(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return (bits >> 63) != 0;
  }

  /// スカラをひとつパースして probe へ記録する
  ParseError ProbeScalar(StringView json, NumberProbe& probe, ParseFlags flags) {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    return reader.Parse(json, probe, flags);
  }

  void ExpectInt64Impl(int line, StringView json, std::int64_t expected) {
    ++GLFD::Test::g_checkCount;
    NumberProbe probe;
    const ParseError error = ProbeScalar(json, probe, kNone);

    char scratch[128];
    if (!error.IsOk()) {
      FailDetail(line, "expected Int64", "<ok>", FormatError(error, scratch, sizeof(scratch)), json);
      return;
    }
    if (probe.Kind() != Scalar::Int64) {
      FailDetail(line, "expected Int64", "Int64", "<other type>", json);
      return;
    }
    if (probe.Int() != expected) {
      char want[64];
      char got[64];
      std::snprintf(want, sizeof(want), "%lld", static_cast<long long>(expected));
      std::snprintf(got, sizeof(got), "%lld", static_cast<long long>(probe.Int()));
      FailDetail(line, "Int64 value mismatch", want, got, json);
    }
  }

  void ExpectUInt64Impl(int line, StringView json, std::uint64_t expected) {
    ++GLFD::Test::g_checkCount;
    NumberProbe probe;
    const ParseError error = ProbeScalar(json, probe, kNone);

    char scratch[128];
    if (!error.IsOk()) {
      FailDetail(line, "expected UInt64", "<ok>", FormatError(error, scratch, sizeof(scratch)), json);
      return;
    }
    if (probe.Kind() != Scalar::UInt64) {
      FailDetail(line, "expected UInt64", "UInt64", "<other type>", json);
      return;
    }
    if (probe.UInt() != expected) {
      char want[64];
      char got[64];
      std::snprintf(want, sizeof(want), "%llu", static_cast<unsigned long long>(expected));
      std::snprintf(got, sizeof(got), "%llu", static_cast<unsigned long long>(probe.UInt()));
      FailDetail(line, "UInt64 value mismatch", want, got, json);
    }
  }

  void ExpectDoubleImpl(int line, StringView json, double expected, bool compareBits,
                        ParseFlags flags) {
    ++GLFD::Test::g_checkCount;
    NumberProbe probe;
    const ParseError error = ProbeScalar(json, probe, flags);

    char scratch[128];
    if (!error.IsOk()) {
      FailDetail(line, "expected Double", "<ok>", FormatError(error, scratch, sizeof(scratch)), json);
      return;
    }
    if (probe.Kind() != Scalar::Double) {
      FailDetail(line, "expected Double", "Double", "<other type>", json);
      return;
    }

    const bool matched = compareBits ? SameBits(probe.Double(), expected)
                                     : (probe.Double() == expected);
    if (!matched) {
      char want[64];
      char got[64];
      std::snprintf(want, sizeof(want), "%.17g%s", expected, IsNegative(expected) ? " (sign-)" : "");
      std::snprintf(got, sizeof(got), "%.17g%s", probe.Double(),
                    IsNegative(probe.Double()) ? " (sign-)" : "");
      FailDetail(line, "Double value mismatch", want, got, json);
    }
  }

  #define EXPECT_INT64(json, value)  ExpectInt64Impl(__LINE__, (json), (value))
  #define EXPECT_UINT64(json, value) ExpectUInt64Impl(__LINE__, (json), (value))
  #define EXPECT_DOUBLE(json, value) ExpectDoubleImpl(__LINE__, (json), (value), false, kNone)
  #define EXPECT_DOUBLE_BITS(json, value) ExpectDoubleImpl(__LINE__, (json), (value), true, kNone)

}

// ---------------------------------------------------------------------------
// 1. 構造と SAX イベント列 (T-7)
// ---------------------------------------------------------------------------

static void Test_Structure() {
  BeginCase("structure: SAX event traces match exactly");

  // ルート単独スカラ (R1-3)
  EXPECT_TRACE("null",  "n",      kNone);
  EXPECT_TRACE("true",  "t",      kNone);
  EXPECT_TRACE("false", "f",      kNone);
  EXPECT_TRACE("123",   "i(123)", kNone);
  EXPECT_TRACE("\"s\"", "s(s)",   kNone);

  // 空のコンテナ
  EXPECT_TRACE("{}",   "{}0", kNone);
  EXPECT_TRACE("[]",   "[]0", kNone);
  EXPECT_TRACE("[[]]", "[[]0]1", kNone);
  EXPECT_TRACE("{\"a\":{}}", "{k(a){}0}1", kNone);

  // 基本
  EXPECT_TRACE("[1,2,3]", "[i(1)i(2)i(3)]3", kNone);
  EXPECT_TRACE("{\"a\":1}", "{k(a)i(1)}1", kNone);
  EXPECT_TRACE("{\"a\":1,\"b\":2}", "{k(a)i(1)k(b)i(2)}2", kNone);

  // ネストと要素数
  EXPECT_TRACE("{\"a\":[1,2],\"b\":{\"c\":null}}",
               "{k(a)[i(1)i(2)]2k(b){k(c)n}1}2", kNone);
  EXPECT_TRACE("[[1],[2,3],[]]", "[[i(1)]1[i(2)i(3)]2[]0]3", kNone);

  // 空白の許容
  EXPECT_TRACE("  [ 1 , 2 ]  ", "[i(1)i(2)]2", kNone);
  EXPECT_TRACE("\t{\n\"a\"\r:\r\n1\n}\n", "{k(a)i(1)}1", kNone);

  // キーの重複はパース順にすべて通す (R2-3 の前提)
  EXPECT_TRACE("{\"a\":1,\"a\":2}", "{k(a)i(1)k(a)i(2)}2", kNone);

  // 深いネスト(インライン領域の内側)
  EXPECT_TRACE("[[[[[1]]]]]", "[[[[[i(1)]1]1]1]1]1", kNone);
}

static void Test_StructureErrors() {
  BeginCase("structure errors: each case yields the expected code and offset");

  EXPECT_ERROR("",      ErrorCode::DocumentEmpty, 0, kNone);
  EXPECT_ERROR("   ",   ErrorCode::DocumentEmpty, 3, kNone);
  EXPECT_ERROR("\n\t ", ErrorCode::DocumentEmpty, 3, kNone);

  // ルート値の後ろのゴミ
  EXPECT_ERROR("{} x",   ErrorCode::DocumentRootNotSingular, 3, kNone);
  EXPECT_ERROR("1 2",    ErrorCode::DocumentRootNotSingular, 2, kNone);
  EXPECT_ERROR("[] []",  ErrorCode::DocumentRootNotSingular, 3, kNone);
  EXPECT_ERROR("null,",  ErrorCode::DocumentRootNotSingular, 4, kNone);

  // 値として解釈できない
  EXPECT_ERROR("x",     ErrorCode::ValueInvalid, 0, kNone);
  EXPECT_ERROR("'a'",   ErrorCode::ValueInvalid, 0, kNone);
  EXPECT_ERROR("[,]",   ErrorCode::ValueInvalid, 1, kNone);
  EXPECT_ERROR("tru",   ErrorCode::ValueInvalid, 0, kNone);
  EXPECT_ERROR("nul",   ErrorCode::ValueInvalid, 0, kNone);
  EXPECT_ERROR("[1,",   ErrorCode::ValueInvalid, 3, kNone);

  // オブジェクト
  EXPECT_ERROR("{",           ErrorCode::ObjectMissingName, 1, kNone);
  EXPECT_ERROR("{1:2}",       ErrorCode::ObjectMissingName, 1, kNone);
  EXPECT_ERROR("{a:1}",       ErrorCode::ObjectMissingName, 1, kNone);
  EXPECT_ERROR("{\"a\"}",     ErrorCode::ObjectMissingColon, 4, kNone);
  EXPECT_ERROR("{\"a\" 1}",   ErrorCode::ObjectMissingColon, 5, kNone);
  EXPECT_ERROR("{\"a\"",      ErrorCode::ObjectMissingColon, 4, kNone);
  EXPECT_ERROR("{\"a\":1 2}", ErrorCode::ObjectMissingCommaOrBrace, 7, kNone);
  EXPECT_ERROR("{\"a\":1",    ErrorCode::ObjectMissingCommaOrBrace, 6, kNone);
  EXPECT_ERROR("{\"a\":1]",   ErrorCode::ObjectMissingCommaOrBrace, 6, kNone);

  // 配列
  EXPECT_ERROR("[1 2]", ErrorCode::ArrayMissingCommaOrBracket, 3, kNone);
  EXPECT_ERROR("[1",    ErrorCode::ArrayMissingCommaOrBracket, 2, kNone);
  EXPECT_ERROR("[1}",   ErrorCode::ArrayMissingCommaOrBracket, 2, kNone);
  EXPECT_ERROR("[[1]",  ErrorCode::ArrayMissingCommaOrBracket, 4, kNone);
}

// ---------------------------------------------------------------------------
// 2. 数値 (T-1 / R1-7 / R1-16 / R1-17 / R1-19)
// ---------------------------------------------------------------------------

static void Test_NumberMapping() {
  BeginCase("numbers: Int64 / UInt64 / Double mapping and boundaries");

  EXPECT_INT64("0", 0);
  EXPECT_INT64("1", 1);
  EXPECT_INT64("-1", -1);
  EXPECT_INT64("1234567890", 1234567890);

  // int64 の境界
  EXPECT_INT64("9223372036854775807", (std::numeric_limits<std::int64_t>::max)());
  EXPECT_INT64("-9223372036854775808", (std::numeric_limits<std::int64_t>::min)());

  // int64 を超えたら UInt64 へ (R1-7 規則 2)
  EXPECT_UINT64("9223372036854775808", 9223372036854775808ull);
  EXPECT_UINT64("18446744073709551615", (std::numeric_limits<std::uint64_t>::max)());

  // uint64 も超えたら Double へ (R1-7 規則 3)
  EXPECT_DOUBLE("18446744073709551616", 18446744073709551616.0);
  EXPECT_DOUBLE("-9223372036854775809", -9223372036854775809.0);

  // 小数点 / 指数があれば必ず Double
  EXPECT_DOUBLE("1.0", 1.0);
  EXPECT_DOUBLE("1.0e5", 100000.0);
  EXPECT_DOUBLE("1E5", 100000.0);
  EXPECT_DOUBLE("1e-5", 1e-5);
  EXPECT_DOUBLE("-1.5e+3", -1500.0);
  EXPECT_DOUBLE("0.5", 0.5);
  EXPECT_DOUBLE("1e308", 1e308);

  // R1-19: "-0" と "-0.0" で符号の扱いが異なる
  EXPECT_INT64("-0", 0);                    // 小数点・指数なし → Int64(0)。符号は失われる
  EXPECT_INT64("0", 0);
  EXPECT_DOUBLE_BITS("-0.0", -0.0);         // Double なら符号ビットが保たれる
  EXPECT_DOUBLE_BITS("0.0", 0.0);
  EXPECT_DOUBLE_BITS("-0e0", -0.0);

  {
    // "-0" が Int64 であって Double(-0.0) ではないことを型でも確認する
    NumberProbe probe;
    ++GLFD::Test::g_checkCount;
    const ParseError error = ProbeScalar(StringView("-0"), probe, kNone);
    if (!error.IsOk() || probe.Kind() != Scalar::Int64 || probe.Int() != 0) {
      Fail(__LINE__, "\"-0\" must be delivered as Int64(0)");
    }
  }
}

static void Test_NumberOverflow() {
  BeginCase("numbers: overflow yields NumberTooBig, underflow yields signed zero");

  // オーバーフロー (E10 > 0)
  EXPECT_ERROR("1e309",    ErrorCode::NumberTooBig, 0, kNone);
  EXPECT_ERROR("-1e309",   ErrorCode::NumberTooBig, 0, kNone);
  EXPECT_ERROR("1e999999", ErrorCode::NumberTooBig, 0, kNone);
  EXPECT_ERROR("1.8e308",  ErrorCode::NumberTooBig, 0, kNone);
  EXPECT_ERROR("[1e309]",  ErrorCode::NumberTooBig, 1, kNone);
  // 指数の桁を積み上げても E10 自体が壊れないこと (R1-17 の飽和)
  EXPECT_ERROR("1e99999999999999999999", ErrorCode::NumberTooBig, 0, kNone);

  // アンダーフロー (E10 <= 0) はエラーにせず符号付きゼロ
  EXPECT_DOUBLE_BITS("1e-400", 0.0);
  EXPECT_DOUBLE_BITS("-1e-400", -0.0);
  EXPECT_DOUBLE_BITS("0.0001e-400", 0.0);
  EXPECT_DOUBLE_BITS("-0.0001e-400", -0.0);
  EXPECT_DOUBLE_BITS("1e-99999999999999999999", 0.0);
  EXPECT_DOUBLE_BITS("-1e-99999999999999999999", -0.0);

  // 有効数字がすべてゼロのケースはガードされる
  EXPECT_DOUBLE_BITS("0.0e-400", 0.0);
  EXPECT_DOUBLE_BITS("-0.0e999999", -0.0);
  EXPECT_DOUBLE_BITS("0e999999", 0.0);

  // 表現域ぎりぎりは通ること
  EXPECT_DOUBLE("1.7976931348623157e308", 1.7976931348623157e308);
  EXPECT_DOUBLE("2.2250738585072014e-308", 2.2250738585072014e-308);
  EXPECT_DOUBLE("5e-324", 5e-324);
}

static void Test_NumberRejection() {
  BeginCase("numbers: strict token validation (from_chars would let these through)");

  // 先頭ゼロ
  EXPECT_ERROR("01",     ErrorCode::NumberInvalid, 1, kNone);
  EXPECT_ERROR("-01",    ErrorCode::NumberInvalid, 2, kNone);
  EXPECT_ERROR("[00]",   ErrorCode::NumberInvalid, 2, kNone);
  EXPECT_ERROR("0123",   ErrorCode::NumberInvalid, 1, kNone);

  // 整数部が無い
  EXPECT_ERROR(".5",  ErrorCode::ValueInvalid, 0, kNone);
  EXPECT_ERROR("-.5", ErrorCode::NumberInvalid, 1, kNone);

  // 小数点の後に数字が無い
  EXPECT_ERROR("1.",   ErrorCode::NumberMissingFraction, 2, kNone);
  EXPECT_ERROR("[1.]", ErrorCode::NumberMissingFraction, 3, kNone);
  EXPECT_ERROR("1.e5", ErrorCode::NumberMissingFraction, 2, kNone);

  // 明示的なプラス符号
  EXPECT_ERROR("+1", ErrorCode::ValueInvalid, 0, kNone);

  // 指数部に数字が無い
  EXPECT_ERROR("1e",   ErrorCode::NumberMissingExponent, 2, kNone);
  EXPECT_ERROR("1e+",  ErrorCode::NumberMissingExponent, 3, kNone);
  EXPECT_ERROR("1e-",  ErrorCode::NumberMissingExponent, 3, kNone);
  EXPECT_ERROR("1E+x", ErrorCode::NumberMissingExponent, 3, kNone);

  // 数値の直後の余分な英数字 (R1-16)
  EXPECT_ERROR("0x10",   ErrorCode::NumberInvalid, 1, kNone);
  EXPECT_ERROR("1.2.3",  ErrorCode::NumberInvalid, 3, kNone);
  EXPECT_ERROR("1e5e5",  ErrorCode::NumberInvalid, 3, kNone);
  EXPECT_ERROR("123abc", ErrorCode::NumberInvalid, 3, kNone);
  EXPECT_ERROR("[0x1]",  ErrorCode::NumberInvalid, 2, kNone);
  EXPECT_ERROR("1_000",  ErrorCode::NumberInvalid, 1, kNone);

  // 符号だけ
  EXPECT_ERROR("-",  ErrorCode::NumberInvalid, 1, kNone);
  EXPECT_ERROR("-x", ErrorCode::NumberInvalid, 1, kNone);
}

// ---------------------------------------------------------------------------
// 3. 文字列 (T-1 / R1-5 / R1-6)
// ---------------------------------------------------------------------------

static void Test_Strings() {
  BeginCase("strings: escapes, surrogate pairs, and needsCopy semantics");

  // エスケープが無ければ入力バッファ直指し (needsCopy = true → '*' が付かない)
  EXPECT_TRACE("\"\"",     "s()",     kNone);
  EXPECT_TRACE("\"abc\"",  "s(abc)",  kNone);
  EXPECT_TRACE("\"a b\"",  "s(a b)",  kNone);

  // エスケープを含めばアリーナ上へ (needsCopy = false → '*' が付く)
  EXPECT_TRACE("\"a\\nb\"",  "s*(a<0A>b)", kNone);
  EXPECT_TRACE("\"\\\"\"",   "s*(\")",     kNone);   // \" -> " (印字可能なのでそのまま出る)
  EXPECT_TRACE("\"\\\\\"",   "s*(\\)",    kNone);   // 逆スラッシュ(印字可能)
  EXPECT_TRACE("\"\\/\"",    "s*(/)",      kNone);
  EXPECT_TRACE("\"\\b\\f\\n\\r\\t\"", "s*(<08><0C><0A><0D><09>)", kNone);

  // \uXXXX (R1-5)
  EXPECT_TRACE("\"\\u0041\"",   "s*(A)",             kNone);
  EXPECT_TRACE("\"\\u0000\"",   "s*(<00>)",          kNone);   // 埋め込みヌルも扱える
  EXPECT_TRACE("\"\\u00E9\"",   "s*(<C3><A9>)",      kNone);   // 2 バイト
  EXPECT_TRACE("\"\\u3042\"",   "s*(<E3><81><82>)",  kNone);   // 3 バイト
  EXPECT_TRACE("\"\\uFEFF\"",   "s*(<EF><BB><BF>)",  kNone);   // 文字列内の BOM は正当な内容

  // サロゲートペアを合成して 4 バイトへ
  EXPECT_TRACE("\"\\uD83D\\uDE00\"", "s*(<F0><9F><98><80>)", kNone);
  EXPECT_TRACE("\"\\uD800\\uDC00\"", "s*(<F0><90><80><80>)", kNone);   // U+10000
  EXPECT_TRACE("\"\\uDBFF\\uDFFF\"", "s*(<F4><8F><BF><BF>)", kNone);   // U+10FFFF

  // 生の UTF-8 はそのまま通す
  EXPECT_TRACE("\"\xE3\x81\x82\"", "s(<E3><81><82>)", kNone);
  EXPECT_TRACE("\"\xF0\x9F\x98\x80\"", "s(<F0><9F><98><80>)", kNone);

  // キーにも同じ規則が効くこと
  EXPECT_TRACE("{\"\\u0041\":1}", "{k*(A)i(1)}1", kNone);
  EXPECT_TRACE("{\"A\":1}",       "{k(A)i(1)}1",  kNone);
}

static void Test_StringErrors() {
  BeginCase("strings: invalid escapes, surrogates, control chars, and UTF-8");

  // 終端されていない
  EXPECT_ERROR("\"abc",   ErrorCode::StringUnterminated, 0, kNone);
  EXPECT_ERROR("\"",      ErrorCode::StringUnterminated, 0, kNone);
  EXPECT_ERROR("\"a\\",   ErrorCode::StringUnterminated, 0, kNone);
  EXPECT_ERROR("[\"a]",   ErrorCode::StringUnterminated, 1, kNone);

  // 未知のエスケープ
  EXPECT_ERROR("\"\\q\"",  ErrorCode::StringEscapeInvalid, 1, kNone);
  EXPECT_ERROR("\"a\\z\"", ErrorCode::StringEscapeInvalid, 2, kNone);
  EXPECT_ERROR("\"\\U0041\"", ErrorCode::StringEscapeInvalid, 1, kNone);   // 大文字 U は不可

  // \uXXXX の桁不正
  EXPECT_ERROR("\"\\u12\"",   ErrorCode::StringUnicodeEscapeInvalid, 1, kNone);
  EXPECT_ERROR("\"\\u12G4\"", ErrorCode::StringUnicodeEscapeInvalid, 1, kNone);
  EXPECT_ERROR("\"\\u\"",     ErrorCode::StringUnicodeEscapeInvalid, 1, kNone);
  EXPECT_ERROR("\"\\u123\"",  ErrorCode::StringUnicodeEscapeInvalid, 1, kNone);

  // サロゲートの不整合
  EXPECT_ERROR("\"\\uD800\"",       ErrorCode::StringUnicodeSurrogateInvalid, 1, kNone);
  EXPECT_ERROR("\"\\uDC00\"",       ErrorCode::StringUnicodeSurrogateInvalid, 1, kNone);
  EXPECT_ERROR("\"\\uDC00\\uD800\"", ErrorCode::StringUnicodeSurrogateInvalid, 1, kNone);
  EXPECT_ERROR("\"\\uD800\\u0041\"", ErrorCode::StringUnicodeSurrogateInvalid, 1, kNone);
  EXPECT_ERROR("\"\\uD800A\"",       ErrorCode::StringUnicodeSurrogateInvalid, 1, kNone);
  EXPECT_ERROR("\"a\\uDFFF\"",       ErrorCode::StringUnicodeSurrogateInvalid, 2, kNone);

  // 生の制御文字 (R1-6)
  EXPECT_ERROR(StringView("\"\x01\"", 3),    ErrorCode::StringControlCharUnescaped, 1, kNone);
  EXPECT_ERROR(StringView("\"a\n\"", 4),     ErrorCode::StringControlCharUnescaped, 2, kNone);
  EXPECT_ERROR(StringView("\"a\tb\"", 5),    ErrorCode::StringControlCharUnescaped, 2, kNone);
  EXPECT_ERROR(StringView("\"a\0b\"", 5),    ErrorCode::StringControlCharUnescaped, 2, kNone);
  EXPECT_ERROR(StringView("\"\x1F\"", 3),    ErrorCode::StringControlCharUnescaped, 1, kNone);

  // 不正な UTF-8
  EXPECT_ERROR("\"\xC0\x80\"",         ErrorCode::StringInvalidUtf8, 1, kNone);  // 過長符号化
  EXPECT_ERROR("\"\xC1\xBF\"",         ErrorCode::StringInvalidUtf8, 1, kNone);  // 過長符号化
  EXPECT_ERROR("\"\xE0\x80\x80\"",     ErrorCode::StringInvalidUtf8, 1, kNone);  // 過長符号化
  EXPECT_ERROR("\"\xED\xA0\x80\"",     ErrorCode::StringInvalidUtf8, 1, kNone);  // サロゲート
  EXPECT_ERROR("\"\xF0\x80\x80\x80\"", ErrorCode::StringInvalidUtf8, 1, kNone);  // 過長符号化
  EXPECT_ERROR("\"\xF4\x90\x80\x80\"", ErrorCode::StringInvalidUtf8, 1, kNone);  // U+10FFFF 超
  EXPECT_ERROR("\"\xF5\x80\x80\x80\"", ErrorCode::StringInvalidUtf8, 1, kNone);  // 範囲外の先行バイト
  EXPECT_ERROR("\"\x80\"",             ErrorCode::StringInvalidUtf8, 1, kNone);  // 継続バイト単独
  EXPECT_ERROR("\"\xE3\x81\"",         ErrorCode::StringInvalidUtf8, 1, kNone);  // 途中で切れている
  EXPECT_ERROR("\"a\xE3\"",            ErrorCode::StringInvalidUtf8, 2, kNone);
}

// ---------------------------------------------------------------------------
// 4. BOM (R1-4)
// ---------------------------------------------------------------------------

static void Test_ByteOrderMark() {
  BeginCase("BOM: leading UTF-8 BOM is skipped, other encodings are rejected");

  EXPECT_TRACE("\xEF\xBB\xBF{}",    "{}0",    kNone);
  EXPECT_TRACE("\xEF\xBB\xBF 123 ", "i(123)", kNone);
  EXPECT_TRACE("\xEF\xBB\xBF[1]",   "[i(1)]1", kNone);

  // BOM だけの入力は空ドキュメント
  EXPECT_ERROR("\xEF\xBB\xBF", ErrorCode::DocumentEmpty, 3, kNone);

  // UTF-16 / UTF-32 の BOM は非対応
  EXPECT_ERROR(StringView("\xFF\xFE{\0}\0", 6),           ErrorCode::EncodingNotSupported, 0, kNone);
  EXPECT_ERROR(StringView("\xFE\xFF\0{\0}", 6),           ErrorCode::EncodingNotSupported, 0, kNone);
  EXPECT_ERROR(StringView("\xFF\xFE\0\0{\0\0\0", 8),      ErrorCode::EncodingNotSupported, 0, kNone);
  EXPECT_ERROR(StringView("\0\0\xFE\xFF\0\0\0{", 8),      ErrorCode::EncodingNotSupported, 0, kNone);

  // 途中に現れた BOM は構造としてエラー
  EXPECT_ERROR("[1,\xEF\xBB\xBF 2]", ErrorCode::ValueInvalid, 3, kNone);
  EXPECT_ERROR("{\xEF\xBB\xBF}",     ErrorCode::ObjectMissingName, 1, kNone);

  // 文字列の中の U+FEFF は正当な内容(BOM として扱わない)
  EXPECT_TRACE("\"\xEF\xBB\xBF\"", "s(<EF><BB><BF>)", kNone);
}

// ---------------------------------------------------------------------------
// 5. JSONC (T-2)
// ---------------------------------------------------------------------------

static void Test_Comments() {
  BeginCase("JSONC: comments toggle by flag and never leak from string literals");

  // フラグで許可 / 拒否が切り替わる
  EXPECT_TRACE("// c\n1",   "i(1)", kComment);
  EXPECT_TRACE("/* c */1",  "i(1)", kComment);
  EXPECT_TRACE("1 // tail", "i(1)", kComment);
  EXPECT_ERROR("// c\n1",   ErrorCode::CommentNotAllowed, 0, kNone);
  EXPECT_ERROR("/* c */1",  ErrorCode::CommentNotAllowed, 0, kNone);
  EXPECT_ERROR("1 // tail", ErrorCode::CommentNotAllowed, 2, kNone);

  // 未閉鎖のブロックコメント
  EXPECT_ERROR("/* c",     ErrorCode::CommentUnterminated, 0, kComment);
  EXPECT_ERROR("1 /*",     ErrorCode::CommentUnterminated, 2, kComment);
  EXPECT_ERROR("[1 /* ]",  ErrorCode::CommentUnterminated, 3, kComment);
  EXPECT_ERROR("/* * /",   ErrorCode::CommentUnterminated, 0, kComment);

  // 単独の `/` はコメントではないので構文エラーとして扱われる
  EXPECT_ERROR("/",  ErrorCode::ValueInvalid, 0, kComment);
  EXPECT_ERROR("/x", ErrorCode::ValueInvalid, 0, kComment);

  // 文字列リテラル内の // /* */ をコメントと誤認しないこと
  EXPECT_TRACE("\"//\"",       "s(//)",       kNone);
  EXPECT_TRACE("\"/*\"",       "s(/*)",       kNone);
  EXPECT_TRACE("\"*/\"",       "s(*/)",       kNone);
  EXPECT_TRACE("\"// not a comment\"", "s(// not a comment)", kNone);
  EXPECT_TRACE("\"//\"",       "s(//)",       kJsonC);
  EXPECT_TRACE("\"/* x */\"",  "s(/* x */)",  kJsonC);
  EXPECT_TRACE("[\"//\",\"/*\"]", "[s(//)s(/*)]2", kJsonC);
  EXPECT_TRACE("{\"//\":\"/*\"}", "{k(//)s(/*)}1", kJsonC);

  // 空白が許される全位置でコメントが効くこと
  EXPECT_TRACE("/*a*/{/*b*/\"k\"/*c*/:/*d*/1/*e*/,/*f*/\"j\"/*g*/:/*h*/2/*i*/}/*j*/",
               "{k(k)i(1)k(j)i(2)}2", kComment);
  EXPECT_TRACE("[/*a*/1/*b*/,/*c*/2/*d*/]", "[i(1)i(2)]2", kComment);
  EXPECT_TRACE("{//x\n}",  "{}0", kComment);
  EXPECT_TRACE("[//x\n]",  "[]0", kComment);
  EXPECT_TRACE("//x\n[]//y", "[]0", kComment);

  // ブロックコメントはネストしない。最初の */ で閉じ、残りは構文エラーになる
  EXPECT_ERROR("/* /* */ */1", ErrorCode::ValueInvalid, 9, kComment);
  EXPECT_TRACE("/* /* */1",    "i(1)", kComment);
}

static void Test_TrailingComma() {
  BeginCase("JSONC: trailing commas toggle by flag");

  EXPECT_TRACE("[1,]",         "[i(1)]1",     kComma);
  EXPECT_TRACE("[1,2,]",       "[i(1)i(2)]2", kComma);
  EXPECT_TRACE("{\"a\":1,}",   "{k(a)i(1)}1", kComma);
  EXPECT_TRACE("[ 1 , ]",      "[i(1)]1",     kComma);
  EXPECT_TRACE("[[1,],]",      "[[i(1)]1]1",  kComma);
  EXPECT_TRACE("[1/*c*/,/*c*/]", "[i(1)]1",   kJsonC);

  EXPECT_ERROR("[1,]",       ErrorCode::TrailingCommaNotAllowed, 2, kNone);
  EXPECT_ERROR("[1,2,]",     ErrorCode::TrailingCommaNotAllowed, 4, kNone);
  EXPECT_ERROR("{\"a\":1,}", ErrorCode::TrailingCommaNotAllowed, 6, kNone);

  // 二重カンマは末尾カンマではないので、許可しても通らない
  EXPECT_ERROR("[1,,]", ErrorCode::ValueInvalid, 3, kComma);
  EXPECT_ERROR("[,]",   ErrorCode::ValueInvalid, 1, kComma);
  EXPECT_ERROR("{\"a\":1,,}", ErrorCode::ObjectMissingName, 7, kComma);
}

// ---------------------------------------------------------------------------
// 6. NaN / Infinity (R1-18)
// ---------------------------------------------------------------------------

static void Test_NaNInfinity() {
  BeginCase("NaN / Infinity: keyword matching only, never handed to from_chars");

  // AllowNaNInf 有効時のみ受理される
  {
    ++GLFD::Test::g_checkCount;
    NumberProbe probe;
    const ParseError error = ProbeScalar(StringView("NaN"), probe, kNaNInf);
    const double value = probe.Double();
    if (!error.IsOk() || probe.Kind() != Scalar::Double || value == value) {
      Fail(__LINE__, "\"NaN\" with AllowNaNInf must yield a NaN Double");
    }
  }
  ExpectDoubleImpl(__LINE__, StringView("Infinity"),
                   std::numeric_limits<double>::infinity(), true, kNaNInf);
  ExpectDoubleImpl(__LINE__, StringView("-Infinity"),
                   -std::numeric_limits<double>::infinity(), true, kNaNInf);

  // フラグ未指定なら拒否
  EXPECT_ERROR("NaN",       ErrorCode::NaNInfNotAllowed, 0, kNone);
  EXPECT_ERROR("Infinity",  ErrorCode::NaNInfNotAllowed, 0, kNone);
  EXPECT_ERROR("-Infinity", ErrorCode::NaNInfNotAllowed, 0, kNone);
  EXPECT_ERROR("[NaN]",     ErrorCode::NaNInfNotAllowed, 1, kNone);

  // 小文字 / 部分一致は AllowNaNInf 有効でも拒否されること (R1-18)
  EXPECT_ERROR("nan",      ErrorCode::ValueInvalid, 0, kNaNInf);
  EXPECT_ERROR("inf",      ErrorCode::ValueInvalid, 0, kNaNInf);
  EXPECT_ERROR("Inf",      ErrorCode::ValueInvalid, 0, kNaNInf);
  EXPECT_ERROR("INFINITY", ErrorCode::ValueInvalid, 0, kNaNInf);
  EXPECT_ERROR("NAN",      ErrorCode::ValueInvalid, 0, kNaNInf);
  EXPECT_ERROR("Nan",      ErrorCode::ValueInvalid, 0, kNaNInf);
  EXPECT_ERROR("infinity", ErrorCode::ValueInvalid, 0, kNaNInf);
  EXPECT_ERROR("-inf",     ErrorCode::NumberInvalid, 1, kNaNInf);
  EXPECT_ERROR("-Inf",     ErrorCode::NumberInvalid, 1, kNaNInf);
}

// ---------------------------------------------------------------------------
// 7. ハンドラ中断 (R1-1c)
// ---------------------------------------------------------------------------

static void Test_HandlerAbort() {
  BeginCase("handler abort: HandlerAborted is distinct from syntax errors");

  struct Case {
    const char* json;
    int         abortAfter;
    size_t      expectedOffset;
    int         line;
  };

  const Case cases[] = {
    { "[1,2,3]",   0, 0, __LINE__ },   // OnArrayBegin で中断
    { "[1,2,3]",   1, 1, __LINE__ },   // OnInt64(1) で中断
    { "[1,2,3]",   2, 3, __LINE__ },   // OnInt64(2) で中断
    { "[1,2,3]",   4, 6, __LINE__ },   // OnArrayEnd で中断(`]` の位置)
    { "{\"a\":1}", 1, 1, __LINE__ },   // OnKey で中断(キーの `"` の位置)
    { "{\"a\":1}", 2, 5, __LINE__ },   // OnInt64 で中断
    { "{}",        1, 1, __LINE__ },   // 空オブジェクトの OnObjectEnd で中断
    { "[]",        1, 1, __LINE__ },   // 空配列の OnArrayEnd で中断
    { "null",      0, 0, __LINE__ },   // ルートスカラで中断
    { "\"abc\"",   0, 0, __LINE__ },
  };

  for (const Case& testCase : cases) {
    ++GLFD::Test::g_checkCount;

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;
    handler.AbortAfter(testCase.abortAfter);

    const StringView json(testCase.json);
    const ParseError error = reader.Parse(json, handler, kNone);

    if (error.code != ErrorCode::HandlerAborted || error.offset != testCase.expectedOffset) {
      char want[128];
      char got[128];
      const ParseError expected{ ErrorCode::HandlerAborted, testCase.expectedOffset };
      FailDetail(testCase.line, "handler abort mismatch",
                 FormatError(expected, want, sizeof(want)),
                 FormatError(error, got, sizeof(got)), json);
    }
  }

  // 中断しなければ当然成功する(同じ入力で対照を取る)
  EXPECT_TRACE("[1,2,3]", "[i(1)i(2)i(3)]3", kNone);
}

// ---------------------------------------------------------------------------
// 8. 深度制限 (T-6)
// ---------------------------------------------------------------------------

namespace {

  /// `[` を depth 個、`]` を depth 個並べた入力を作る
  size_t BuildNestedArrays(char* buffer, size_t capacity, size_t depth) {
    const size_t total = depth * 2;
    if (total > capacity) {
      return 0;
    }
    for (size_t i = 0; i < depth; ++i) {
      buffer[i] = '[';
      buffer[depth + i] = ']';
    }
    return total;
  }

}

static void Test_DepthLimit() {
  BeginCase("depth limit: MaxDepth is honored and deep input never overflows the stack");

  static char buffer[8192];

  // 既定 (256) の境界
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;

    ++GLFD::Test::g_checkCount;
    CHECK(reader.MaxDepth() == JsonReader::kDefaultMaxDepth);

    const size_t okLength = BuildNestedArrays(buffer, sizeof(buffer), 256);
    const ParseError okError = reader.Parse(StringView(buffer, okLength), handler, kNone);
    ++GLFD::Test::g_checkCount;
    if (!okError.IsOk()) {
      char scratch[128];
      FailDetail(__LINE__, "256 levels must parse", "<ok>",
                 FormatError(okError, scratch, sizeof(scratch)), StringView("[...]"));
    }
  }
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;

    const size_t tooDeep = BuildNestedArrays(buffer, sizeof(buffer), 257);
    const ParseError error = reader.Parse(StringView(buffer, tooDeep), handler, kNone);
    ++GLFD::Test::g_checkCount;
    // 257 段目の `[` はインデックス 256 にある
    if (error.code != ErrorCode::DepthLimitExceeded || error.offset != 256) {
      char want[128];
      char got[128];
      const ParseError expected{ ErrorCode::DepthLimitExceeded, 256 };
      FailDetail(__LINE__, "257 levels must be rejected",
                 FormatError(expected, want, sizeof(want)),
                 FormatError(error, got, sizeof(got)), StringView("[...]"));
    }
  }

  // SetMaxDepth が効くこと
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;
    reader.SetMaxDepth(3);
    ++GLFD::Test::g_checkCount;
    CHECK(reader.MaxDepth() == 3);

    EXPECT_TRACE("[[[1]]]", "[[[i(1)]1]1]1", kNone);   // 別インスタンス(既定深度)で対照

    const ParseError ok = reader.Parse(StringView("[[[1]]]"), handler, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(ok.IsOk());

    JsonTraceHandler handler2;
    const ParseError tooDeep = reader.Parse(StringView("[[[[1]]]]"), handler2, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(tooDeep.code == ErrorCode::DepthLimitExceeded && tooDeep.offset == 3);
  }

  // MaxDepth 0 ではコンテナを一切開けない。スカラは通る
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    reader.SetMaxDepth(0);

    JsonTraceHandler handler;
    const ParseError containerError = reader.Parse(StringView("[]"), handler, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(containerError.code == ErrorCode::DepthLimitExceeded && containerError.offset == 0);

    JsonTraceHandler handler2;
    const ParseError scalarOk = reader.Parse(StringView("1"), handler2, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(scalarOk.IsOk() && handler2.Trace() == StringView("i(1)"));
  }

  // 非常に深い入力でもクラッシュしないこと(反復パーサなのでネイティブスタックを消費しない)
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    reader.SetMaxDepth(2000);

    JsonTraceHandler handler;
    const size_t length = BuildNestedArrays(buffer, sizeof(buffer), 2000);
    const ParseError error = reader.Parse(StringView(buffer, length), handler, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error.IsOk());

    JsonTraceHandler handler2;
    const size_t tooDeep = BuildNestedArrays(buffer, sizeof(buffer), 2001);
    const ParseError error2 = reader.Parse(StringView(buffer, tooDeep), handler2, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error2.code == ErrorCode::DepthLimitExceeded);
  }
}

// ---------------------------------------------------------------------------
// 9. 深度スタックの移送 (T-6a)
//    インライン 64 フレームを超えないとアリーナ経路が一度も走らないため、
//    ここを狙ったテストが無いと取りこぼしの発見が非常に遅れる
// ---------------------------------------------------------------------------

namespace {

  /**
   * @brief 各段が「内側の配列 + 9」の 2 要素になるネストを組み立てる
   * @details `[[...[1,2,3]...,9],9]` の形。全段の要素数が 2、最内が 3 になるので、
   *          インライン→アリーナ移送でカウンタが失われれば必ず検出できる
   */
  size_t BuildCountedNesting(char* json, size_t jsonCapacity,
                             char* trace, size_t traceCapacity,
                             size_t depth, size_t& traceLength) {
    size_t jsonLength = 0;
    size_t traceUsed  = 0;

    const auto putJson = [&](const char* text) {
      while (*text != '\0' && jsonLength < jsonCapacity) { json[jsonLength++] = *text++; }
    };
    const auto putTrace = [&](const char* text) {
      while (*text != '\0' && traceUsed < traceCapacity) { trace[traceUsed++] = *text++; }
    };

    for (size_t i = 0; i + 1 < depth; ++i) {
      putJson("[");
      putTrace("[");
    }
    putJson("[1,2,3]");
    putTrace("[i(1)i(2)i(3)]3");
    for (size_t i = 0; i + 1 < depth; ++i) {
      putJson(",9]");
      putTrace("i(9)]2");
    }

    traceLength = traceUsed;
    return jsonLength;
  }

}

static void Test_FrameTransfer() {
  BeginCase("frame transfer: nesting past the inline 64 frames keeps counts intact");

  static char json[16384];
  static char trace[16384];

  // 深さ 65 以上が正しくパースでき、全段の要素数が保たれること
  for (const size_t depth : { size_t{ 63 }, size_t{ 64 }, size_t{ 65 },
                              size_t{ 66 }, size_t{ 100 }, size_t{ 200 } }) {
    size_t       traceLength = 0;
    const size_t jsonLength  = BuildCountedNesting(json, sizeof(json),
                                                   trace, sizeof(trace),
                                                   depth, traceLength);

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;

    const ParseError error = reader.Parse(StringView(json, jsonLength), handler, kNone);

    ++GLFD::Test::g_checkCount;
    if (!error.IsOk()) {
      char scratch[128];
      char what[96];
      std::snprintf(what, sizeof(what), "depth %zu must parse", depth);
      FailDetail(__LINE__, what, "<ok>", FormatError(error, scratch, sizeof(scratch)),
                 StringView(json, jsonLength < 60 ? jsonLength : 60));
      continue;
    }

    ++GLFD::Test::g_checkCount;
    if (handler.Trace() != StringView(trace, traceLength)) {
      char what[96];
      std::snprintf(what, sizeof(what), "depth %zu: counts lost across the transfer", depth);
      Fail(__LINE__, what);
    }
  }

  // インライン内で収まる深さではアリーナに一切触れないこと
  {
    size_t       traceLength = 0;
    const size_t jsonLength  = BuildCountedNesting(json, sizeof(json), trace, sizeof(trace),
                                                   60, traceLength);
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;

    const ParseError error = reader.Parse(StringView(json, jsonLength), handler, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error.IsOk());
    ++GLFD::Test::g_checkCount;
    CHECK(mock.AllocateCalls() == 0);
  }

  // 深さ 65 以上で確保失敗を注入すると OutOfMemory になり、クラッシュしないこと
  {
    size_t       traceLength = 0;
    const size_t jsonLength  = BuildCountedNesting(json, sizeof(json), trace, sizeof(trace),
                                                   70, traceLength);
    MockMemoryResource mock;
    mock.SetFailAfter(0);                 // 最初のアリーナ確保(フレーム移送)を失敗させる

    JsonArena        arena(&mock);
    JsonReader       reader(arena);
    JsonTraceHandler handler;

    const ParseError error = reader.Parse(StringView(json, jsonLength), handler, kNone);
    ++GLFD::Test::g_checkCount;
    // 65 段目の `[` はインデックス 64 にある
    if (error.code != ErrorCode::OutOfMemory || error.offset != 64) {
      char want[128];
      char got[128];
      const ParseError expected{ ErrorCode::OutOfMemory, 64 };
      FailDetail(__LINE__, "frame transfer OOM", FormatError(expected, want, sizeof(want)),
                 FormatError(error, got, sizeof(got)), StringView("[[[...]]]"));
    }
    ++GLFD::Test::g_checkCount;
    CHECK(mock.InjectedFailures() >= 1);
  }

  // Parse をまたいでバッファが再利用され、追加確保が発生しないこと (§3.5.1)
  {
    size_t       traceLength = 0;
    const size_t jsonLength  = BuildCountedNesting(json, sizeof(json), trace, sizeof(trace),
                                                   70, traceLength);
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);

    JsonTraceHandler first;
    const ParseError firstError = reader.Parse(StringView(json, jsonLength), first, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(firstError.IsOk());

    const int callsAfterFirst = mock.AllocateCalls();
    ++GLFD::Test::g_checkCount;
    CHECK(callsAfterFirst >= 1);          // 1 回はフレーム用に確保しているはず

    for (int i = 0; i < 5; ++i) {
      JsonTraceHandler repeat;
      const ParseError repeatError = reader.Parse(StringView(json, jsonLength), repeat, kNone);
      ++GLFD::Test::g_checkCount;
      CHECK(repeatError.IsOk());
      ++GLFD::Test::g_checkCount;
      CHECK(repeat.Trace() == StringView(trace, traceLength));
    }

    ++GLFD::Test::g_checkCount;
    // 容量が足りている限り再確保しない
    CHECK(mock.AllocateCalls() == callsAfterFirst);
  }
}

// ---------------------------------------------------------------------------
// 10. 確保失敗の注入 (T-5 / T-6)
// ---------------------------------------------------------------------------

static void Test_OutOfMemory() {
  BeginCase("out of memory: unescape allocation failure propagates without crashing");

  // アンエスケープ用の確保が失敗したら OutOfMemory
  {
    MockMemoryResource mock;
    mock.SetFailAfter(0);
    JsonArena        arena(&mock);
    JsonReader       reader(arena);
    JsonTraceHandler handler;

    const ParseError error = reader.Parse(StringView("\"\\u0041\""), handler, kNone);
    ++GLFD::Test::g_checkCount;
    if (error.code != ErrorCode::OutOfMemory || error.offset != 0) {
      char want[128];
      char got[128];
      const ParseError expected{ ErrorCode::OutOfMemory, 0 };
      FailDetail(__LINE__, "unescape OOM", FormatError(expected, want, sizeof(want)),
                 FormatError(error, got, sizeof(got)), StringView("\"\\u0041\""));
    }
  }

  // 配列の途中の文字列で失敗しても、位置が正しく報告されること
  {
    MockMemoryResource mock;
    mock.SetFailAfter(0);
    JsonArena        arena(&mock);
    JsonReader       reader(arena);
    JsonTraceHandler handler;

    const ParseError error = reader.Parse(StringView("[1,\"a\\nb\"]"), handler, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error.code == ErrorCode::OutOfMemory && error.offset == 3);
  }

  // 失敗しなければ同じ入力が通ること(対照)
  EXPECT_TRACE("[1,\"a\\nb\"]", "[i(1)s*(a<0A>b)]2", kNone);

  // エスケープを含まない入力ではアリーナ確保が一度も起きないこと (N-4)
  {
    MockMemoryResource mock;
    mock.SetFailAfter(0);                 // 1 回でも確保したら失敗するはず
    JsonArena        arena(&mock);
    JsonReader       reader(arena);
    JsonTraceHandler handler;

    const ParseError error =
      reader.Parse(StringView("{\"a\":[1,2,\"plain\"],\"b\":{\"c\":true}}"), handler, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error.IsOk());
    ++GLFD::Test::g_checkCount;
    CHECK(mock.AllocateCalls() == 0);
  }

  // 失敗注入後もリーダを再利用でき、リークしないこと
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    {
      JsonReader       reader(arena);
      JsonTraceHandler handler;
      mock.SetFailAfter(0);
      const ParseError failed = reader.Parse(StringView("\"\\n\""), handler, kNone);
      ++GLFD::Test::g_checkCount;
      CHECK(failed.code == ErrorCode::OutOfMemory);

      mock.ClearFailure();
      JsonTraceHandler handler2;
      const ParseError ok = reader.Parse(StringView("\"\\n\""), handler2, kNone);
      ++GLFD::Test::g_checkCount;
      CHECK(ok.IsOk() && handler2.Trace() == StringView("s*(<0A>)"));
    }
    arena.Release();
    ++GLFD::Test::g_checkCount;
    // 注入で失敗した確保も AllocateCalls に数えられるので、その分を差し引いて突き合わせる
    CHECK(mock.AllocateCalls() - mock.InjectedFailures() == mock.DeallocateCalls());
    ++GLFD::Test::g_checkCount;
    CHECK(mock.LiveBlockCount() == 0);
    ++GLFD::Test::g_checkCount;
    CHECK(!mock.DoubleFree());
  }
}

// ---------------------------------------------------------------------------
// 11. 頑健性 (T-6)
// ---------------------------------------------------------------------------

static void Test_TruncatedInput() {
  BeginCase("robustness: every truncated prefix returns an error without crashing");

  // ルートがオブジェクトなので、真の接頭辞はすべて不完全 = 必ずエラーになる
  static const char* const kSources[] = {
    "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":null},\"e\":\"x\\ny\",\"f\":1.5e-3}",
    "{\"k\":\"\\uD83D\\uDE00\",\"n\":[true,false,null],\"deep\":[[[[1]]]]}",
    "{\"u\":\"\xE3\x81\x82\xE3\x81\x84\",\"z\":-0.0,\"big\":9223372036854775807}",
  };
  static const ParseFlags kFlagSets[] = { kNone, kJsonC, kNaNInf };

  for (const char* source : kSources) {
    const StringView full(source);

    // まず完全な入力が通ることを確かめる(前提が崩れていたら意味が無い)
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      JsonReader         reader(arena);
      JsonTraceHandler   handler;
      ++GLFD::Test::g_checkCount;
      CHECK(reader.Parse(full, handler, kNone).IsOk());
    }

    for (const ParseFlags flags : kFlagSets) {
      for (size_t length = 0; length < full.Size(); ++length) {
        MockMemoryResource mock;
        JsonArena          arena(&mock);
        JsonReader         reader(arena);
        JsonTraceHandler   handler;

        const StringView prefix(full.Data(), length);
        const ParseError error = reader.Parse(prefix, handler, flags);

        ++GLFD::Test::g_checkCount;
        if (error.IsOk()) {
          char what[160];
          std::snprintf(what, sizeof(what),
                        "truncated prefix of length %zu unexpectedly parsed", length);
          Fail(__LINE__, what);
        }
        else if (error.offset > length) {
          char what[160];
          std::snprintf(what, sizeof(what),
                        "error offset %zu exceeds prefix length %zu", error.offset, length);
          Fail(__LINE__, what);
        }
      }
    }
  }
}

static void Test_RandomBytes() {
  BeginCase("robustness: random byte sequences never crash and keep offsets in range");

  // 再現可能な擬似乱数 (xorshift)。外部依存を持ち込まない
  std::uint32_t state = 0x9E3779B9u;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };

  static const ParseFlags kFlagSets[] = { kNone, kJsonC, kNaNInf, kJsonC | kNaNInf };
  static const char kAlphabet[] = "{}[]\",:0123456789.eE-+ \t\n\r\\/*truefalsnI\x80\xC0\xE3\xF5";
  constexpr size_t kAlphabetSize = sizeof(kAlphabet) - 1;

  char buffer[128];

  for (int iteration = 0; iteration < 3000; ++iteration) {
    const size_t length = next() % sizeof(buffer);
    for (size_t i = 0; i < length; ++i) {
      // 半分は JSON らしい文字、半分は完全にランダムなバイト
      buffer[i] = ((next() & 1u) != 0u)
        ? kAlphabet[next() % kAlphabetSize]
        : static_cast<char>(next() & 0xFFu);
    }

    const ParseFlags flags = kFlagSets[next() % (sizeof(kFlagSets) / sizeof(kFlagSets[0]))];

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonReader         reader(arena);
    JsonTraceHandler   handler;

    const ParseError error = reader.Parse(StringView(buffer, length), handler, flags);

    // 成否は問わない。オフセットが必ず入力の範囲内であることだけを不変条件として見る
    ++GLFD::Test::g_checkCount;
    if (!error.IsOk() && error.offset > length) {
      char what[160];
      std::snprintf(what, sizeof(what),
                    "random input: offset %zu exceeds length %zu", error.offset, length);
      Fail(__LINE__, what);
    }
  }
}

// ---------------------------------------------------------------------------
// 12. 再利用と独立性 (R1-10)
// ---------------------------------------------------------------------------

static void Test_Reuse() {
  BeginCase("reuse: the same reader parses repeatedly with no shared state");

  MockMemoryResource mock;
  JsonArena          arena(&mock);
  JsonReader         reader(arena);

  struct Case { const char* json; const char* trace; };
  const Case cases[] = {
    { "[1,2]",       "[i(1)i(2)]2" },
    { "{\"a\":1}",   "{k(a)i(1)}1" },
    { "\"x\"",       "s(x)" },
    { "[[1],[2]]",   "[[i(1)]1[i(2)]1]2" },
    { "null",        "n" },
  };

  for (int pass = 0; pass < 3; ++pass) {
    for (const Case& testCase : cases) {
      JsonTraceHandler handler;
      const ParseError error = reader.Parse(StringView(testCase.json), handler, kNone);
      ++GLFD::Test::g_checkCount;
      CHECK(error.IsOk());
      ++GLFD::Test::g_checkCount;
      CHECK(handler.Trace() == StringView(testCase.trace));
    }

    // 失敗を挟んでも次のパースに影響しないこと
    JsonTraceHandler broken;
    const ParseError error = reader.Parse(StringView("{\"a\""), broken, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error.code == ErrorCode::ObjectMissingColon);
  }

  // アリーナを Reset しても引き続き使えること (R0-4)
  arena.Reset();
  JsonTraceHandler handler;
  const ParseError error = reader.Parse(StringView("\"a\\tb\""), handler, kNone);
  ++GLFD::Test::g_checkCount;
  CHECK(error.IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(handler.Trace() == StringView("s*(a<09>b)"));
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("JsonReader");

  Test_Structure();
  Test_StructureErrors();
  Test_NumberMapping();
  Test_NumberOverflow();
  Test_NumberRejection();
  Test_Strings();
  Test_StringErrors();
  Test_ByteOrderMark();
  Test_Comments();
  Test_TrailingComma();
  Test_NaNInfinity();
  Test_HandlerAbort();
  Test_DepthLimit();
  Test_FrameTransfer();
  Test_OutOfMemory();
  Test_TruncatedInput();
  Test_RandomBytes();
  Test_Reuse();

  return Summarize();
}
