/**
 * @file  JsonDocumentTests.cpp
 * @brief DOM ビルダ / Document の単体テスト
 *        (フェーズ 1-3 / 要件 T-8, T-9, T-10, T-13, T-14)
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#include "TestHarness.h"
#include "MockMemoryResource.h"
#include "JsonTraceHandler.h"
#include "Core/Json/JsonDocument.h"
#include "Core/Json/JsonFileIO.h"

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
using GLFD::Json::JsonReader;
using GLFD::Json::Member;
using GLFD::Json::ParseError;
using GLFD::Json::ParseFlags;
using GLFD::Json::ReadEntireFile;
using GLFD::Json::ToString;
using GLFD::Json::Value;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::JsonTraceHandler;
using GLFD::Test::MockMemoryResource;
using GLFD::Test::Summarize;

namespace {

  constexpr ParseFlags kNone  = ParseFlags::None;
  constexpr ParseFlags kJsonC = ParseFlags::JsonC;

  void Fail(int line, const char* message) {
    std::printf("    [FAIL] %s(%d): %s\n", __FILE__, line, message);
    ++GLFD::Test::g_failureCount;
  }

  const char* FormatError(const ParseError& error, char* buffer, size_t capacity) {
    const StringView name = ToString(error.code);
    std::snprintf(buffer, capacity, "%.*s@%zu",
                  static_cast<int>(name.Size()), name.Data(), error.offset);
    return buffer;
  }

  // -------------------------------------------------------------------------
  // T-8: DOM を走査して SAX イベント列を再生成する
  // -------------------------------------------------------------------------

  /// DOM を深さ優先で辿り、JsonTraceHandler へ SAX と同じイベント列を流す
  bool ReplayValue(const Value& value, JsonTraceHandler& handler) {
    switch (value.Type()) {
    case GLFD::Json::ValueType::Null:   return handler.OnNull();
    case GLFD::Json::ValueType::Bool:   return handler.OnBool(value.GetBool());
    case GLFD::Json::ValueType::Int64:  return handler.OnInt64(value.GetInt64());
    case GLFD::Json::ValueType::UInt64: return handler.OnUInt64(value.GetUInt64());
    case GLFD::Json::ValueType::Double: return handler.OnDouble(value.GetDouble());

    case GLFD::Json::ValueType::String:
      // DOM 上の文字列はすべてアリーナ所有なので needsCopy 相当は false。
      // SAX 直結の結果と突き合わせるため、トレース表記を合わせる必要がある
      return handler.OnString(value.GetString(), false);

    case GLFD::Json::ValueType::Array: {
      if (!handler.OnArrayBegin()) { return false; }
      for (const Value* it = value.Begin(); it != value.End(); ++it) {
        if (!ReplayValue(*it, handler)) { return false; }
      }
      return handler.OnArrayEnd(value.Size());
    }

    case GLFD::Json::ValueType::Object: {
      if (!handler.OnObjectBegin()) { return false; }
      for (const Member* it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        if (!handler.OnKey(it->key, false)) { return false; }
        if (!ReplayValue(it->value, handler)) { return false; }
      }
      return handler.OnObjectEnd(value.MemberCount());
    }

    default:
      return false;
    }
  }

  /**
   * @brief SAX 直結の結果と DOM 再生成の結果を突き合わせる
   * @note  文字列の needsCopy 印(`*`)だけは両者で必ず異なる(SAX は入力直指しの
   *        ことがあり、DOM は常にアリーナ所有)ため、比較前に印を取り除いて正規化する
   */
  void NormalizeTrace(StringView trace, char* out, size_t capacity, size_t& outLength) {
    size_t written = 0;
    for (size_t i = 0; i < trace.Size(); ++i) {
      const char c = trace[i];
      // "s*(" / "k*(" の '*' を落とす
      if (c == '*' && written > 0 && (out[written - 1] == 's' || out[written - 1] == 'k')
          && (i + 1) < trace.Size() && trace[i + 1] == '(') {
        continue;
      }
      if (written < capacity) {
        out[written++] = c;
      }
    }
    outLength = written;
  }

  void ExpectDomMatchesSaxImpl(int line, StringView json, ParseFlags flags) {
    ++GLFD::Test::g_checkCount;

    // (1) SAX に直接流す
    MockMemoryResource saxMock;
    JsonArena          saxArena(&saxMock);
    JsonReader         reader(saxArena);
    JsonTraceHandler   saxTrace;
    const ParseError   saxError = reader.Parse(json, saxTrace, flags);

    // (2) DOM を構築し、走査してイベント列を再生成する
    MockMemoryResource domMock;
    Document           document(&domMock);
    const ParseError   domError = document.Parse(json, flags);

    char scratch[128];
    if (saxError.IsOk() != domError.IsOk()) {
      Fail(line, "SAX and DOM disagree on success/failure");
      std::printf("             sax=%s dom=%s\n",
                  FormatError(saxError, scratch, sizeof(scratch)),
                  ToString(domError.code).Data());
      return;
    }
    if (!saxError.IsOk()) {
      return;   // 双方エラーなら比較対象なし
    }

    JsonTraceHandler domTrace;
    if (!ReplayValue(document.Root(), domTrace)) {
      Fail(line, "failed to replay the DOM");
      return;
    }

    char   saxNormalized[8192];
    char   domNormalized[8192];
    size_t saxLength = 0;
    size_t domLength = 0;
    NormalizeTrace(saxTrace.Trace(), saxNormalized, sizeof(saxNormalized), saxLength);
    NormalizeTrace(domTrace.Trace(), domNormalized, sizeof(domNormalized), domLength);

    if (StringView(saxNormalized, saxLength) != StringView(domNormalized, domLength)) {
      std::printf("    [FAIL] %s(%d): DOM lost information relative to SAX\n"
                  "             sax: %.*s\n"
                  "             dom: %.*s\n"
                  "             input: %.*s\n",
                  __FILE__, line,
                  static_cast<int>(saxLength < 400 ? saxLength : 400), saxNormalized,
                  static_cast<int>(domLength < 400 ? domLength : 400), domNormalized,
                  static_cast<int>(json.Size() < 200 ? json.Size() : 200), json.Data());
      ++GLFD::Test::g_failureCount;
    }
  }

  #define EXPECT_DOM_MATCHES_SAX(json, flags) ExpectDomMatchesSaxImpl(__LINE__, (json), (flags))

  // -------------------------------------------------------------------------
  // 一時ファイル(日本語パスを含む)
  // -------------------------------------------------------------------------

  /// UTF-8 のパスへ内容を書き出す。テストの前提を作るためだけのヘルパ
  bool WriteFileUtf8Path(const char* utf8Path, const void* data, size_t size) {
    wchar_t wide[1024];
    const int converted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                utf8Path, -1, wide, 1024);
    if (converted <= 0) {
      return false;
    }
    const HANDLE handle = ::CreateFileW(wide, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }
    bool ok = true;
    if (size != 0) {
      DWORD written = 0;
      ok = (::WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr) != 0)
        && (written == size);
    }
    ::CloseHandle(handle);
    return ok;
  }

  bool CreateDirectoryUtf8Path(const char* utf8Path) {
    wchar_t wide[1024];
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, wide, 1024) <= 0) {
      return false;
    }
    return (::CreateDirectoryW(wide, nullptr) != 0) || (::GetLastError() == ERROR_ALREADY_EXISTS);
  }

  void DeleteFileUtf8Path(const char* utf8Path) {
    wchar_t wide[1024];
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, wide, 1024) > 0) {
      ::DeleteFileW(wide);
    }
  }

  /// テスト用の一時ディレクトリ(UTF-8)。末尾に区切りは付けない
  const char* TempRoot() {
    static char root[1024];
    static bool initialized = false;
    if (!initialized) {
      wchar_t wide[512];
      const DWORD length = ::GetTempPathW(512, wide);
      char utf8[1024];
      const int converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                  utf8, sizeof(utf8), nullptr, nullptr);
      std::snprintf(root, sizeof(root), "%.*sglfd_json_tests",
                    (converted > 0 ? converted : 0), utf8);
      (void)CreateDirectoryUtf8Path(root);
      initialized = true;
    }
    return root;
  }

}

