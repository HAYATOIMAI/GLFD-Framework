/**
 * @file  JsonWriterTests.cpp
 * @brief JsonWriter / JsonPrettyWriter の単体テスト
 *        (フェーズ 1-4 / 要件 T-3, T-15, T-16, T-17, T-18, T-21)
 */

#include <charconv>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>

#include "TestHarness.h"
#include "MockMemoryResource.h"
#include "JsonTraceHandler.h"
#include "Core/Json/JsonDocument.h"
#include "Core/Json/JsonWriter.h"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

using GLFD::StringView;
using GLFD::Json::Document;
using GLFD::Json::ErrorCode;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonFileStream;
using GLFD::Json::JsonHandler;
using GLFD::Json::JsonPrettyWriter;
using GLFD::Json::JsonReader;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::JsonWriter;
using GLFD::Json::JsonWriterHandler;
using GLFD::Json::ParseError;
using GLFD::Json::ParseFlags;
using GLFD::Json::ToString;
using GLFD::Json::WriteFlags;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::JsonTraceHandler;
using GLFD::Test::MockMemoryResource;
using GLFD::Test::Summarize;

namespace {

  constexpr ParseFlags kNoParseFlags = ParseFlags::None;
  constexpr WriteFlags kNoWriteFlags = WriteFlags::None;

  using CompactWriter = JsonWriter<JsonStringBuffer>;
  using PrettyWriter  = JsonPrettyWriter<JsonStringBuffer>;

  // R1-15: Writer をそのまま SAX ハンドラとして使えること
  static_assert(JsonHandler<JsonWriterHandler<CompactWriter>>,
                "JsonWriterHandler must satisfy JsonHandler");
  static_assert(JsonHandler<JsonWriterHandler<PrettyWriter>>,
                "pretty writer must also work as a SAX handler");

  void Fail(int line, const char* message) {
    std::printf("    [FAIL] %s(%d): %s\n", __FILE__, line, message);
    std::fflush(stdout);
    ++GLFD::Test::g_failureCount;
  }

  void FailDetail(int line, const char* what, StringView expected, StringView actual) {
    std::printf("    [FAIL] %s(%d): %s\n"
                "             expected: %.*s\n"
                "             actual  : %.*s\n",
                __FILE__, line, what,
                static_cast<int>(expected.Size() < 400 ? expected.Size() : 400), expected.Data(),
                static_cast<int>(actual.Size() < 400 ? actual.Size() : 400), actual.Data());
    std::fflush(stdout);
    ++GLFD::Test::g_failureCount;
  }

  const char* NameOf(ErrorCode code) {
    static char buffer[128];
    const StringView name = ToString(code);
    std::snprintf(buffer, sizeof(buffer), "%.*s",
                  static_cast<int>(name.Size()), name.Data());
    return buffer;
  }

  /// JSON をパースし、compact / pretty のどちらかで書き戻したバイト列を得る
  bool Reserialize(StringView json, bool pretty, StringView& out,
                   JsonArena& arena, JsonStringBuffer& buffer,
                   ParseFlags parseFlags, WriteFlags writeFlags) {
    Document document(arena.Resource());
    if (!document.Parse(json, parseFlags).IsOk()) {
      return false;
    }
    const bool ok = pretty ? document.WritePrettyTo(buffer, writeFlags)
                           : document.WriteTo(buffer, writeFlags);
    out = buffer.View();
    return ok;
  }

  /// 書き出したバイト列が期待どおりであること
  void ExpectBytesImpl(int line, StringView json, StringView expected, bool pretty,
                       ParseFlags parseFlags, WriteFlags writeFlags) {
    ++GLFD::Test::g_checkCount;

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);
    StringView         actual;

