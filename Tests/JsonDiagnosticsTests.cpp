/**
 * @file  JsonDiagnosticsTests.cpp
 * @brief フェーズ 2-4: 診断ログ — T-49 / T-50
 *
 * @details
 *  中核は **T-49**。「ロードは成功したが一部おかしい」が**必ず見える**こと、
 *  出力されたパスが 2-1 の `Query` にそのまま貼れること、Fatal と非 Fatal が
 *  見分けられること、そして**正常時に何も出さない**ことを固定する。
 *
 *  `Logger` は使わない。行を受け取るのは `onLine` コールバックで、
 *  テストはそれをバッファへ溜めてバイト比較する(層の規律。T-53 参照)。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 *        日本語はバイト配列で組み立てる。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/DynamicArray.h"
#include "Core/Json/Json.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

using GLFD::DynamicArray;
using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::Document;
using GLFD::Json::ErrorCode;
using GLFD::Json::FormatParseError;
using GLFD::Json::DiffValues;
using GLFD::Json::ForEachIssueLine;
using GLFD::Json::ForEachLine;
using GLFD::Json::IsFatal;
using GLFD::Json::LoadFromJson;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::ParseFlags;
using GLFD::Json::SaveToJson;
using GLFD::Json::Query;
using GLFD::Json::Value;
using GLFD::Test::MockMemoryResource;

// ---------------------------------------------------------------------------
// 診断を出させるためのユーザー型(GameConfig と同じ形をなぞる)
// ---------------------------------------------------------------------------

namespace DiagTest {

  struct Profile {
    StringView   name;
    float        weight = 1.0f;
    std::uint8_t level  = 1;   ///< RangeOverflow を出すための狭い型
  };

  template <class Ar>
  void Serialize(Ar& ar, Profile& v) {
    (void)ar.Member("name", v.name);
    (void)ar.Member("weight", v.weight, 1.0f);
    (void)ar.Member("level", v.level, static_cast<std::uint8_t>(1));
  }

  struct Window {
    std::int32_t width  = 1024;
    std::int32_t height = 768;
  };

  template <class Ar>
  void Serialize(Ar& ar, Window& v) {
    (void)ar.Member("width", v.width, 1024);
    (void)ar.Member("height", v.height, 768);
  }

  struct Settings {
    Window                window;
    float                 bounds[2] = { 85.0f, 64.0f };
    DynamicArray<Profile> profiles;

    explicit Settings(GLFD::Memory::IMemoryResource* resource) : profiles(resource) {}
    Settings() = delete;
  };

  template <class Ar>
  void Serialize(Ar& ar, Settings& v) {
    (void)ar.Member("window", v.window);
    (void)ar.Member("bounds", v.bounds);
    (void)ar.Member("profiles", v.profiles);
  }

  /// 実効値のダンプ (T-51) 用。文字列と狭い整数を持つ
  struct Dumpable {
    StringView   title;
    std::int32_t width  = 1024;
    std::uint8_t level  = 7;
    float        speed  = 2.5f;
  };

  template <class Ar>
  void Serialize(Ar& ar, Dumpable& v) {
    (void)ar.Member("title", v.title);
    (void)ar.Member("width", v.width, 1024);
    (void)ar.Member("level", v.level, static_cast<std::uint8_t>(7));
    (void)ar.Member("speed", v.speed, 2.5f);
  }

  struct Mandatory {
    std::int32_t must = 0;
  };

  template <class Ar>
  void Serialize(Ar& ar, Mandatory& v) {
    (void)ar.RequiredMember("must", v.must);
  }

}

namespace {

  // -------------------------------------------------------------------------
  // 行を溜めるだけの受け皿(Logger の代わり)
  // -------------------------------------------------------------------------

  class LineSink {
  public:
    static constexpr int    kMaxLines  = 32;
    static constexpr size_t kLineBytes = 512;

    void operator()(StringView line, bool isFatal) {
      if (m_count >= kMaxLines) { m_overflow = true; return; }
      const size_t size = (line.Size() < kLineBytes - 1) ? line.Size() : kLineBytes - 1;
      std::memcpy(m_lines[m_count], line.Data(), size);
      m_lines[m_count][size] = '\0';
      m_sizes[m_count]       = size;
      m_fatal[m_count]       = isFatal;
      ++m_count;
    }

    [[nodiscard]] int         Count()             const { return m_count; }
    [[nodiscard]] const char* Line(int i)          const { return m_lines[i]; }
    [[nodiscard]] size_t      Size(int i)          const { return m_sizes[i]; }
    [[nodiscard]] bool        Fatal(int i)         const { return m_fatal[i]; }
    [[nodiscard]] bool        Overflowed()         const { return m_overflow; }

    /// 部分一致で探す。見つからなければ -1
    [[nodiscard]] int Find(const char* needle) const {
      for (int i = 0; i < m_count; ++i) {
        if (std::strstr(m_lines[i], needle) != nullptr) { return i; }
      }
      return -1;
    }

    void Dump() const {
      for (int i = 0; i < m_count; ++i) {
        std::printf("      %s| %s\n", m_fatal[i] ? "F" : " ", m_lines[i]);
      }
    }

  private:
    char   m_lines[kMaxLines][kLineBytes] = {};
    size_t m_sizes[kMaxLines]             = {};
    bool   m_fatal[kMaxLines]             = {};
    int    m_count                        = 0;
    bool   m_overflow                     = false;
  };

  /// 行から N 列目の空白区切りトークンを取り出す(パスを Query へ渡すため)
  [[nodiscard]] StringView Column(const char* line, int index) {
    const char* p = line;
    for (int i = 0; i <= index; ++i) {
      while (*p == ' ') { ++p; }
      if (*p == '\0') { return StringView(); }
      const char* start = p;
      while (*p != '\0' && *p != ' ') { ++p; }
      if (i == index) { return StringView(start, static_cast<size_t>(p - start)); }
    }
    return StringView();
  }

  // --- 一時ファイル -----------------------------------------------------------

  bool ToWide(const char* utf8, wchar_t* out, int capacity) {
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, capacity) > 0;
  }

  void DeleteFileUtf8(const char* utf8Path) {
    wchar_t wide[1024];
    if (ToWide(utf8Path, wide, 1024)) { ::DeleteFileW(wide); }
  }

  bool WriteBytesUtf8Path(const char* utf8Path, const void* data, size_t size) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) { return false; }
    const HANDLE handle = ::CreateFileW(wide, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { return false; }
    bool ok = true;
    if (size != 0) {
      DWORD written = 0;
      ok = (::WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr) != 0)
        && (written == size);
    }
    ::CloseHandle(handle);
    return ok;
  }

  /**
   * @brief TEMP 配下のパスを組み立てる
   * @warning **戻り値は共有の static バッファ**である。2 本のパスを同時に持つときは
   *          それぞれ自分の配列へ写すこと。写さないと、後から取った方が
   *          前の内容を上書きし、**2 つが同じファイルを指す**
   *          (2-3 の T-41 で実際に踏んだ。基底と上書きが同一ファイルになった)
   */
  const char* TempPath(const char* leaf) {
    static char path[1024];
    wchar_t     wide[512];
    const DWORD length    = ::GetTempPathW(512, wide);
    char        utf8[1024];
    const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                  utf8, sizeof(utf8), nullptr, nullptr);
    std::snprintf(path, sizeof(path), "%.*s%s", (converted > 0 ? converted : 0), utf8, leaf);
    return path;
  }

  // -------------------------------------------------------------------------
  // T-49 Issue の出力(A〜C の中核)
  // -------------------------------------------------------------------------

  void TestIssuesAreReported() {
    GLFD::Test::BeginCase("T-49: every issue is printed, and its path can be pasted into Query");

    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);

    // 型違い / 要素数違い / 範囲外 / 未知フィールドを 1 つの config に詰める
    static const char kJson[] =
      "{\"window\":{\"width\":\"wide\",\"height\":900},"
      " \"bounds\":[1.5],"
      " \"profiles\":[{\"name\":\"first\",\"level\":999}],"
      " \"legacy\":7}";

    {
      DiagTest::Settings loaded(&mock);
      CHECK(LoadFromJson(loaded, StringView(kJson), doc, ctx, ParseFlags::None));
      CHECK(!ctx.HasFatal());   // どれも Fatal ではない = 起動は続行できる
    }

    LineSink sink;
    ForEachIssueLine(ctx, StringView("GameConfig.jsonc"), sink);
    if (sink.Count() == 0) { std::printf("    (nothing was printed)\n"); }
    sink.Dump();
    CHECK(!sink.Overflowed());

    // ヘッダ + 4 件
    CHECK(sink.Count() == static_cast<int>(ctx.IssueCount()) + 1);
    CHECK(sink.Find("GameConfig.jsonc: 4 issue(s), 0 fatal") == 0);

    // 4 種がすべて出ている
    CHECK(sink.Find("TypeMismatch") > 0);
    CHECK(sink.Find("ArrayLengthMismatch") > 0);
    CHECK(sink.Find("RangeOverflow") > 0);
    CHECK(sink.Find("UnknownField") > 0);

    // パスも出ている
    CHECK(sink.Find("window/width") > 0);
    CHECK(sink.Find("bounds") > 0);
    CHECK(sink.Find("profiles/0/level") > 0);
    CHECK(sink.Find("legacy") > 0);

    // **出力された行からパスを切り出して Query に渡すと値が引ける**(2-1 との結合)
    int resolved = 0;
    for (int i = 1; i < sink.Count(); ++i) {
      const StringView path = Column(sink.Line(i), 1);   // 0 列目は kind
      if (path == StringView("<root>")) { continue; }
      const Value* const value = doc.Query(path);
      if (value != nullptr) { ++resolved; }
      else { std::printf("    (unresolved path: %s)\n", sink.Line(i)); }
    }
    CHECK(resolved == sink.Count() - 1);

    // 非 Fatal なので、どの行も fatal 印は付かない
    bool anyFatal = false;
    for (int i = 0; i < sink.Count(); ++i) { anyFatal = anyFatal || sink.Fatal(i); }
    CHECK(!anyFatal);
  }

  void TestFatalIsDistinguishable() {
    GLFD::Test::BeginCase("T-49: fatal and non-fatal issues are told apart");

    MockMemoryResource mock;

    // (a) MissingRequired は常に Fatal
    {
      Document       doc(&mock);
      ArchiveContext ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);
      {
        DiagTest::Mandatory value;
        CHECK(!LoadFromJson(value, StringView("{\"other\":1}"), doc, ctx, ParseFlags::None));
      }
      CHECK(ctx.HasFatal());

      LineSink sink;
      ForEachIssueLine(ctx, StringView("save.json"), sink);
      sink.Dump();

      const int missing = sink.Find("MissingRequired");
      CHECK(missing > 0);
      if (missing > 0) { CHECK(sink.Fatal(missing)); }         // **Fatal と分かる**

      const int unknown = sink.Find("UnknownField");
      CHECK(unknown > 0);
      if (unknown > 0) { CHECK(!sink.Fatal(unknown)); }        // 同じ文脈でも非 Fatal
      CHECK(sink.Fatal(0));                                    // ヘッダも Fatal 側
      CHECK(sink.Find("2 issue(s), 1 fatal") == 0);
    }

    // (b) StrictTypes を立てると TypeMismatch が Fatal に変わる
    {
      Document       docA(&mock);
      ArchiveContext lenient(docA.Arena());
      Document       docB(&mock);
      ArchiveContext strict(docB.Arena(), 0, ArchiveFlags::StrictTypes);
      {
        DiagTest::Settings a(&mock);
        DiagTest::Settings b(&mock);
        (void)LoadFromJson(a, StringView("{\"window\":{\"width\":\"x\"}}"), docA, lenient,
                           ParseFlags::None);
        (void)LoadFromJson(b, StringView("{\"window\":{\"width\":\"x\"}}"), docB, strict,
                           ParseFlags::None);
      }

      LineSink lenientSink;
      LineSink strictSink;
      ForEachIssueLine(lenient, StringView("a.json"), lenientSink);
      ForEachIssueLine(strict, StringView("b.json"), strictSink);

      const int lenientLine = lenientSink.Find("TypeMismatch");
      const int strictLine  = strictSink.Find("TypeMismatch");
      CHECK(lenientLine > 0);
      CHECK(strictLine > 0);
      if (lenientLine > 0) { CHECK(!lenientSink.Fatal(lenientLine)); }
      if (strictLine > 0)  { CHECK(strictSink.Fatal(strictLine)); }

      // 判定は自由関数として公開されているので、呼び出し側が単体でも使える
      CHECK(!IsFatal(ArchiveErrorKind::TypeMismatch, ArchiveFlags::None));
      CHECK(IsFatal(ArchiveErrorKind::TypeMismatch, ArchiveFlags::StrictTypes));
      CHECK(IsFatal(ArchiveErrorKind::MissingRequired, ArchiveFlags::None));
      CHECK(IsFatal(ArchiveErrorKind::ContainerFailure, ArchiveFlags::None));
      CHECK(!IsFatal(ArchiveErrorKind::RangeOverflow, ArchiveFlags::StrictTypes));
      CHECK(!IsFatal(ArchiveErrorKind::UnknownField, ArchiveFlags::StrictTypes));
      CHECK(!IsFatal(ArchiveErrorKind::ArrayLengthMismatch, ArchiveFlags::StrictTypes));
    }
  }

  void TestSilentWhenClean() {
    GLFD::Test::BeginCase("T-49: a clean load prints nothing at all");

    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);

    {
      DiagTest::Settings loaded(&mock);
      CHECK(LoadFromJson(
        loaded,
        StringView("{\"window\":{\"width\":800,\"height\":600},"
                   " \"bounds\":[1.0,2.0],"
                   " \"profiles\":[{\"name\":\"a\",\"weight\":2.0,\"level\":3}]}"),
        doc, ctx, ParseFlags::None));
    }
    CHECK(!ctx.HasIssues());

    LineSink sink;
    ForEachIssueLine(ctx, StringView("GameConfig.jsonc"), sink);
    // **ヘッダすら出さない。** 正常時に 1 行でも出ると、異常時の行が埋もれる
    CHECK(sink.Count() == 0);
  }

  // -------------------------------------------------------------------------
  // T-50 line:column
  // -------------------------------------------------------------------------

  void TestParseErrorLocation() {
    GLFD::Test::BeginCase("T-50: parse failures are reported as file(line,col)");

    MockMemoryResource mock;
    char               line[GLFD::Json::kDiagnosticLineCapacity];

    // (a) 既知の位置に構文エラーを置く。3 行目の 8 桁目に ',' が来る
    //     1: {
    //     2:   "a": 1,
    //     3:   "b": ,
    {
      static const char kJson[] = "{\n  \"a\": 1,\n  \"b\": ,\n}\n";
      Document          doc(&mock);
      const GLFD::Json::ParseError error = doc.Parse(StringView(kJson, sizeof(kJson) - 1),
                                                     ParseFlags::None);
      CHECK(!error.IsOk());

      const size_t size = FormatParseError(line, sizeof(line), StringView("GameConfig.jsonc"),
                                           error.code, error.offset,
                                           StringView(kJson, sizeof(kJson) - 1));
      std::printf("      %s\n", line);
      CHECK(size > 0);
      CHECK(std::strstr(line, "GameConfig.jsonc(3,8): ") == line);
    }

    // (b) **日本語のコメントを挟んだ後**の桁。コードポイント単位でなければずれる
    //     { "a":1, /* AI */ "b": }   の "AI" を 3 バイト文字 2 つに置き換える
    //     桁: 1:'{' 2:'"' 3:'a' 4:'"' 5:':' 6:'1' 7:',' 8:'/' 9:'*'
    //         10:<U+3042> 11:<U+3044> 12:'*' 13:'/' 14:'"' 15:'b' 16:'"' 17:':' 18:'}'
    {
      static const unsigned char kJson[] = {
        '{', '"', 'a', '"', ':', '1', ',', '/', '*',
        0xE3, 0x81, 0x82,          // U+3042
        0xE3, 0x81, 0x84,          // U+3044
        '*', '/', '"', 'b', '"', ':', '}',
      };
      const StringView source(reinterpret_cast<const char*>(kJson), sizeof(kJson));

      Document                     doc(&mock);
      const GLFD::Json::ParseError error = doc.Parse(source, ParseFlags::JsonC);
      CHECK(!error.IsOk());

      const size_t size = FormatParseError(line, sizeof(line), StringView("jp.jsonc"),
                                           error.code, error.offset, source);
      std::printf("      %s\n", line);
      CHECK(size > 0);
      // バイト単位で数えていれば (1,22) になる。コードポイントなら (1,18)
      CHECK(std::strstr(line, "jp.jsonc(1,18): ") == line);
    }

    // (c) CRLF でも行番号が正しいこと
    {
      static const char kJson[] = "{\r\n  \"a\": 1,\r\n  \"b\":\r\n}\r\n";
      const StringView  source(kJson, sizeof(kJson) - 1);

      Document                     doc(&mock);
      const GLFD::Json::ParseError error = doc.Parse(source, ParseFlags::None);
      CHECK(!error.IsOk());

      const size_t size = FormatParseError(line, sizeof(line), StringView("crlf.json"),
                                           error.code, error.offset, source);
      std::printf("      %s\n", line);
      CHECK(size > 0);
      CHECK(std::strstr(line, "crlf.json(4,1): ") == line);
    }

    // (d) ソースが無ければ位置を落とす(Parse(StringView) 経路の Document::Source())
    {
      const size_t size = FormatParseError(line, sizeof(line), StringView("no-source.json"),
                                           ErrorCode::ObjectMissingName, 42, StringView());
      CHECK(size > 0);
      CHECK(std::strcmp(line, "no-source.json: ObjectMissingName") == 0);
    }
  }

  void TestDocumentKeepsFileSource() {
    GLFD::Test::BeginCase("T-50: ParseFile keeps its text so the caller can locate the error");

    MockMemoryResource mock;
    const char* const  path = TempPath("glfd_diag_broken.jsonc");

    // 2 行目の 10 桁目でコロンが無い
    static const char kBroken[] = "{\n  \"width\" 1280\n}\n";
    CHECK(WriteBytesUtf8Path(path, kBroken, sizeof(kBroken) - 1));

    {
      Document                     doc(&mock);
      const GLFD::Json::ParseError error = doc.ParseFile(path, ParseFlags::JsonC);
      CHECK(!error.IsOk());

      // **ParseFile 経路ではソースが残る**。これが無いと line:column を出せない
      CHECK(!doc.Source().Empty());
      CHECK(doc.Source().Size() == sizeof(kBroken) - 1);

      char         line[GLFD::Json::kDiagnosticLineCapacity];
      const size_t size = FormatParseError(line, sizeof(line), StringView("broken.jsonc"),
                                           error.code, error.offset, doc.Source());
      std::printf("      %s\n", line);
      CHECK(size > 0);
      CHECK(std::strstr(line, "broken.jsonc(2,11): ") == line);

      // Clear で参照ごと捨てる (R3-34)
      doc.Clear();
      CHECK(doc.Source().Empty());
    }

    // Parse(StringView) 経路では保持しない(呼び出し側のバッファを指し続けない)
    {
      Document doc(&mock);
      (void)doc.Parse(StringView("{\"a\":1}"), ParseFlags::None);
      CHECK(doc.Source().Empty());
    }

    // **空の Source() を ComputeLocation へ渡すと、常に (1,1) が返る。**
    // 誤った位置が静かに出るので、位置を出す側は空を判定しなければならない
    {
      const GLFD::Json::SourceLocation location =
        GLFD::Json::ComputeLocation(StringView(), 12345);
      CHECK(location.line == 1u);
      CHECK(location.column == 1u);
    }

    // Parse(StringView) で失敗した Document から診断を出しても**位置は付かない**。
    // FormatParseError が空の source を見て落とすのがガードで、(1,1) という
    // 嘘の位置は出ない
    {
      Document                     doc(&mock);
      static const char            kSource[] = "{\n  \"a\": ,\n}";
      const GLFD::Json::ParseError error =
        doc.Parse(StringView(kSource, sizeof(kSource) - 1), ParseFlags::None);
      CHECK(!error.IsOk());
      CHECK(doc.Source().Empty());

      char         line[GLFD::Json::kDiagnosticLineCapacity];
      const size_t size = FormatParseError(line, sizeof(line), StringView("inline.json"),
                                           error.code, error.offset, doc.Source());
      std::printf("      %s\n", line);
      CHECK(size > 0);
      CHECK(std::strchr(line, '(') == nullptr);          // 位置は出ていない
      CHECK(std::strstr(line, "inline.json: ") == line);

      // 呼び出し側が元テキストを持っていれば、同じ関数で位置を出せる
      const size_t withSource = FormatParseError(line, sizeof(line), StringView("inline.json"),
                                                 error.code, error.offset,
                                                 StringView(kSource, sizeof(kSource) - 1));
      std::printf("      %s\n", line);
      CHECK(withSource > 0);
      CHECK(std::strstr(line, "inline.json(2,8): ") == line);
    }

    DeleteFileUtf8(path);
  }

  // -------------------------------------------------------------------------
  // T-51 実効値のダンプ
  // -------------------------------------------------------------------------

  void TestEffectiveValueDump() {
    GLFD::Test::BeginCase("T-51: the dump shows what the engine actually uses, not the file");

    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);

    // "GLFD " + 日本語 3 文字。非 ASCII のリテラルは書かない (C5297)
    static const unsigned char kTitle[] = {
      'G', 'L', 'F', 'D', ' ',
      0xE3, 0x83, 0x9C,   // U+30DC
      0xE3, 0x82, 0xA4,   // U+30A4
      0xE3, 0x83, 0x89,   // U+30C9
    };

    char      json[256];
    const int written = std::snprintf(
      json, sizeof(json),
      "{\"title\":\"%.*s\", \"level\":999, \"legacy\":1}",
      static_cast<int>(sizeof(kTitle)), reinterpret_cast<const char*>(kTitle));
    CHECK(written > 0);

    DiagTest::Dumpable value;
    CHECK(LoadFromJson(value, StringView(json, static_cast<size_t>(written)), doc, ctx,
                       ParseFlags::None));
    CHECK(ctx.IssueCount() == 2u);   // level が範囲外 / legacy が未知

    // ダンプ用のアリーナは **config の Document とは別**にする。
    // Document のアリーナへ書くと、ダンプのたびにその寿命ぶん増え続ける
    JsonArena        arena(&mock);
    JsonStringBuffer buffer(arena);
    CHECK(SaveToJson(value, buffer, 1u, /*pretty=*/true));

    LineSink sink;
    ForEachLine(buffer.View(), [&sink](StringView line) { sink(line, false); });
    sink.Dump();
    CHECK(sink.Count() > 3);      // pretty なので複数行になる
    CHECK(!sink.Overflowed());

    // 欠損したフィールドは**既定値で**出る (R3-4)
    CHECK(sink.Find("\"width\": 1024") > 0);
    CHECK(sink.Find("\"speed\": 2.5") > 0);

    // 範囲外で拒否された値は**既定のまま** (R3-11)。999 は出ない
    CHECK(sink.Find("\"level\": 7") > 0);
    CHECK(sink.Find("999") < 0);

    // **未知フィールドは出ない**(捨てられている。R3-5)
    CHECK(sink.Find("legacy") < 0);

    // 日本語の値がそのまま出る
    CHECK(sink.Find(reinterpret_cast<const char*>(kTitle) + 5) > 0);

    // $version も出る (R3-7)
    CHECK(sink.Find("\"$version\": 1") >= 0);
  }

  // -------------------------------------------------------------------------
  // T-52 差分
  // -------------------------------------------------------------------------

  void TestDiff() {
    GLFD::Test::BeginCase("T-52: only the changes are printed, with pasteable paths");

    MockMemoryResource mock;

    // (a) 変化なし -> 1 行も出ない
    {
      Document before(&mock);
      Document after(&mock);
      static const char kSame[] = "{\"a\":1,\"b\":[1,2],\"c\":{\"d\":\"x\"}}";
      CHECK(before.Parse(StringView(kSame), ParseFlags::None).IsOk());
      CHECK(after.Parse(StringView(kSame), ParseFlags::None).IsOk());

      LineSink sink;
      DiffValues(before.Root(), after.Root(), [&sink](StringView line) { sink(line, false); });
      CHECK(sink.Count() == 0);
    }

    // (b) スカラ / 型 / 配列長 / メンバ増減
    {
      Document before(&mock);
      Document after(&mock);
      CHECK(before.Parse(StringView(
        "{\"simulation\":{\"maxSpeed\":2.0,\"count\":10},"
        " \"world\":{\"bounds\":[85.0,64.0],\"damping\":0.5},"
        " \"name\":\"old\"}"), ParseFlags::None).IsOk());
      CHECK(after.Parse(StringView(
        "{\"simulation\":{\"maxSpeed\":6.5,\"count\":\"ten\"},"
        " \"world\":{\"bounds\":[120.0],\"gravity\":-9.8},"
        " \"name\":\"old\"}"), ParseFlags::None).IsOk());

      LineSink sink;
      DiffValues(before.Root(), after.Root(), [&sink](StringView line) { sink(line, false); });
      sink.Dump();
      CHECK(!sink.Overflowed());

      // スカラの変化(本命)
      const int speed = sink.Find("simulation/maxSpeed");
      CHECK(speed >= 0);
      if (speed >= 0) { CHECK(std::strstr(sink.Line(speed), "2 -> 6.5") != nullptr); }

      // 型の変化は型名だけ
      const int count = sink.Find("simulation/count");
      CHECK(count >= 0);
      if (count >= 0) { CHECK(std::strstr(sink.Line(count), "number -> string") != nullptr); }

      // 配列長 + 共通部分の中身
      const int bounds = sink.Find("world/bounds ");
      CHECK(bounds >= 0);
      if (bounds >= 0) { CHECK(std::strstr(sink.Line(bounds), "[2] -> [1]") != nullptr); }
      const int element = sink.Find("world/bounds/0");
      CHECK(element >= 0);
      if (element >= 0) { CHECK(std::strstr(sink.Line(element), "85 -> 120") != nullptr); }

      // メンバの増減
      CHECK(sink.Find("- world/damping") >= 0);
      CHECK(sink.Find("+ world/gravity") >= 0);

      // 変わっていないものは出ない
      CHECK(sink.Find("name") < 0);

      // **出力されたパスは Query に貼れる**(R3-9 と同じ形式)
      const Value* const changed = after.Query(StringView("simulation/maxSpeed"));
      CHECK(changed != nullptr);
      const Value* const inArray = after.Query(StringView("world/bounds/0"));
      CHECK(inArray != nullptr);
    }

    // (c) 2 と 2.0 は「変わった」と出さない(型は違うが値は同じ)
    {
      Document before(&mock);
      Document after(&mock);
      CHECK(before.Parse(StringView("{\"speed\":2}"), ParseFlags::None).IsOk());
      CHECK(after.Parse(StringView("{\"speed\":2.0}"), ParseFlags::None).IsOk());

      LineSink sink;
      DiffValues(before.Root(), after.Root(), [&sink](StringView line) { sink(line, false); });
      CHECK(sink.Count() == 0);
    }
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonDiagnostics (2-4)");

  TestIssuesAreReported();
  TestFatalIsDistinguishable();
  TestSilentWhenClean();
  TestParseErrorLocation();
  TestDocumentKeepsFileSource();
  TestEffectiveValueDump();
  TestDiff();

  return GLFD::Test::Summarize();
}