// ---------------------------------------------------------------------------
// T-8 SAX / DOM 等価性
// ---------------------------------------------------------------------------

static void Test_DomMatchesSax() {
  BeginCase("T-8: DOM replay reproduces the SAX event stream exactly");

  // ルート単独スカラ
  EXPECT_DOM_MATCHES_SAX("null",   kNone);
  EXPECT_DOM_MATCHES_SAX("true",   kNone);
  EXPECT_DOM_MATCHES_SAX("false",  kNone);
  EXPECT_DOM_MATCHES_SAX("0",      kNone);
  EXPECT_DOM_MATCHES_SAX("-123",   kNone);
  EXPECT_DOM_MATCHES_SAX("18446744073709551615", kNone);
  EXPECT_DOM_MATCHES_SAX("1.5e-3", kNone);
  EXPECT_DOM_MATCHES_SAX("\"scalar\"", kNone);

  // 空コンテナ
  EXPECT_DOM_MATCHES_SAX("{}",       kNone);
  EXPECT_DOM_MATCHES_SAX("[]",       kNone);
  EXPECT_DOM_MATCHES_SAX("[[],[]]",  kNone);
  EXPECT_DOM_MATCHES_SAX("{\"a\":{},\"b\":[]}", kNone);

  // 基本構造
  EXPECT_DOM_MATCHES_SAX("[1,2,3]", kNone);
  EXPECT_DOM_MATCHES_SAX("{\"a\":1,\"b\":\"x\",\"c\":null}", kNone);
  EXPECT_DOM_MATCHES_SAX("{\"a\":[1,{\"b\":[true,false]}]}", kNone);

  // キー重複(R2-3: 全件保持されることの確認でもある)
  EXPECT_DOM_MATCHES_SAX("{\"k\":1,\"k\":2,\"k\":3}", kNone);
  EXPECT_DOM_MATCHES_SAX("{\"a\":1,\"b\":2,\"a\":3,\"b\":4}", kNone);

  // エスケープを含む文字列 / 含まない文字列の両方
  EXPECT_DOM_MATCHES_SAX("{\"plain\":\"abc\",\"esc\":\"a\\nb\\u0041\"}", kNone);
  EXPECT_DOM_MATCHES_SAX("[\"\\uD83D\\uDE00\",\"\xE3\x81\x82\"]", kNone);
  EXPECT_DOM_MATCHES_SAX("{\"\\u30AD\\u30FC\":\"\\t\"}", kNone);

  // JSONC
  EXPECT_DOM_MATCHES_SAX("/*c*/{\"a\":1,/*c*/\"b\":[1,2,],}//c", kJsonC);

  // 深いネスト(構築スタックの伸長経路を踏む)
  {
    static char deep[4096];
    static char expected[4096];
    (void)expected;
    size_t n = 0;
    for (int i = 0; i < 200; ++i) { deep[n++] = '['; }
    deep[n++] = '1';
    for (int i = 0; i < 200; ++i) { deep[n++] = ']'; }
    EXPECT_DOM_MATCHES_SAX(StringView(deep, n), kNone);
  }

  // 要素数が多い配列 / オブジェクト(構築スタックが何度も伸長する)
  {
    static char wide[65536];
    size_t n = 0;
    wide[n++] = '[';
    for (int i = 0; i < 2000; ++i) {
      if (i != 0) { wide[n++] = ','; }
      n += static_cast<size_t>(std::snprintf(wide + n, sizeof(wide) - n, "%d", i));
    }
    wide[n++] = ']';
    EXPECT_DOM_MATCHES_SAX(StringView(wide, n), kNone);
  }
  {
    static char wide[65536];
    size_t n = 0;
    wide[n++] = '{';
    for (int i = 0; i < 1000; ++i) {
      if (i != 0) { wide[n++] = ','; }
      n += static_cast<size_t>(std::snprintf(wide + n, sizeof(wide) - n,
                                             "\"k%04d\":%d", i, i));
    }
    wide[n++] = '}';
    EXPECT_DOM_MATCHES_SAX(StringView(wide, n), kNone);
  }

  // 配列とオブジェクトが交互に入れ子になる(2 本のスタックが交錯する経路)
  {
    static char mixed[8192];
    size_t n = 0;
    for (int i = 0; i < 40; ++i) {
      n += static_cast<size_t>(std::snprintf(mixed + n, sizeof(mixed) - n, "{\"a\":[9,"));
    }
    n += static_cast<size_t>(std::snprintf(mixed + n, sizeof(mixed) - n, "0"));
    for (int i = 0; i < 40; ++i) {
      n += static_cast<size_t>(std::snprintf(mixed + n, sizeof(mixed) - n, "]}"));
    }
    EXPECT_DOM_MATCHES_SAX(StringView(mixed, n), kNone);
  }
}