    if (!Reserialize(json, pretty, actual, arena, buffer, parseFlags, writeFlags)) {
      FailDetail(line, "reserialize failed", expected, actual);
      return;
    }
    if (actual != expected) {
      FailDetail(line, "byte mismatch", expected, actual);
    }
  }

  #define EXPECT_COMPACT(json, expected) \
    ExpectBytesImpl(__LINE__, (json), (expected), false, kNoParseFlags, kNoWriteFlags)
  #define EXPECT_PRETTY(json, expected) \
    ExpectBytesImpl(__LINE__, (json), (expected), true, kNoParseFlags, kNoWriteFlags)
  #define EXPECT_COMPACT_F(json, expected, wf) \
    ExpectBytesImpl(__LINE__, (json), (expected), false, kNoParseFlags, (wf))

  /**
   * @brief T-3 の中核: パース → 書き出し → 再パースで SAX イベント列が一致すること
   * @note  1-3 の T-8 と同じ手法。Writer が情報を落としていれば必ず食い違う
   */
  void ExpectRoundTripImpl(int line, StringView json, bool pretty,
                           ParseFlags parseFlags, WriteFlags writeFlags) {
    ++GLFD::Test::g_checkCount;

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // (1) 元の入力を SAX へ直接流す
    JsonReader       firstReader(arena);
    JsonTraceHandler firstTrace;
    if (!firstReader.Parse(json, firstTrace, parseFlags).IsOk()) {
      Fail(line, "round-trip: the source did not parse");
      return;
    }

    // (2) DOM 経由で書き戻す
    JsonStringBuffer buffer(arena);
    StringView       written;
    if (!Reserialize(json, pretty, written, arena, buffer, parseFlags, writeFlags)) {
      Fail(line, "round-trip: reserialize failed");
      return;
    }

    // (3) 書き戻した結果を SAX へ流す
    JsonReader       secondReader(arena);
    JsonTraceHandler secondTrace;
    const ParseError reparse = secondReader.Parse(written, secondTrace,
                                                  parseFlags | ParseFlags::AllowNaNInf);
    if (!reparse.IsOk()) {
      char what[192];
      std::snprintf(what, sizeof(what), "round-trip: reparse failed (%s at %zu)",
                    NameOf(reparse.code), reparse.offset);
      Fail(line, what);
      std::printf("             written: %.*s\n",
                  static_cast<int>(written.Size() < 300 ? written.Size() : 300), written.Data());
      return;
    }

    // 文字列の needsCopy 印(`*`)は経路で必ず変わるので取り除いてから比較する
    const auto normalize = [](StringView trace, char* out, size_t capacity) -> size_t {
      size_t written2 = 0;
      for (size_t i = 0; i < trace.Size(); ++i) {
        const char c = trace[i];
        if (c == '*' && written2 > 0 && (out[written2 - 1] == 's' || out[written2 - 1] == 'k')
            && (i + 1) < trace.Size() && trace[i + 1] == '(') {
          continue;
        }
        if (written2 < capacity) { out[written2++] = c; }
      }
      return written2;
    };

    static char firstNormalized[16384];
    static char secondNormalized[16384];
    const size_t firstLength  = normalize(firstTrace.Trace(), firstNormalized,
                                          sizeof(firstNormalized));
    const size_t secondLength = normalize(secondTrace.Trace(), secondNormalized,
                                          sizeof(secondNormalized));

    if (StringView(firstNormalized, firstLength) != StringView(secondNormalized, secondLength)) {
      FailDetail(line, "round-trip: the writer lost information",
                 StringView(firstNormalized, firstLength),
                 StringView(secondNormalized, secondLength));
      std::printf("             written: %.*s\n",
                  static_cast<int>(written.Size() < 300 ? written.Size() : 300), written.Data());
    }
  }

  #define EXPECT_ROUND_TRIP(json)                                                    \
    do {                                                                            \
      ExpectRoundTripImpl(__LINE__, (json), false, kNoParseFlags, kNoWriteFlags);    \
      ExpectRoundTripImpl(__LINE__, (json), true,  kNoParseFlags, kNoWriteFlags);    \
    } while (0)

}

// ---------------------------------------------------------------------------
// T-3 数値の round-trip
// ---------------------------------------------------------------------------

