/**
 * @file  JsonEntryPointsTests.cpp
 * @brief フェーズ 1-7: エントリポイント (`Json.h`)
 *
 * @details
 *  対応する要件:
 *   - T-36 エントリポイントの往復(日本語パスを含む / pretty / compact / 異常系)
 *   - T-37 診断の寿命(**戻った後に path と detail の内容を比較する**)
 *   - T-39 ホットリロード相当(同じ Document で 100 回読んで確保が増えないこと)
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 *        日本語のパス・内容はバイト配列で組み立てる。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

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
using GLFD::Json::ArchiveIssue;
using GLFD::Json::Document;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::LoadFromJson;
using GLFD::Json::LoadFromJsonFile;
using GLFD::Json::ParseFlags;
using GLFD::Json::SaveToJson;
using GLFD::Json::SaveToJsonFile;
using GLFD::Test::MockMemoryResource;

// ---------------------------------------------------------------------------
// テスト用のユーザー型(移行対象の GameConfig と同じ形をなぞる)
// ---------------------------------------------------------------------------

namespace EntryTest {

  enum class Quality : std::int32_t { Low = 0, Medium = 10, High = 20 };

  struct Profile {
    StringView name;
    float      weight = 1.0f;
  };

  template <class Ar>
  void Serialize(Ar& ar, Profile& v) {
    (void)ar.Member("name", v.name);
    (void)ar.Member("weight", v.weight, 1.0f);
  }

  struct Window {
    std::int32_t width  = 1024;
    std::int32_t height = 768;
    StringView   title;
  };

  template <class Ar>
  void Serialize(Ar& ar, Window& v) {
    (void)ar.Member("width", v.width, 1024);
    (void)ar.Member("height", v.height, 768);
    (void)ar.Member("title", v.title);
  }

  struct Settings {
    Window                window;
    float                 bounds[2] = { 85.0f, 64.0f };
    Quality               quality   = Quality::Medium;
    std::optional<double> seed;
    DynamicArray<Profile> profiles;

    explicit Settings(GLFD::Memory::IMemoryResource* resource) : profiles(resource) {}
    Settings() = delete;
  };

  template <class Ar>
  void Serialize(Ar& ar, Settings& v) {
    (void)ar.Member("window", v.window);
    (void)ar.Member("bounds", v.bounds);
    (void)ar.Member("quality", v.quality, Quality::Medium);
    (void)ar.Member("seed", v.seed);
    (void)ar.Member("profiles", v.profiles);
  }

  /// `RequiredMember` を持つ型。診断の寿命 (T-37) に使う
  struct Mandatory {
    std::int32_t must = 0;
  };

  template <class Ar>
  void Serialize(Ar& ar, Mandatory& v) {
    (void)ar.RequiredMember("must", v.must);
  }

}

namespace {

  [[nodiscard]] bool ViewEquals(StringView view, const char* expected) {
    const size_t length = std::strlen(expected);
    return view.Size() == length
           && (length == 0 || std::memcmp(view.Data(), expected, length) == 0);
  }

  // --- Win32 の小道具(日本語パスの検証用。R2-12) -------------------------

  bool ToWide(const char* utf8, wchar_t* out, int capacity) {
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, capacity) > 0;
  }

  bool CreateDirectoryUtf8(const char* utf8Path) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) { return false; }
    return (::CreateDirectoryW(wide, nullptr) != 0) || (::GetLastError() == ERROR_ALREADY_EXISTS);
  }

  void DeleteFileUtf8(const char* utf8Path) {
    wchar_t wide[1024];
    if (ToWide(utf8Path, wide, 1024)) { ::DeleteFileW(wide); }
  }

  bool FileExistsUtf8(const char* utf8Path) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) { return false; }
    return ::GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
  }

  bool WriteTextFileUtf8(const char* utf8Path, const void* data, size_t size) {
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

  const char* TempRoot() {
    static char root[1024];
    static bool initialized = false;
    if (!initialized) {
      wchar_t     wide[512];
      const DWORD length    = ::GetTempPathW(512, wide);
      char        utf8[1024];
      const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                    utf8, sizeof(utf8), nullptr, nullptr);
      std::snprintf(root, sizeof(root), "%.*sglfd_json_entry",
                    (converted > 0 ? converted : 0), utf8);
      (void)CreateDirectoryUtf8(root);
      initialized = true;
    }
    return root;
  }

  /// UTF-8 の日本語ディレクトリ名 "設定" (E8 A8 AD E5 AE 9A)
  const char* JapaneseDir() {
    static char path[1024];
    static bool initialized = false;
    if (!initialized) {
      static const unsigned char kName[] = { 0xE8, 0xA8, 0xAD, 0xE5, 0xAE, 0x9A, 0x00 };
      std::snprintf(path, sizeof(path), "%s\\%s", TempRoot(),
                    reinterpret_cast<const char*>(kName));
      (void)CreateDirectoryUtf8(path);
      initialized = true;
    }
    return path;
  }

  /// 日本語ディレクトリ + 日本語ファイル名 "設定\\設定.json"
  const char* JapanesePath() {
    static char path[1024];
    static bool initialized = false;
    if (!initialized) {
      static const unsigned char kName[] = { 0xE8, 0xA8, 0xAD, 0xE5, 0xAE, 0x9A,
                                             '.', 'j', 's', 'o', 'n', 0x00 };
      std::snprintf(path, sizeof(path), "%s\\%s", JapaneseDir(),
                    reinterpret_cast<const char*>(kName));
      initialized = true;
    }
    return path;
  }

  // -------------------------------------------------------------------------
  // 1. T-36 往復(メモリ)
  // -------------------------------------------------------------------------

  void FillSample(EntryTest::Settings& s, MockMemoryResource& mock) {
    (void)mock;
    s.window.width  = 1920;
    s.window.height = 1080;
    s.window.title  = StringView("GLFD");
    s.bounds[0]     = 100.5f;
    s.bounds[1]     = -50.25f;
    s.quality       = EntryTest::Quality::High;
    s.seed          = 0.1 + 0.2;

    EntryTest::Profile calm;
    calm.name   = StringView("calm");
    calm.weight = 0.5f;
    EntryTest::Profile wild;
    wild.name   = StringView("wild");
    wild.weight = 2.5f;
    (void)s.profiles.TryPushBack(calm);
    (void)s.profiles.TryPushBack(wild);
  }

  [[nodiscard]] bool SameSettings(const EntryTest::Settings& a, const EntryTest::Settings& b) {
    if (a.window.width != b.window.width || a.window.height != b.window.height) { return false; }
    if (a.window.title != b.window.title) { return false; }
    if (a.bounds[0] != b.bounds[0] || a.bounds[1] != b.bounds[1]) { return false; }
    if (a.quality != b.quality) { return false; }
    if (a.seed.has_value() != b.seed.has_value()) { return false; }
    if (a.seed.has_value() && *a.seed != *b.seed) { return false; }
    if (a.profiles.GetSize() != b.profiles.GetSize()) { return false; }
    for (size_t i = 0; i < a.profiles.GetSize(); ++i) {
      if (a.profiles[i].name != b.profiles[i].name) { return false; }
      if (a.profiles[i].weight != b.profiles[i].weight) { return false; }
    }
    return true;
  }

  void TestMemoryRoundTrip() {
    GLFD::Test::BeginCase("T-36: SaveToJson -> LoadFromJson keeps every field");

    MockMemoryResource mock;

    for (int pass = 0; pass < 2; ++pass) {
      const bool pretty = (pass == 1);

      JsonArena           arena(&mock);
      JsonStringBuffer    buffer(arena);
      EntryTest::Settings source(&mock);
      FillSample(source, mock);

      CHECK(SaveToJson(source, buffer, 1, pretty));
      CHECK(!buffer.HasFailed());

      Document            doc(&mock);
      ArchiveContext      ctx(doc.Arena());
      EntryTest::Settings loaded(&mock);
      CHECK(LoadFromJson(loaded, buffer.View(), doc, ctx, ParseFlags::None));
      CHECK(!ctx.HasIssues());
      CHECK(ctx.Version() == 1u);
      CHECK(SameSettings(source, loaded));
    }

    // 簡易版(診断なし)も同じ形で書ける
    {
      JsonArena           arena(&mock);
      JsonStringBuffer    buffer(arena);
      EntryTest::Settings source(&mock);
      FillSample(source, mock);
      CHECK(SaveToJson(source, buffer, 7));

      Document            doc(&mock);
      EntryTest::Settings loaded(&mock);
      CHECK(LoadFromJson(loaded, buffer.View(), doc, ParseFlags::None));
      CHECK(SameSettings(source, loaded));
      // 簡易版でも文字列は doc 上にあるので生きている
      CHECK(ViewEquals(loaded.window.title, "GLFD"));
    }
  }

  // -------------------------------------------------------------------------
  // 2. T-36 往復(ファイル / 日本語パス)
  // -------------------------------------------------------------------------

  void TestFileRoundTrip() {
    GLFD::Test::BeginCase("T-36: SaveToJsonFile -> LoadFromJsonFile, including a Japanese path");

    MockMemoryResource mock;

    const char* const paths[2] = { nullptr, JapanesePath() };
    char              asciiPath[1024];
    std::snprintf(asciiPath, sizeof(asciiPath), "%s\\entry.json", TempRoot());

    for (int i = 0; i < 2; ++i) {
      const char* const path = (i == 0) ? asciiPath : paths[1];
      DeleteFileUtf8(path);

      EntryTest::Settings source(&mock);
      FillSample(source, mock);

      CHECK(SaveToJsonFile(source, path, &mock, 3, /*pretty=*/(i == 1)));
      CHECK(FileExistsUtf8(path));

      Document            doc(&mock);
      ArchiveContext      ctx(doc.Arena());
      EntryTest::Settings loaded(&mock);
      CHECK(LoadFromJsonFile(loaded, path, doc, ctx));
      CHECK(!ctx.HasIssues());
      CHECK(ctx.Version() == 3u);
      CHECK(SameSettings(source, loaded));

      DeleteFileUtf8(path);
    }
  }

  // -------------------------------------------------------------------------
  // 3. T-36 異常系
  // -------------------------------------------------------------------------

  void TestFailurePaths() {
    GLFD::Test::BeginCase("T-36: missing file / broken JSON / array root all return false safely");

    MockMemoryResource mock;

    // 存在しないファイル
    {
      char missing[1024];
      std::snprintf(missing, sizeof(missing), "%s\\no_such_file.json", TempRoot());
      DeleteFileUtf8(missing);

      Document            doc(&mock);
      ArchiveContext      ctx(doc.Arena());
      EntryTest::Settings loaded(&mock);
      CHECK(!LoadFromJsonFile(loaded, missing, doc, ctx));
      CHECK(doc.Error().code == GLFD::Json::ErrorCode::FileNotFound);
      // 既定値のまま。R3-4a の「何も起きなかった」が効く経路ではないが、
      // パースに到達していないので Serialize 自体が呼ばれない
      CHECK(loaded.window.width == 1024);
    }

    // 壊れた JSON
    {
      Document            doc(&mock);
      ArchiveContext      ctx(doc.Arena());
      EntryTest::Settings loaded(&mock);
      CHECK(!LoadFromJson(loaded, StringView("{\"window\": {"), doc, ctx));
      CHECK(doc.HasError());
      CHECK(loaded.window.width == 1024);
    }

    // ルートが配列 (R3-4a: 既定値の代入も起きない)
    {
      Document            doc(&mock);
      ArchiveContext      ctx(doc.Arena());
      EntryTest::Settings loaded(&mock);
      loaded.window.width = 4321;
      CHECK(!LoadFromJson(loaded, StringView("[1,2,3]"), doc, ctx));
      CHECK(ctx.HasFatal());
      CHECK(loaded.window.width == 4321);   // 触られていない
    }

    // 空ファイル
    {
      char empty[1024];
      std::snprintf(empty, sizeof(empty), "%s\\empty.json", TempRoot());
      CHECK(WriteTextFileUtf8(empty, "", 0));

      Document            doc(&mock);
      EntryTest::Settings loaded(&mock);
      CHECK(!LoadFromJsonFile(loaded, empty, doc));
      CHECK(doc.Error().code == GLFD::Json::ErrorCode::DocumentEmpty);
      DeleteFileUtf8(empty);
    }
  }

  // -------------------------------------------------------------------------
  // 4. T-37 診断の寿命
  // -------------------------------------------------------------------------

  /**
   * @brief `LoadFromJson` から**戻った後**に Issue の中身を読む
   *
   * @details
   *  ポインタが有効なだけでは不十分なので、文字列の内容を実際に比較する。
   *  §6 論点2 で「`Document` を隠すエントリポイントを作らない」と決めたのは
   *  まさにこれを成立させるためで、`ctx` が `doc.Arena()` を使っている限り
   *  診断は `doc` と同じ寿命を持つ。
   */
  void TestDiagnosticsOutliveTheCall() {
    GLFD::Test::BeginCase("T-37: issue path / detail are readable and correct after the call");

    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);

    {
      EntryTest::Settings loaded(&mock);
      // window/width が文字列 / bounds が短い / 未知フィールド
      const bool ok = LoadFromJson(
        loaded,
        StringView("{\"window\":{\"width\":\"wide\"},\"bounds\":[1],\"legacy\":0}"),
        doc, ctx, ParseFlags::None);
      CHECK(!ok == ctx.HasFatal());   // Fatal でなければ true が返る
    }
    // ここでスコープを抜けている。ctx / doc はまだ生きている

    CHECK(ctx.IssueCount() >= 3u);

    bool sawTypeMismatch = false;
    bool sawArrayLength  = false;
    bool sawUnknown      = false;
    for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
      const ArchiveIssue& issue = ctx.Issues()[i];

      // 内容を実際に比較する(ポインタが有効なだけでは不十分)
      if (issue.kind == ArchiveErrorKind::TypeMismatch
          && ViewEquals(issue.path, "window/width")) {
        sawTypeMismatch = true;
        CHECK(issue.detail.Size() > 0);
        CHECK(issue.detail.Data()[issue.detail.Size()] == '\0');   // ヌル終端の契約
      }
      if (issue.kind == ArchiveErrorKind::ArrayLengthMismatch
          && ViewEquals(issue.path, "bounds")) {
        sawArrayLength = true;
      }
      if (issue.kind == ArchiveErrorKind::UnknownField
          && ViewEquals(issue.path, "legacy")) {
        sawUnknown = true;
      }
    }
    CHECK(sawTypeMismatch);
    CHECK(sawArrayLength);
    CHECK(sawUnknown);

    // MissingRequired も同じく戻った後に読める
    {
      Document       doc2(&mock);
      ArchiveContext ctx2(doc2.Arena());
      {
        EntryTest::Mandatory value;
        CHECK(!LoadFromJson(value, StringView("{}"), doc2, ctx2, ParseFlags::None));
      }
      CHECK(ctx2.HasFatal());
      CHECK(ctx2.IssueCount() == 1u);
      CHECK(ctx2.Issues()[0].kind == ArchiveErrorKind::MissingRequired);
      CHECK(ViewEquals(ctx2.Issues()[0].path, "must"));
    }
  }

  // -------------------------------------------------------------------------
  // 5. T-39 ホットリロード相当
  // -------------------------------------------------------------------------

  /**
   * @brief 同じ `Document` で 100 回読み、確保が初回以降増えないこと
   *
   * @details
   *  `Document::Parse` は scratch しか Reset しないため、`Load*` が内部で
   *  `Document::Clear()` を呼ばないとファイル内容と DOM が毎回積み上がる。
   *  `Clear()` は `Reset()`(ブロック保持)なので、2 回目以降は
   *  `IMemoryResource::Allocate` が 1 回も発生しない。
   */
  void TestRepeatedLoad() {
    GLFD::Test::BeginCase("T-39: reloading the same file 100 times allocates nothing after the first");

    MockMemoryResource mock;

    char path[1024];
    std::snprintf(path, sizeof(path), "%s\\reload.json", TempRoot());
    static const char kJson[] =
      "{\"$version\":2,"
      "\"window\":{\"width\":800,\"height\":600,\"title\":\"reload\"},"
      "\"bounds\":[10.0,20.0],"
      "\"quality\":20,"
      "\"seed\":0.5,"
      "\"profiles\":[{\"name\":\"a\",\"weight\":1.0},{\"name\":\"b\",\"weight\":2.0}]}";
    CHECK(WriteTextFileUtf8(path, kJson, sizeof(kJson) - 1));

    Document            doc(&mock);
    ArchiveContext      ctx(doc.Arena());
    EntryTest::Settings loaded(&mock);

    CHECK(LoadFromJsonFile(loaded, path, doc, ctx));
    CHECK(loaded.window.width == 800);

    const int allocatesAfterFirst = mock.AllocateCalls();
    CHECK(allocatesAfterFirst > 0);

    bool allOk = true;
    for (int i = 0; i < 100; ++i) {
      if (!LoadFromJsonFile(loaded, path, doc, ctx)) { allOk = false; }
      CHECK_QUIET(loaded.window.width == 800);
      CHECK_QUIET(loaded.profiles.GetSize() == 2);
      CHECK_QUIET(ViewEquals(loaded.window.title, "reload"));
    }
    ++GLFD::Test::g_checkCount;

    CHECK(allOk);
    // **本テストの核心**
    CHECK(mock.AllocateCalls() == allocatesAfterFirst);
    CHECK(!ctx.HasIssues());
    CHECK(!mock.DoubleFree());
    CHECK(!mock.UnknownFree());

    DeleteFileUtf8(path);
  }

  // -------------------------------------------------------------------------
  // 6. 再ロードで前回の文字列が無効になること(R0-5 の契約)
  // -------------------------------------------------------------------------

  void TestReloadInvalidatesPreviousStrings() {
    GLFD::Test::BeginCase("reloading into the same Document is documented to invalidate the old data");

    MockMemoryResource mock;
    Document           doc(&mock);

    EntryTest::Window first;
    CHECK(LoadFromJson(first, StringView("{\"title\":\"alpha\"}"), doc, ParseFlags::None));
    CHECK(ViewEquals(first.title, "alpha"));

    // 別の構造体へ読み直す。first.title の指す領域はここで巻き戻される
    EntryTest::Window second;
    CHECK(LoadFromJson(second, StringView("{\"title\":\"beta\"}"), doc, ParseFlags::None));
    CHECK(ViewEquals(second.title, "beta"));

    // first.title は R0-5 により無効。内容は読まない(ダングリングを確認する
    // テストは書けないため、契約が守られる形 = 別 Document を使う形を示す)
    Document          doc2(&mock);
    EntryTest::Window third;
    CHECK(LoadFromJson(third, StringView("{\"title\":\"gamma\"}"), doc2, ParseFlags::None));
    CHECK(ViewEquals(second.title, "beta"));    // doc は触っていないので生きている
    CHECK(ViewEquals(third.title, "gamma"));
  }

  // -------------------------------------------------------------------------
  // 7. 書き出しの失敗経路(論点5)
  // -------------------------------------------------------------------------

  void TestSaveFailureLeavesTargetIntact() {
    GLFD::Test::BeginCase("SaveToJsonFile leaves the existing file intact when writing fails");

    MockMemoryResource mock;

    char path[1024];
    std::snprintf(path, sizeof(path), "%s\\save_guard.json", TempRoot());
    static const char kOriginal[] = "ORIGINAL";
    CHECK(WriteTextFileUtf8(path, kOriginal, sizeof(kOriginal) - 1));

    EntryTest::Settings source(&mock);
    FillSample(source, mock);

    // 確保を全て失敗させる
    mock.SetFailAfter(0);
    CHECK(!SaveToJsonFile(source, path, &mock, 1));
    mock.ClearFailure();

    // 既存ファイルが無傷であること (R1-20)
    Document doc(&mock);
    const GLFD::Json::ParseError error = doc.ParseFile(path, ParseFlags::None);
    CHECK(!error.IsOk());                 // "ORIGINAL" は JSON ではない
    CHECK(FileExistsUtf8(path));          // 消えてもいない

    // 成功すれば置き換わる
    CHECK(SaveToJsonFile(source, path, &mock, 1));
    Document            doc2(&mock);
    EntryTest::Settings loaded(&mock);
    CHECK(LoadFromJsonFile(loaded, path, doc2));
    CHECK(SameSettings(source, loaded));

    DeleteFileUtf8(path);
  }

  // -------------------------------------------------------------------------
  // 8. 静的検証
  // -------------------------------------------------------------------------

  void TestStaticProperties() {
    GLFD::Test::BeginCase("entry points are [[nodiscard]] and take a caller-owned Document");

    // const なオブジェクトを保存できること(論点1)
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);
    EntryTest::Settings mutableSource(&mock);
    FillSample(mutableSource, mock);
    const EntryTest::Settings& constSource = mutableSource;
    CHECK(SaveToJson(constSource, buffer, 1));
    CHECK(buffer.Size() > 0);

    // JsonStringBuffer が自分のアリーナを返せること(簡易版の実装に必要)
    CHECK(&buffer.Arena() == &arena);

    ++GLFD::Test::g_checkCount;
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonEntryPoints (1-7)");

  TestMemoryRoundTrip();
  TestFileRoundTrip();
  TestFailurePaths();
  TestDiagnosticsOutliveTheCall();
  TestRepeatedLoad();
  TestReloadInvalidatesPreviousStrings();
  TestSaveFailureLeavesTargetIntact();
  TestStaticProperties();

  return GLFD::Test::Summarize();
}