// ---------------------------------------------------------------------------
// T-9 入力バッファ寿命
// ---------------------------------------------------------------------------

static void Test_InputBufferLifetime() {
  BeginCase("T-9: DOM strings survive destruction of the input buffer");

  // エスケープあり / なしの文字列を両方含める
  const char* source =
    "{"
      "\"plainKey\":\"plainValue\","
      "\"esc\\u0041Key\":\"escaped\\nvalue\","
      "\"nested\":[\"a\",\"b\\tc\",{\"deep\":\"d\\u3042e\"}],"
      "\"utf8\":\"\xE3\x81\x82\xE3\x81\x84\""
    "}";

  const size_t length = std::strlen(source);

  // ヒープ上のバッファへ複製してからパースする
  auto* buffer = static_cast<char*>(std::malloc(length + 1));
  if (buffer == nullptr) {
    Fail(__LINE__, "malloc failed");
    return;
  }
  std::memcpy(buffer, source, length + 1);

  MockMemoryResource mock;
  Document           document(&mock);

  const ParseError error = document.Parse(StringView(buffer, length), kNone);
  ++GLFD::Test::g_checkCount;
  if (!error.IsOk()) {
    char scratch[128];
    Fail(__LINE__, "parse failed");
    std::printf("             %s\n", FormatError(error, scratch, sizeof(scratch)));
    std::free(buffer);
    return;
  }

  // **入力バッファを破壊する** — needsCopy の扱いを誤っていればここで壊れる
  std::memset(buffer, 0xCC, length);
  std::free(buffer);
  buffer = nullptr;

  const Value& root = document.Root();
  ++GLFD::Test::g_checkCount;
  CHECK(root.IsObject());
  ++GLFD::Test::g_checkCount;
  CHECK(root.MemberCount() == 4u);

  ++GLFD::Test::g_checkCount;
  CHECK(root.Has(StringView("plainKey")));                       // キーもコピーされている
  ++GLFD::Test::g_checkCount;
  CHECK(root[StringView("plainKey")].GetString() == StringView("plainValue"));

  ++GLFD::Test::g_checkCount;
  CHECK(root.Has(StringView("escAKey")));                        // \u0041 -> 'A'
  ++GLFD::Test::g_checkCount;
  CHECK(root[StringView("escAKey")].GetString() == StringView("escaped\nvalue"));

  const Value& nested = root[StringView("nested")];
  ++GLFD::Test::g_checkCount;
  CHECK(nested.IsArray() && nested.Size() == 3u);
  ++GLFD::Test::g_checkCount;
  CHECK(nested[0u].GetString() == StringView("a"));
  ++GLFD::Test::g_checkCount;
  CHECK(nested[1u].GetString() == StringView("b\tc"));
  ++GLFD::Test::g_checkCount;
  CHECK(nested[2u][StringView("deep")].GetString()
        == StringView("d\xE3\x81\x82" "e"));

  ++GLFD::Test::g_checkCount;
  CHECK(root[StringView("utf8")].GetString()
        == StringView("\xE3\x81\x82\xE3\x81\x84"));

  // 破壊したバッファの中身が紛れ込んでいないこと
  bool clean = true;
  for (const Member* it = root.MemberBegin(); it != root.MemberEnd(); ++it) {
    for (size_t i = 0; i < it->key.Size(); ++i) {
      if (static_cast<unsigned char>(it->key[i]) == 0xCCu) { clean = false; }
    }
  }
  ++GLFD::Test::g_checkCount;
  CHECK(clean);
}