static void Test_DoubleRoundTrip() {
  BeginCase("T-3: every double survives Double() -> from_chars bit-exactly");

  MockMemoryResource mock;
  JsonArena          arena(&mock);

  const auto check = [&](double value, int line) {
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    if (!writer.Double(value)) {
      char what[128];
      std::snprintf(what, sizeof(what), "Double() failed for %.17g", value);
      Fail(line, what);
      return;
    }

    const StringView text = buffer.View();
    double           back = 0.0;
    const auto result = std::from_chars(text.Data(), text.Data() + text.Size(), back);
    if (result.ec != std::errc{} || result.ptr != text.Data() + text.Size()) {
      char what[192];
      std::snprintf(what, sizeof(what), "output %.*s did not parse back",
                    static_cast<int>(text.Size()), text.Data());
      Fail(line, what);
      return;
    }

    std::uint64_t original = 0;
    std::uint64_t restored = 0;
    std::memcpy(&original, &value, sizeof(original));
    std::memcpy(&restored, &back, sizeof(restored));
    if (original != restored) {
      char what[192];
      std::snprintf(what, sizeof(what), "bit mismatch: %.17g -> %.*s -> %.17g",
                    value, static_cast<int>(text.Size()), text.Data(), back);
      Fail(line, what);
    }
  };

  // 明示的に押さえる値
  static const double kExplicit[] = {
    0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 0.1, -0.1, 1.0 / 3.0,
    1e308, -1e308, 1e-308, -1e-308,
    1.7976931348623157e308,      // DBL_MAX
    2.2250738585072014e-308,     // 正規化最小
    4.9406564584124654e-324,     // 非正規化最小
    3.141592653589793, 2.718281828459045,
    9007199254740992.0,          // 2^53
    123456789.123456789,
  };
  for (const double value : kExplicit) {
    ++GLFD::Test::g_checkCount;
    check(value, __LINE__);
  }

  // ランダムな bit パターンから作った double を大量に往復させる
  {
    std::uint64_t state = 0x123456789ABCDEFull;
    const auto    next  = [&state]() {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return state;
    };

    int tested = 0;
    for (int i = 0; i < 20000; ++i) {
      const std::uint64_t bits  = next();
      double              value = 0.0;
      std::memcpy(&value, &bits, sizeof(value));
      if (value != value) {
        continue;   // NaN は既定でエラーになるので別のケースで扱う
      }
      const double infinity = std::numeric_limits<double>::infinity();
      if (value == infinity || value == -infinity) {
        continue;
      }
      check(value, __LINE__);
      ++tested;
    }
    ++GLFD::Test::g_checkCount;
    CHECK_QUIET(tested > 19000);   // 大半が有限値であること
  }

  // 整数に見える double が Int64 へ化けないこと(".0" が付くこと)
  {
    EXPECT_COMPACT("1.0",   "1.0");
    EXPECT_COMPACT("-0.0",  "-0.0");
    EXPECT_COMPACT("2.0e0", "2.0");
    EXPECT_COMPACT("1e2",   "100.0");
    // R1-19: 小数点の無い -0 は Int64(0) なので "0" に戻る(意図的な非可逆)
    EXPECT_COMPACT("-0", "0");
    EXPECT_COMPACT("0",  "0");
  }
}

// ---------------------------------------------------------------------------
// T-3 ドキュメント全体の round-trip
// ---------------------------------------------------------------------------

static void Test_DocumentRoundTrip() {
  BeginCase("T-3: parse -> write -> reparse preserves the SAX event stream");

  // スカラ
  EXPECT_ROUND_TRIP("null");
  EXPECT_ROUND_TRIP("true");
  EXPECT_ROUND_TRIP("false");
  EXPECT_ROUND_TRIP("0");
  EXPECT_ROUND_TRIP("-123");
  EXPECT_ROUND_TRIP("9223372036854775807");
  EXPECT_ROUND_TRIP("18446744073709551615");
  EXPECT_ROUND_TRIP("1.5e-3");
  EXPECT_ROUND_TRIP("-0.0");
  EXPECT_ROUND_TRIP("\"scalar\"");

  // 空コンテナ
  EXPECT_ROUND_TRIP("{}");
  EXPECT_ROUND_TRIP("[]");
  EXPECT_ROUND_TRIP("[[],{}]");
  EXPECT_ROUND_TRIP("{\"a\":{},\"b\":[]}");

  // 構造
  EXPECT_ROUND_TRIP("[1,2,3]");
  EXPECT_ROUND_TRIP("{\"a\":1,\"b\":\"x\",\"c\":null}");
  EXPECT_ROUND_TRIP("{\"a\":[1,{\"b\":[true,false]}]}");

  // キー重複(順序が保たれること)
  EXPECT_ROUND_TRIP("{\"k\":1,\"k\":2,\"k\":3}");
  EXPECT_ROUND_TRIP("{\"a\":1,\"b\":2,\"a\":3,\"b\":4}");

  // エスケープ
  EXPECT_ROUND_TRIP("{\"plain\":\"abc\",\"esc\":\"a\\nb\\u0041\"}");
  EXPECT_ROUND_TRIP("[\"\\u0000\",\"\\u001f\",\"\\\"\",\"\\\\\",\"\\b\\f\\n\\r\\t\"]");
  EXPECT_ROUND_TRIP("[\"\\uD83D\\uDE00\",\"\xE3\x81\x82\xE3\x81\x84\"]");
  EXPECT_ROUND_TRIP("{\"\\u30AD\\u30FC\":\"\\t\"}");

  // 深いネスト
  {
    static char deep[2048];
    size_t n = 0;
    for (int i = 0; i < 200; ++i) { deep[n++] = '['; }
    deep[n++] = '1';
    for (int i = 0; i < 200; ++i) { deep[n++] = ']'; }
    EXPECT_ROUND_TRIP(StringView(deep, n));
  }

  // 要素数の多いコンテナ
  {
    static char wide[32768];
    size_t n = 0;
    wide[n++] = '{';
    for (int i = 0; i < 400; ++i) {
      if (i != 0) { wide[n++] = ','; }
      n += static_cast<size_t>(std::snprintf(wide + n, sizeof(wide) - n,
                                             "\"k%03d\":[%d,%d.5,\"v%03d\"]", i, i, i, i));
    }
    wide[n++] = '}';
    EXPECT_ROUND_TRIP(StringView(wide, n));
  }
}

// ---------------------------------------------------------------------------
// 整形仕様(バイト列で固定)
// ---------------------------------------------------------------------------

static void Test_Formatting() {
  BeginCase("formatting: compact and pretty byte layouts are pinned");

  // compact: 余白を一切入れない
  EXPECT_COMPACT("{ \"a\" : 1 , \"b\" : [ 1 , 2 ] }", "{\"a\":1,\"b\":[1,2]}");
  EXPECT_COMPACT("[ ]", "[]");
  EXPECT_COMPACT("{ }", "{}");

  // pretty: スペース 2 / 改行 \n / ':' の後に空白 1 / 末尾改行なし
  EXPECT_PRETTY("{\"a\":1}", "{\n  \"a\": 1\n}");
  EXPECT_PRETTY("[1,2]",     "[\n  1,\n  2\n]");
  EXPECT_PRETTY("{\"a\":[1,2],\"b\":{\"c\":null}}",
                "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {\n    \"c\": null\n  }\n}");

  // 空コンテナは 1 行に潰す
  EXPECT_PRETTY("{}", "{}");
  EXPECT_PRETTY("[]", "[]");
  EXPECT_PRETTY("{\"a\":{},\"b\":[]}", "{\n  \"a\": {},\n  \"b\": []\n}");
  EXPECT_PRETTY("[[],[]]", "[\n  [],\n  []\n]");

  // ルート単独スカラは整形の影響を受けない
  EXPECT_PRETTY("1",       "1");
  EXPECT_PRETTY("\"x\"",   "\"x\"");
  EXPECT_PRETTY("null",    "null");

  // SetIndent
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    Document           document(&mock);
    CHECK(document.Parse(StringView("{\"a\":[1]}"), kNoParseFlags).IsOk());

    JsonStringBuffer buffer(arena);
    PrettyWriter     writer(buffer);
    writer.SetIndent('\t', 1);
    CHECK(GLFD::Json::WriteValue(writer, document.Root()));
    CHECK(writer.IsComplete());
    CHECK(buffer.View() == StringView("{\n\t\"a\": [\n\t\t1\n\t]\n}"));
  }
}

// ---------------------------------------------------------------------------
// T-15 エスケープ
// ---------------------------------------------------------------------------