// ---------------------------------------------------------------------------
// T-10 確保失敗の網羅注入
// ---------------------------------------------------------------------------

static void Test_ExhaustiveAllocationFailure() {
  BeginCase("T-10: failing the Nth allocation never crashes and never exposes a partial DOM");

  static const char* const kSources[] = {
    "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"x\\ny\"},\"e\":null}",
    "[[[[[[[[[[1,2,3]]]]]]]]]]",
    "{\"k\":\"\\uD83D\\uDE00\",\"dup\":1,\"dup\":2,\"arr\":[{},[],\"\"]}",
  };

  for (const char* source : kSources) {
    const StringView json(source);

    // まず成功に必要な確保回数を測る
    int required = 0;
    {
      MockMemoryResource mock;
      Document           document(&mock);
      const ParseError   error = document.Parse(json, kNone);
      ++GLFD::Test::g_checkCount;
      CHECK(error.IsOk());
      required = mock.AllocateCalls();
    }

    // N = 0 から「必要回数 + 余裕」まで総当たりで失敗させる
    for (int n = 0; n <= required + 2; ++n) {
      MockMemoryResource mock;
      mock.SetFailAfter(n);

      {
        Document         document(&mock);
        const ParseError error = document.Parse(json, kNone);

        ++GLFD::Test::g_checkCount;
        if (!error.IsOk()) {
          // 失敗したなら OutOfMemory であること
          if (error.code != ErrorCode::OutOfMemory) {
            char what[192];
            char scratch[128];
            std::snprintf(what, sizeof(what),
                          "N=%d: expected OutOfMemory, got %s", n,
                          FormatError(error, scratch, sizeof(scratch)));
            Fail(__LINE__, what);
          }

          // R2-7: 部分構築された DOM を露出しない
          ++GLFD::Test::g_checkCount;
          if (!document.Root().IsNull()) {
            char what[128];
            std::snprintf(what, sizeof(what), "N=%d: Root() must be Null after failure", n);
            Fail(__LINE__, what);
          }

          ++GLFD::Test::g_checkCount;
          CHECK(document.HasError());
        }
        else {
          // 成功したなら DOM は完全であること
          ++GLFD::Test::g_checkCount;
          CHECK(!document.Root().IsNull());
        }
      }
      // デストラクタが安全に走り、確保と解放が一致すること
      ++GLFD::Test::g_checkCount;
      if (mock.AllocateCalls() - mock.InjectedFailures() != mock.DeallocateCalls()) {
        char what[128];
        std::snprintf(what, sizeof(what), "N=%d: allocate/deallocate mismatch", n);
        Fail(__LINE__, what);
      }
      ++GLFD::Test::g_checkCount;
      CHECK(mock.LiveBlockCount() == 0);
      ++GLFD::Test::g_checkCount;
      CHECK(!mock.DoubleFree() && !mock.SizeMismatch() && !mock.AlignmentMismatch());
    }
  }
}