static void Test_Escaping() {
  BeginCase("T-15: mandatory escapes, short forms, and '/' left alone");

  // 必須エスケープ。短縮形を使う
  EXPECT_COMPACT("\"a\\\"b\"",  "\"a\\\"b\"");
  EXPECT_COMPACT("\"a\\\\b\"",  "\"a\\\\b\"");
  EXPECT_COMPACT("\"\\b\"",     "\"\\b\"");
  EXPECT_COMPACT("\"\\f\"",     "\"\\f\"");
  EXPECT_COMPACT("\"\\n\"",     "\"\\n\"");
  EXPECT_COMPACT("\"\\r\"",     "\"\\r\"");
  EXPECT_COMPACT("\"\\t\"",     "\"\\t\"");

  // 短縮形の無い制御文字は \u00XX
  EXPECT_COMPACT("\"\\u0000\"", "\"\\u0000\"");
  EXPECT_COMPACT("\"\\u0001\"", "\"\\u0001\"");
  EXPECT_COMPACT("\"\\u001f\"", "\"\\u001f\"");
  EXPECT_COMPACT("\"\\u000b\"", "\"\\u000b\"");   // 垂直タブに短縮形は無い

  // '/' はエスケープしない
  EXPECT_COMPACT("\"a/b\"",   "\"a/b\"");
  EXPECT_COMPACT("\"\\/\"",   "\"/\"");

  // 既定では UTF-8 をそのまま透過する
  EXPECT_COMPACT("\"\xE3\x81\x82\"",          "\"\xE3\x81\x82\"");
  EXPECT_COMPACT("\"\\u3042\"",               "\"\xE3\x81\x82\"");
  EXPECT_COMPACT("\"\\uD83D\\uDE00\"",        "\"\xF0\x9F\x98\x80\"");

  // EscapeNonAscii: 非 ASCII を \uXXXX へ。BMP 外はサロゲートペアへ再分解する
  EXPECT_COMPACT_F("\"\xE3\x81\x82\"",   "\"\\u3042\"",       WriteFlags::EscapeNonAscii);
  EXPECT_COMPACT_F("\"\\u00e9\"",        "\"\\u00e9\"",       WriteFlags::EscapeNonAscii);
  EXPECT_COMPACT_F("\"\xF0\x9F\x98\x80\"", "\"\\ud83d\\ude00\"", WriteFlags::EscapeNonAscii);
  EXPECT_COMPACT_F("\"a\xE3\x81\x82" "b\"", "\"a\\u3042b\"",  WriteFlags::EscapeNonAscii);
  // ASCII 部分は素通しされる
  EXPECT_COMPACT_F("\"plain\"", "\"plain\"", WriteFlags::EscapeNonAscii);
  // 制御文字の扱いは既定と同じ
  EXPECT_COMPACT_F("\"\\n\xE3\x81\x82\"", "\"\\n\\u3042\"", WriteFlags::EscapeNonAscii);

  // キーにも同じ規則が効く
  EXPECT_COMPACT("{\"a\\nb\":1}", "{\"a\\nb\":1}");
  EXPECT_COMPACT_F("{\"\xE3\x81\x82\":1}", "{\"\\u3042\":1}", WriteFlags::EscapeNonAscii);

  // EscapeNonAscii で不正 UTF-8 に当たったらエラー(置換しない)
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);
    CompactWriter      writer(buffer, WriteFlags::EscapeNonAscii);

    const char broken[3] = { 'a', '\xE3', 'b' };   // 途中で切れた 3 バイト列
    CHECK(!writer.String(StringView(broken, 3)));
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  // 既定(透過)なら不正 UTF-8 でも素通しする(検証コストを払わない)
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);
    CompactWriter      writer(buffer);

    const char broken[3] = { 'a', '\xE3', 'b' };
    CHECK(writer.String(StringView(broken, 3)));
    CHECK(writer.Error() == ErrorCode::None);
    CHECK(buffer.View().Size() == 5);
  }
}

// ---------------------------------------------------------------------------
// T-16 NaN / Infinity
// ---------------------------------------------------------------------------

static void Test_NaNInfinity() {
  BeginCase("T-16: NaN / Infinity require AllowNaNInf and round-trip through the reader");

  const double nan      = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();

  // 既定ではエラー
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);

    struct Case { double value; };
    const Case cases[] = { { nan }, { infinity }, { -infinity } };
    for (const Case& testCase : cases) {
      JsonStringBuffer buffer(arena);
      CompactWriter    writer(buffer);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(!writer.Double(testCase.value));
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(writer.Error() == ErrorCode::WriteInvalidValue);
    }
  }

  // AllowNaNInf 指定時は出力され、AllowNaNInf 付きの Reader で読み戻せる
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);

    struct Case { double value; const char* text; };
    const Case cases[] = {
      { nan,       "NaN" },
      { infinity,  "Infinity" },
      { -infinity, "-Infinity" },
    };

    for (const Case& testCase : cases) {
      JsonStringBuffer buffer(arena);
      CompactWriter    writer(buffer, WriteFlags::AllowNaNInf);
      CHECK(writer.Double(testCase.value));
      CHECK(writer.IsComplete());
      CHECK(buffer.View() == StringView(testCase.text));

      // R1-18 のキーワード照合と往復すること
      JsonReader       reader(arena);
      JsonTraceHandler trace;
      CHECK(reader.Parse(buffer.View(), trace, ParseFlags::AllowNaNInf).IsOk());
      // AllowNaNInf 無しなら拒否されること
      JsonReader       strictReader(arena);
      JsonTraceHandler strictTrace;
      CHECK(strictReader.Parse(buffer.View(), strictTrace, kNoParseFlags).code
            == ErrorCode::NaNInfNotAllowed);
    }
  }

  // コンテナの中でも同じ
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);
    CompactWriter      writer(buffer, WriteFlags::AllowNaNInf);

    CHECK(writer.ArrayBegin());
    CHECK(writer.Double(1.5));
    CHECK(writer.Double(nan));
    CHECK(writer.Double(-infinity));
    CHECK(writer.ArrayEnd());
    CHECK(writer.IsComplete());
    CHECK(buffer.View() == StringView("[1.5,NaN,-Infinity]"));
  }
}

// ---------------------------------------------------------------------------
// T-17 誤用検出
// ---------------------------------------------------------------------------

static void Test_Misuse() {
  BeginCase("T-17: misuse is reported without crashing");

  MockMemoryResource mock;
  JsonArena          arena(&mock);

  // 未クローズのまま IsComplete() が false
  {
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.ObjectBegin());
    CHECK(writer.Key(StringView("a")));
    CHECK(writer.Int64(1));
    CHECK(!writer.IsComplete());          // } が無い
    CHECK(writer.Error() == ErrorCode::None);
  }
  // 何も書いていない状態も未完成
  {
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(!writer.IsComplete());
  }
  // 正常に閉じれば完成
  {
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.ObjectBegin());
    CHECK(writer.ObjectEnd());
    CHECK(writer.IsComplete());
  }

#ifdef NDEBUG
  // 誤用は Debug では assert で停止するため、Release でのみ ErrorCode を検証する (R1-14)
  {
    // 値を期待する位置での Key()
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.ObjectBegin());
    CHECK(writer.Key(StringView("a")));
    CHECK(!writer.Key(StringView("b")));           // キーの連続
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  {
    // キーを期待する位置での値
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.ObjectBegin());
    CHECK(!writer.Int64(1));
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  {
    // 配列の中での Key()
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.ArrayBegin());
    CHECK(!writer.Key(StringView("a")));
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  {
    // 閉じ過ぎ
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(!writer.ObjectEnd());
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  {
    // 括弧の不一致: { に ] を対応させる
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.ObjectBegin());
    CHECK(!writer.ArrayEnd());
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  {
    // キーの直後に閉じる
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.ObjectBegin());
    CHECK(writer.Key(StringView("a")));
    CHECK(!writer.ObjectEnd());
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  {
    // ルート値を 2 つ書く
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(writer.Int64(1));
    CHECK(!writer.Int64(2));
    CHECK(writer.Error() == ErrorCode::WriteInvalidValue);
  }
  {
    // 一度失敗したら以降の呼び出しも失敗し続ける
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    CHECK(!writer.ObjectEnd());
    CHECK(!writer.Int64(1));
    CHECK(!writer.ArrayBegin());
    CHECK(!writer.IsComplete());
  }
#endif
}

// ---------------------------------------------------------------------------
// T-18 深度
// ---------------------------------------------------------------------------

static void Test_Depth() {
  BeginCase("T-18: depth limit matches the reader so anything readable is writable");

  MockMemoryResource mock;
  JsonArena          arena(&mock);

  CHECK(CompactWriter::kMaxDepth == JsonReader::kDefaultMaxDepth);

  // 上限ちょうどまでは書ける
  {
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    bool             ok = true;
    for (std::uint32_t i = 0; i < CompactWriter::kMaxDepth; ++i) {
      if (!writer.ArrayBegin()) { ok = false; break; }
    }
    CHECK(ok);
    CHECK(writer.Int64(1));
    for (std::uint32_t i = 0; i < CompactWriter::kMaxDepth && ok; ++i) {
      if (!writer.ArrayEnd()) { ok = false; break; }
    }
    CHECK(ok);
    CHECK(writer.IsComplete());
    CHECK(writer.Error() == ErrorCode::None);
  }

  // 上限を 1 段超えると DepthLimitExceeded(クラッシュしない)
  {
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    for (std::uint32_t i = 0; i < CompactWriter::kMaxDepth; ++i) {
      CHECK_QUIET(writer.ArrayBegin());
    }
    ++GLFD::Test::g_checkCount;
    CHECK(!writer.ArrayBegin());
    CHECK(writer.Error() == ErrorCode::DepthLimitExceeded);
    CHECK(!writer.IsComplete());
  }

  // Reader の既定 MaxDepth で読める JSON は必ず書き戻せる
  {
    static char deep[1024];
    size_t n = 0;
    for (std::uint32_t i = 0; i < JsonReader::kDefaultMaxDepth; ++i) { deep[n++] = '['; }
    deep[n++] = '1';
    for (std::uint32_t i = 0; i < JsonReader::kDefaultMaxDepth; ++i) { deep[n++] = ']'; }

    Document document(&mock);
    CHECK(document.Parse(StringView(deep, n), kNoParseFlags).IsOk());

    JsonStringBuffer buffer(arena);
    CHECK(document.WriteTo(buffer));
    CHECK(buffer.View() == StringView(deep, n));
  }
}