// ---------------------------------------------------------------------------
// T-13 構造
// ---------------------------------------------------------------------------

static void Test_Structure() {
  BeginCase("T-13: containers, duplicate keys, and wide objects");

  MockMemoryResource mock;
  Document           document(&mock);

  // 空コンテナ
  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(StringView("{}"), kNone).IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root().IsObject() && document.Root().MemberCount() == 0u);

  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(StringView("[]"), kNone).IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root().IsArray() && document.Root().Size() == 0u);

  // ルート単独スカラ
  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(StringView("42"), kNone).IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root().IsInt64() && document.Root().GetInt64() == 42);

  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(StringView("\"root\""), kNone).IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root().GetString() == StringView("root"));

  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(StringView("null"), kNone).IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root().IsNull() && !document.HasError());

  // R2-3: キー重複は全件保持、Find は最初の一致
  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(StringView("{\"k\":1,\"k\":2,\"k\":3}"), kNone).IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root().MemberCount() == 3u);
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root()[StringView("k")].GetInt64() == 1);
  {
    std::int64_t sum = 0;
    for (const Member* it = document.Root().MemberBegin();
         it != document.Root().MemberEnd(); ++it) {
      sum += it->value.GetInt64();
    }
    ++GLFD::Test::g_checkCount;
    CHECK(sum == 6);                        // 3 件すべてが残っている
  }

  // メンバ数の多いオブジェクト: 先頭 / 中央 / 末尾 / 不在
  {
    static char wide[65536];
    size_t n = 0;
    wide[n++] = '{';
    for (int i = 0; i < 500; ++i) {
      if (i != 0) { wide[n++] = ','; }
      n += static_cast<size_t>(std::snprintf(wide + n, sizeof(wide) - n,
                                             "\"m%03d\":%d", i, i));
    }
    wide[n++] = '}';

    ++GLFD::Test::g_checkCount;
    CHECK(document.Parse(StringView(wide, n), kNone).IsOk());
    const Value& root = document.Root();
    ++GLFD::Test::g_checkCount;
    CHECK(root.MemberCount() == 500u);
    ++GLFD::Test::g_checkCount;
    CHECK(root[StringView("m000")].GetInt64() == 0);      // 先頭
    ++GLFD::Test::g_checkCount;
    CHECK(root[StringView("m250")].GetInt64() == 250);    // 中央
    ++GLFD::Test::g_checkCount;
    CHECK(root[StringView("m499")].GetInt64() == 499);    // 末尾
    ++GLFD::Test::g_checkCount;
    CHECK(root.Find(StringView("m500")) == nullptr);      // 不在
  }

  // 深いネスト(構築スタックのインライン容量を超える)
  {
    static char deep[4096];
    size_t n = 0;
    for (int i = 0; i < 250; ++i) { deep[n++] = '['; }
    n += static_cast<size_t>(std::snprintf(deep + n, sizeof(deep) - n, "7"));
    for (int i = 0; i < 250; ++i) { deep[n++] = ']'; }

    ++GLFD::Test::g_checkCount;
    CHECK(document.Parse(StringView(deep, n), kNone).IsOk());

    const Value* cursor = &document.Root();
    int depth = 0;
    while (cursor->IsArray() && cursor->Size() == 1u) {
      cursor = &(*cursor)[0u];
      ++depth;
    }
    ++GLFD::Test::g_checkCount;
    CHECK(depth == 250);
    ++GLFD::Test::g_checkCount;
    CHECK(cursor->GetInt64() == 7);
  }

  // パース失敗時は Root() が Null (R2-7)
  {
    ++GLFD::Test::g_checkCount;
    CHECK(document.Parse(StringView("{\"a\":1"), kNone).IsOk() == false);
    ++GLFD::Test::g_checkCount;
    CHECK(document.Root().IsNull());
    ++GLFD::Test::g_checkCount;
    CHECK(document.HasError());
    ++GLFD::Test::g_checkCount;
    CHECK(document.Error().code == ErrorCode::ObjectMissingCommaOrBrace);
  }

  // 失敗のあとで成功すればエラーが解消すること
  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(StringView("[1]"), kNone).IsOk());
  ++GLFD::Test::g_checkCount;
  CHECK(!document.HasError());
  ++GLFD::Test::g_checkCount;
  CHECK(document.Error().code == ErrorCode::None);
}