// ---------------------------------------------------------------------------
// T-19 確保失敗(Writer 経路)
// ---------------------------------------------------------------------------

static void Test_AllocationFailure() {
  BeginCase("T-19: allocation failure surfaces as WriteStreamFailure, never a crash");

  const StringView json("{\"a\":[1,2,3],\"b\":{\"c\":\"x\\ny\"},\"d\":\"plain\"}");

  // 成功に必要な確保回数を測る
  int required = 0;
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    Document           document(&mock);
    CHECK(document.Parse(json, kNoParseFlags).IsOk());
    JsonStringBuffer buffer(arena);
    CHECK(document.WriteTo(buffer));
    required = mock.AllocateCalls();
  }

  for (int n = 0; n <= required + 2; ++n) {
    MockMemoryResource mock;
    {
      Document document(&mock);
      const bool parsed = document.Parse(json, kNoParseFlags).IsOk();

      JsonArena        arena(&mock);
      JsonStringBuffer buffer(arena);
      mock.SetFailAfter(n);

      const bool written = parsed && document.WriteTo(buffer);

      ++GLFD::Test::g_checkCount;
      if (!written && parsed) {
        // 失敗したならバッファ側にも失敗が記録されていること
        CHECK_QUIET(buffer.HasFailed());
      }
      mock.ClearFailure();
    }
    ++GLFD::Test::g_checkCount;
    if (mock.AllocateCalls() - mock.InjectedFailures() != mock.DeallocateCalls()) {
      char what[128];
      std::snprintf(what, sizeof(what), "N=%d: allocate/deallocate mismatch", n);
      Fail(__LINE__, what);
    }
    ++GLFD::Test::g_checkCount;
    CHECK_QUIET(mock.LiveBlockCount() == 0);
  }

  // Writer 単体でストリーム失敗が WriteStreamFailure になること
  {
    MockMemoryResource mock;
    mock.SetFailAfter(0);
    JsonArena        arena(&mock);
    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);

    CHECK(!writer.Int64(1));
    CHECK(writer.Error() == ErrorCode::WriteStreamFailure);
    CHECK(buffer.HasFailed());
  }
}

// ---------------------------------------------------------------------------
// T-21 SAX 直結(R1-15)
// ---------------------------------------------------------------------------

static void Test_SaxToWriter() {
  BeginCase("T-21: JsonReader output can drive JsonWriter without a DOM");

  MockMemoryResource mock;
  JsonArena          arena(&mock);

  // minify: 余白を落とすだけ。DOM を経由しない
  {
    const StringView source("{ \"a\" : [ 1 , 2 ] , \"b\" : { \"c\" : \"x\\ny\" } }");

    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    JsonWriterHandler<CompactWriter> handler(writer);

    JsonReader reader(arena);
    CHECK(reader.Parse(source, handler, kNoParseFlags).IsOk());
    CHECK(writer.IsComplete());
    CHECK(buffer.View() == StringView("{\"a\":[1,2],\"b\":{\"c\":\"x\\ny\"}}"));
  }

  // 整形: 同じ経路で pretty へ
  {
    const StringView source("{\"a\":[1,2]}");

    JsonStringBuffer buffer(arena);
    PrettyWriter     writer(buffer);
    JsonWriterHandler<PrettyWriter> handler(writer);

    JsonReader reader(arena);
    CHECK(reader.Parse(source, handler, kNoParseFlags).IsOk());
    CHECK(writer.IsComplete());
    CHECK(buffer.View() == StringView("{\n  \"a\": [\n    1,\n    2\n  ]\n}"));
  }

  // JSONC のコメントと末尾カンマが落ちること(コメントは保持しない方針)
  {
    const StringView source("/* c */{\"a\":1,/* c */\"b\":[1,2,],}// tail");

    JsonStringBuffer buffer(arena);
    CompactWriter    writer(buffer);
    JsonWriterHandler<CompactWriter> handler(writer);

    JsonReader reader(arena);
    CHECK(reader.Parse(source, handler, ParseFlags::JsonC).IsOk());
    CHECK(writer.IsComplete());
    CHECK(buffer.View() == StringView("{\"a\":1,\"b\":[1,2]}"));
  }

  // SAX 直結の結果が DOM 経由と一致すること
  {
    const StringView source("{\"a\":[1,{\"b\":[true,false,null]}],\"c\":\"\\u3042\"}");

    JsonStringBuffer direct(arena);
    CompactWriter    directWriter(direct);
    JsonWriterHandler<CompactWriter> handler(directWriter);
    JsonReader                       reader(arena);
    CHECK(reader.Parse(source, handler, kNoParseFlags).IsOk());

    Document document(&mock);
    CHECK(document.Parse(source, kNoParseFlags).IsOk());
    JsonStringBuffer viaDom(arena);
    CHECK(document.WriteTo(viaDom));

    CHECK(direct.View() == viaDom.View());
  }
}