static void Test_ClearAndReuse() {
  BeginCase("T-13: Clear() rewinds without asking the resource for more memory");

  MockMemoryResource mock;
  Document           document(&mock);

  const StringView json("{\"a\":[1,2,3],\"b\":{\"c\":\"x\\ny\"},\"d\":\"plain\"}");

  ++GLFD::Test::g_checkCount;
  CHECK(document.Parse(json, kNone).IsOk());
  const int callsAfterFirst = mock.AllocateCalls();
  ++GLFD::Test::g_checkCount;
  CHECK(callsAfterFirst > 0);

  document.Clear();
  ++GLFD::Test::g_checkCount;
  CHECK(document.Root().IsNull());
  ++GLFD::Test::g_checkCount;
  CHECK(!document.HasError());
  ++GLFD::Test::g_checkCount;
  CHECK(mock.DeallocateCalls() == 0);              // Reset なので返却しない
  ++GLFD::Test::g_checkCount;
  CHECK(mock.AllocateCalls() == callsAfterFirst);  // Clear 自体は確保しない

  // 同じ入力を繰り返しても追加確保が発生しない
  for (int i = 0; i < 10; ++i) {
    ++GLFD::Test::g_checkCount;
    CHECK(document.Parse(json, kNone).IsOk());
    ++GLFD::Test::g_checkCount;
    CHECK(document.Root()[StringView("a")].Size() == 3u);
    ++GLFD::Test::g_checkCount;
    CHECK(document.Root()[StringView("b")][StringView("c")].GetString()
          == StringView("x\ny"));
  }
  ++GLFD::Test::g_checkCount;
  CHECK(mock.AllocateCalls() == callsAfterFirst);

  // Clear を挟んでも同じ
  for (int i = 0; i < 5; ++i) {
    document.Clear();
    ++GLFD::Test::g_checkCount;
    CHECK(document.Parse(json, kNone).IsOk());
  }
  ++GLFD::Test::g_checkCount;
  CHECK(mock.AllocateCalls() == callsAfterFirst);
}

static void Test_Move() {
  BeginCase("T-13: Document is movable and the moved-to instance owns the DOM");

  MockMemoryResource mock;
  {
    Document source(&mock);
    ++GLFD::Test::g_checkCount;
    CHECK(source.Parse(StringView("{\"a\":\"moved\\nvalue\"}"), kNone).IsOk());

    Document target(static_cast<Document&&>(source));
    ++GLFD::Test::g_checkCount;
    CHECK(target.Root().IsObject());
    ++GLFD::Test::g_checkCount;
    CHECK(target.Root()[StringView("a")].GetString() == StringView("moved\nvalue"));

    // ムーブ代入
    Document other(&mock);
    ++GLFD::Test::g_checkCount;
    CHECK(other.Parse(StringView("[1,2]"), kNone).IsOk());
    other = static_cast<Document&&>(target);
    ++GLFD::Test::g_checkCount;
    CHECK(other.Root()[StringView("a")].GetString() == StringView("moved\nvalue"));
  }

  ++GLFD::Test::g_checkCount;
  CHECK(mock.AllocateCalls() == mock.DeallocateCalls());
  ++GLFD::Test::g_checkCount;
  CHECK(mock.LiveBlockCount() == 0);
  ++GLFD::Test::g_checkCount;
  CHECK(!mock.DoubleFree());
}

// ---------------------------------------------------------------------------
// T-14 ParseFile
// ---------------------------------------------------------------------------

static void Test_ParseFile() {
  BeginCase("T-14: ParseFile reads UTF-8 paths, including Japanese directories");

  const char* root = TempRoot();

  char asciiPath[1024];
  char japanesePath[1024];
  char japaneseDir[1024];
  char emptyPath[1024];
  char missingPath[1024];

  std::snprintf(asciiPath, sizeof(asciiPath), "%s\\plain.json", root);

  // 非 ASCII を文字列リテラルへ直接埋めると、コンパイラが実行文字セット (cp932) へ
  // 再エンコードしようとして C5297 が出る。ここで確かめたいのは
  // 「この UTF-8 バイト列をパスとして開けるか」なので、バイト配列で直接組み立てる。
  //   kDirNameUtf8  = "テストフォルダ"
  //   kFileNameUtf8 = "設定ファイル.json"
  //   kNameUtf8     = "ゲーム"
  static const unsigned char kDirNameUtf8[] = {
    0xE3, 0x83, 0x86, 0xE3, 0x82, 0xB9, 0xE3, 0x83, 0x88,
    0xE3, 0x83, 0x95, 0xE3, 0x82, 0xA9, 0xE3, 0x83, 0xAB, 0xE3, 0x83, 0x80, 0x00
  };
  static const unsigned char kFileNameUtf8[] = {
    0xE8, 0xA8, 0xAD, 0xE5, 0xAE, 0x9A,
    0xE3, 0x83, 0x95, 0xE3, 0x82, 0xA1, 0xE3, 0x82, 0xA4, 0xE3, 0x83, 0xAB,
    '.', 'j', 's', 'o', 'n', 0x00
  };
  static const unsigned char kNameUtf8[] = {
    0xE3, 0x82, 0xB2, 0xE3, 0x83, 0xBC, 0xE3, 0x83, 0xA0, 0x00
  };

  std::snprintf(japaneseDir, sizeof(japaneseDir), "%s\\%s",
                root, reinterpret_cast<const char*>(kDirNameUtf8));
  std::snprintf(japanesePath, sizeof(japanesePath), "%s\\%s",
                japaneseDir, reinterpret_cast<const char*>(kFileNameUtf8));
  std::snprintf(emptyPath, sizeof(emptyPath), "%s\\empty.json", root);
  std::snprintf(missingPath, sizeof(missingPath), "%s\\does_not_exist.json", root);

  static char contentBuffer[128];
  std::snprintf(contentBuffer, sizeof(contentBuffer),
                "{\"name\":\"%s\",\"hp\":100,\"tags\":[\"a\",\"b\\nc\"]}",
                reinterpret_cast<const char*>(kNameUtf8));
  const char* const content = contentBuffer;

  ++GLFD::Test::g_checkCount;
  CHECK(WriteFileUtf8Path(asciiPath, content, std::strlen(content)));
  ++GLFD::Test::g_checkCount;
  CHECK(CreateDirectoryUtf8Path(japaneseDir));
  ++GLFD::Test::g_checkCount;
  CHECK(WriteFileUtf8Path(japanesePath, content, std::strlen(content)));
  ++GLFD::Test::g_checkCount;
  CHECK(WriteFileUtf8Path(emptyPath, "", 0));

  MockMemoryResource mock;

  // --- 実在ファイル(ASCII パス)---
  {
    Document         document(&mock);
    const ParseError error = document.ParseFile(asciiPath, kNone);
    ++GLFD::Test::g_checkCount;
    if (!error.IsOk()) {
      char scratch[128];
      Fail(__LINE__, "ParseFile(ascii) failed");
      std::printf("             %s  path=%s\n",
                  FormatError(error, scratch, sizeof(scratch)), asciiPath);
    }
    else {
      ++GLFD::Test::g_checkCount;
      CHECK(document.Root()[StringView("hp")].GetInt64() == 100);
      ++GLFD::Test::g_checkCount;
      CHECK(document.Root()[StringView("tags")].Size() == 2u);
      // ハンドルを閉じた後も文字列が有効であること
      ++GLFD::Test::g_checkCount;
      CHECK(document.Root()[StringView("name")].GetString()
            == StringView(reinterpret_cast<const char*>(kNameUtf8), 9));
      ++GLFD::Test::g_checkCount;
      CHECK(document.Root()[StringView("tags")][1u].GetString() == StringView("b\nc"));
    }
  }

  // --- 日本語ディレクトリ + 日本語ファイル名(CreateFileA では開けない)---
  {
    Document         document(&mock);
    const ParseError error = document.ParseFile(japanesePath, kNone);
    ++GLFD::Test::g_checkCount;
    if (!error.IsOk()) {
      char scratch[128];
      Fail(__LINE__, "ParseFile(japanese path) failed");
      std::printf("             %s  path=%s\n",
                  FormatError(error, scratch, sizeof(scratch)), japanesePath);
    }
    else {
      ++GLFD::Test::g_checkCount;
      CHECK(document.Root()[StringView("hp")].GetInt64() == 100);
    }
  }

  // --- 存在しないパス ---
  {
    Document         document(&mock);
    const ParseError error = document.ParseFile(missingPath, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error.code == ErrorCode::FileNotFound);
    ++GLFD::Test::g_checkCount;
    CHECK(error.offset == 0);
    ++GLFD::Test::g_checkCount;
    CHECK(document.Root().IsNull());
    ++GLFD::Test::g_checkCount;
    CHECK(document.HasError());
  }

  // --- ディレクトリを開こうとした場合(クラッシュしないこと)---
  {
    Document         document(&mock);
    const ParseError error = document.ParseFile(root, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(!error.IsOk());
    ++GLFD::Test::g_checkCount;
    CHECK(document.Root().IsNull());
  }

  // --- 空ファイル -> DocumentEmpty ---
  {
    Document         document(&mock);
    const ParseError error = document.ParseFile(emptyPath, kNone);
    ++GLFD::Test::g_checkCount;
    CHECK(error.code == ErrorCode::DocumentEmpty);
    ++GLFD::Test::g_checkCount;
    CHECK(document.Root().IsNull());
  }

  // --- 不正な UTF-8 パス -> PathEncodingInvalid ---
  {
    // 単独の継続バイト / サロゲート相当 / 途中で切れた列。
    // 文字列リテラルで書くと再エンコードの対象になるためバイト配列で組み立てる
    static const unsigned char kBadPathBytes[][8] = {
      { 0x80, '.', 'j', 's', 'n', 0, 0, 0 },           // 継続バイト単独
      { 0xC0, 0x80, '.', 'j', 's', 'n', 0, 0 },        // 過長符号化
      { 0xED, 0xA0, 0x80, '.', 'j', 's', 'n', 0 },     // 符号化されたサロゲート
      { 0xE3, 0x81, '.', 'j', 's', 'n', 0, 0 },        // 途中で切れた 3 バイト列
      { 0xF5, 0x80, 0x80, 0x80, '.', 'j', 's', 0 },    // 範囲外の先行バイト
    };
    for (const unsigned char* rawPath : kBadPathBytes) {
      const char* const badPath = reinterpret_cast<const char*>(rawPath);
      Document         document(&mock);
      const ParseError error = document.ParseFile(badPath, kNone);
      ++GLFD::Test::g_checkCount;
      if (error.code != ErrorCode::PathEncodingInvalid) {
        char what[160];
        char scratch[128];
        std::snprintf(what, sizeof(what), "bad utf-8 path: expected PathEncodingInvalid, got %s",
                      FormatError(error, scratch, sizeof(scratch)));
        Fail(__LINE__, what);
      }
      ++GLFD::Test::g_checkCount;
      CHECK(error.offset == 0);
      ++GLFD::Test::g_checkCount;
      CHECK(document.Root().IsNull());
    }
  }

  // --- ReadEntireFile を直接叩く(内容がハンドルを閉じた後も有効であること)---
  {
    MockMemoryResource ioMock;
    JsonArena          arena(&ioMock);
    StringView         contents;
    ErrorCode          ioError = ErrorCode::None;

    ++GLFD::Test::g_checkCount;
    CHECK(ReadEntireFile(japanesePath, arena, contents, ioError));
    ++GLFD::Test::g_checkCount;
    CHECK(ioError == ErrorCode::None);
    ++GLFD::Test::g_checkCount;
    CHECK(contents.Size() == std::strlen(content));
    ++GLFD::Test::g_checkCount;
    CHECK(contents == StringView(content));

    // 空ファイルは成功して空ビュー
    ++GLFD::Test::g_checkCount;
    CHECK(ReadEntireFile(emptyPath, arena, contents, ioError));
    ++GLFD::Test::g_checkCount;
    CHECK(contents.Empty() && ioError == ErrorCode::None);

    // 確保失敗の注入
    MockMemoryResource failMock;
    failMock.SetFailAfter(0);
    JsonArena  failArena(&failMock);
    StringView failContents;
    ErrorCode  failError = ErrorCode::None;
    ++GLFD::Test::g_checkCount;
    CHECK(!ReadEntireFile(asciiPath, failArena, failContents, failError));
    ++GLFD::Test::g_checkCount;
    CHECK(failError == ErrorCode::OutOfMemory);
  }

  DeleteFileUtf8Path(asciiPath);
  DeleteFileUtf8Path(japanesePath);
  DeleteFileUtf8Path(emptyPath);
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("JsonDocument");

  Test_DomMatchesSax();
  Test_InputBufferLifetime();
  Test_ExhaustiveAllocationFailure();
  Test_Structure();
  Test_ClearAndReuse();
  Test_Move();
  Test_ParseFile();

  return Summarize();
}