// ---------------------------------------------------------------------------
// T-20 書き出し -> ParseFile による読み戻し(1-3 の成果との結合)
// ---------------------------------------------------------------------------

static void Test_FileRoundTrip() {
  BeginCase("T-20: WriteTo a file and read it back with ParseFile");

  // 一時ディレクトリを用意する
  static char root[1024];
  {
    wchar_t     wide[512];
    const DWORD length    = ::GetTempPathW(512, wide);
    char        utf8[1024];
    const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                  utf8, sizeof(utf8), nullptr, nullptr);
    std::snprintf(root, sizeof(root), "%.*sglfd_json_writer",
                  (converted > 0 ? converted : 0), utf8);
    wchar_t wideRoot[1024];
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, wideRoot, 1024) > 0) {
      (void)::CreateDirectoryW(wideRoot, nullptr);
    }
  }

  char path[1024];
  std::snprintf(path, sizeof(path), "%s\\roundtrip.json", root);

  // 非 ASCII はバイト配列で組み立てる (C5297)。"名" と "値"
  static const unsigned char kNameUtf8[]  = { 0xE5, 0x90, 0x8D, 0x00 };
  static const unsigned char kValueUtf8[] = { 0xE5, 0x80, 0xA4, 0x00 };

  static char source[512];
  std::snprintf(source, sizeof(source),
                "{\"%s\":\"%s\",\"n\":[1,2.5,-0.0,true,null],\"esc\":\"a\\nb\\tc\"}",
                reinterpret_cast<const char*>(kNameUtf8),
                reinterpret_cast<const char*>(kValueUtf8));

  MockMemoryResource mock;

  for (int pretty = 0; pretty < 2; ++pretty) {
    // (1) パースしてファイルへ書き出す
    {
      Document document(&mock);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(document.Parse(StringView(source), kNoParseFlags).IsOk());

      JsonArena      arena(&mock);
      JsonFileStream stream(path, arena);
      ++GLFD::Test::g_checkCount;
      if (!stream.IsOpen()) {
        Fail(__LINE__, "failed to open the output file");
        return;
      }

      const bool written = (pretty != 0) ? document.WritePrettyTo(stream)
                                         : document.WriteTo(stream);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(written);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(stream.Finish());
    }

    // (2) ParseFile で読み戻し、SAX イベント列が元と一致すること
    {
      MockMemoryResource traceMock;
      JsonArena          arena(&traceMock);

      JsonReader       sourceReader(arena);
      JsonTraceHandler sourceTrace;
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(sourceReader.Parse(StringView(source), sourceTrace, kNoParseFlags).IsOk());

      Document reloaded(&traceMock);
      const ParseError error = reloaded.ParseFile(path, kNoParseFlags);
      ++GLFD::Test::g_checkCount;
      if (!error.IsOk()) {
        char what[192];
        std::snprintf(what, sizeof(what), "ParseFile failed: %s", NameOf(error.code));
        Fail(__LINE__, what);
        continue;
      }

      // 読み戻した DOM をもう一度 compact で書き、元の compact 出力と比べる
      JsonStringBuffer reloadedBuffer(arena);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(reloaded.WriteTo(reloadedBuffer));

      Document original(&traceMock);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(original.Parse(StringView(source), kNoParseFlags).IsOk());
      JsonStringBuffer originalBuffer(arena);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(original.WriteTo(originalBuffer));

      ++GLFD::Test::g_checkCount;
      if (reloadedBuffer.View() != originalBuffer.View()) {
        FailDetail(__LINE__, "file round-trip changed the document",
                   originalBuffer.View(), reloadedBuffer.View());
      }
    }
  }

  // 後始末
  {
    wchar_t wide[1024];
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024) > 0) {
      ::DeleteFileW(wide);
    }
  }
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("JsonWriter");

  Test_DoubleRoundTrip();
  Test_DocumentRoundTrip();
  Test_Formatting();
  Test_Escaping();
  Test_NaNInfinity();
  Test_Misuse();
  Test_Depth();
  Test_AllocationFailure();
  Test_SaxToWriter();
  Test_FileRoundTrip();

  return Summarize();
}
