/**
 * @file  JsonQueryTests.cpp
 * @brief フェーズ 2-1: パスアクセス — T-42 〜 T-47
 *
 * @details
 *  中核は **T-42(診断パスとの往復)**。`ArchiveIssue::path` をそのまま
 *  `Query` へ渡して、**問題のある値そのもの**が返ることを確かめる。
 *  これが通ることが、R3-9 で「角括弧を使わない `/` 区切り」を選んだ判断の回収になる。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/DynamicArray.h"
#include "Core/Json/Json.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::DynamicArray;
using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::ArchiveIssue;
using GLFD::Json::Document;
using GLFD::Json::LoadFromJson;
using GLFD::Json::ParseFlags;
using GLFD::Json::Query;
using GLFD::Json::Value;
using GLFD::Test::MockMemoryResource;

// ---------------------------------------------------------------------------
// T-42 用のユーザー型。4 種の Issue を1つの JSON から出せる形にしてある
// ---------------------------------------------------------------------------

namespace QueryTest {

  struct Profile {
    StringView   name;
    float        weight = 1.0f;
    std::uint8_t level  = 1;   ///< RangeOverflow を出すために幅の狭い型を置く
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

  /// 枠の上限より深いネストを作るための型(T-46c。1 形態だけ)
  template <int N> struct Deep { Deep<N - 1> n; };
  template <>      struct Deep<0> { std::int32_t leaf = 0; };

  template <class Ar, int N> void Serialize(Ar& ar, Deep<N>& v) { (void)ar.Member("n", v.n); }
  template <class Ar>        void Serialize(Ar& ar, Deep<0>& v) { (void)ar.Member("leaf", v.leaf); }

  /// `MissingRequired` の経路。パスが JSON に存在しないことの確認に使う
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

  /// 指定の種別で、指定のパスを持つ Issue を探す
  [[nodiscard]] const ArchiveIssue* FindIssue(const ArchiveContext& ctx,
                                              ArchiveErrorKind kind, const char* path) {
    for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
      const ArchiveIssue& issue = ctx.Issues()[i];
      if (issue.kind == kind && ViewEquals(issue.path, path)) {
        return &issue;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool IsInt(const Value* v, std::int64_t expected) {
    std::int64_t actual = 0;
    return v != nullptr && v->TryGetInt64(actual) && actual == expected;
  }

  // -------------------------------------------------------------------------
  // T-42 診断パスとの往復(このフェーズの中核)
  // -------------------------------------------------------------------------

  void TestIssuePathRoundTrip() {
    GLFD::Test::BeginCase("T-42: every diagnostic path resolves back to the offending value");

    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);

    // window/width が文字列 (TypeMismatch) / bounds が 1 要素 (ArrayLengthMismatch) /
    // profiles/1/level が uint8 に収まらない (RangeOverflow) / legacy は未知 (UnknownField)
    static const char kJson[] =
      "{\"window\":{\"width\":\"wide\",\"height\":900},"
      " \"bounds\":[1.5],"
      " \"profiles\":[{\"name\":\"first\",\"weight\":2.0,\"level\":3},"
      "              {\"name\":\"second\",\"weight\":4.0,\"level\":999}],"
      " \"legacy\":7}";

    {
      QueryTest::Settings loaded(&mock);
      // 4 種はいずれも Fatal ではないので、ロード自体は成功する
      CHECK(LoadFromJson(loaded, StringView(kJson), doc, ctx, ParseFlags::None));
      CHECK(!ctx.HasFatal());
    }

    // --- 4 種それぞれについて、パスを貼って値を引く ---------------------------

    // (1) TypeMismatch — ネストしたユーザー型の中のスカラ
    {
      const ArchiveIssue* const issue =
        FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "window/width");
      CHECK(issue != nullptr);
      if (issue != nullptr) {
        const Value* const v = doc.Query(issue->path);
        CHECK(v != nullptr);                      // **診断パスがそのまま通る**
        if (v != nullptr) {
          StringView text;
          CHECK(v->TryGetString(text));           // 問題の値そのもの = 文字列
          CHECK(ViewEquals(text, "wide"));
        }
      }
    }

    // (2) ArrayLengthMismatch — T[N]。パスは要素ではなく**配列そのもの**を指す
    {
      const ArchiveIssue* const issue =
        FindIssue(ctx, ArchiveErrorKind::ArrayLengthMismatch, "bounds");
      CHECK(issue != nullptr);
      if (issue != nullptr) {
        const Value* const v = doc.Query(issue->path);
        CHECK(v != nullptr);
        if (v != nullptr) {
          CHECK(v->IsArray());
          CHECK(v->Size() == 1u);                 // float[2] に 1 要素しか無かった
        }
      }
    }

    // (3) RangeOverflow — **配列要素の中**のスカラ ("profiles/1/level")
    {
      const ArchiveIssue* const issue =
        FindIssue(ctx, ArchiveErrorKind::RangeOverflow, "profiles/1/level");
      CHECK(issue != nullptr);
      if (issue != nullptr) {
        const Value* const v = doc.Query(issue->path);
        CHECK(v != nullptr);
        CHECK(IsInt(v, 999));                     // uint8 に入らなかった値そのもの
      }
    }

    // (4) UnknownField — パスは未知メンバ自身を指す
    {
      const ArchiveIssue* const issue =
        FindIssue(ctx, ArchiveErrorKind::UnknownField, "legacy");
      CHECK(issue != nullptr);
      if (issue != nullptr) {
        const Value* const v = doc.Query(issue->path);
        CHECK(v != nullptr);
        CHECK(IsInt(v, 7));
      }
    }

    // --- 記録された Issue が1つ残らず引けること(取りこぼしの検出)-------------
    CHECK(ctx.IssueCount() >= 4u);
    bool everyIssueResolves = true;
    for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
      if (doc.Query(ctx.Issues()[i].path) == nullptr) { everyIssueResolves = false; }
    }
    CHECK(everyIssueResolves);

    // --- MissingRequired だけは nullptr になる(意図的な性質)-----------------
    {
      Document       doc2(&mock);
      ArchiveContext ctx2(doc2.Arena());
      {
        QueryTest::Mandatory value;
        CHECK(!LoadFromJson(value, StringView("{\"other\":1}"), doc2, ctx2, ParseFlags::None));
      }
      CHECK(ctx2.IssueCount() == 1u);
      if (ctx2.IssueCount() == 1u) {
        const ArchiveIssue& issue = ctx2.Issues()[0];
        CHECK(issue.kind == ArchiveErrorKind::MissingRequired);
        CHECK(ViewEquals(issue.path, "must"));
        // **引けない = そのフィールドが JSON に無い**、と読める
        CHECK(doc2.Query(issue.path) == nullptr);
        CHECK(doc2.Query(StringView("other")) != nullptr);   // 実在する方は引ける
      }
    }
  }

  // -------------------------------------------------------------------------
  // T-43 解決規則
  // -------------------------------------------------------------------------

  void TestResolutionRules() {
    GLFD::Test::BeginCase("T-43: the node type decides how each segment is read");

    MockMemoryResource mock;
    Document           doc(&mock);

    static const char kJson[] =
      "{\"a\":{\"b\":{\"c\":42}},"
      " \"arr\":[10,20,{\"k\":\"v\"}],"
      " \"matrix\":[[1,2],[3]],"
      " \"3\":\"root numeric key\","
      " \"obj\":{\"3\":7},"
      " \"scalar\":5,"
      " \"nothing\":null,"
      " \"empty\":[],"
      " \"dup\":1,\"dup\":2}";
    CHECK(doc.Parse(StringView(kJson), ParseFlags::None).IsOk());

    // オブジェクトのキー / 多段のネスト
    CHECK(IsInt(doc.Query(StringView("a/b/c")), 42));
    CHECK(doc.Query(StringView("a")) != nullptr);
    CHECK(doc.Query(StringView("a/b")) != nullptr);

    // 配列のインデックス
    CHECK(IsInt(doc.Query(StringView("arr/0")), 10));
    CHECK(IsInt(doc.Query(StringView("arr/1")), 20));
    // 配列 -> オブジェクト -> キー
    {
      const Value* const v = doc.Query(StringView("arr/2/k"));
      CHECK(v != nullptr);
      if (v != nullptr) {
        StringView text;
        CHECK(v->TryGetString(text) && ViewEquals(text, "v"));
      }
    }
    // 配列の配列
    CHECK(IsInt(doc.Query(StringView("matrix/0/1")), 2));
    CHECK(IsInt(doc.Query(StringView("matrix/1/0")), 3));
    CHECK(doc.Query(StringView("matrix/1/1")) == nullptr);   // 2 本目は 1 要素しかない

    // 存在しないキー / 範囲外の添字
    CHECK(doc.Query(StringView("nosuch")) == nullptr);
    CHECK(doc.Query(StringView("a/b/nosuch")) == nullptr);
    CHECK(doc.Query(StringView("arr/3")) == nullptr);
    CHECK(doc.Query(StringView("empty/0")) == nullptr);      // 空配列

    // **オブジェクトに数字のセグメント** -> キーとして引ける
    {
      const Value* const v = doc.Query(StringView("3"));
      CHECK(v != nullptr);
      if (v != nullptr) {
        StringView text;
        CHECK(v->TryGetString(text) && ViewEquals(text, "root numeric key"));
      }
    }
    CHECK(IsInt(doc.Query(StringView("obj/3")), 7));

    // **配列にキー名のセグメント** -> nullptr
    CHECK(doc.Query(StringView("arr/k")) == nullptr);
    CHECK(doc.Query(StringView("matrix/first")) == nullptr);

    // スカラ / Null の先は解決できない
    CHECK(doc.Query(StringView("scalar")) != nullptr);
    CHECK(doc.Query(StringView("scalar/0")) == nullptr);
    CHECK(doc.Query(StringView("scalar/any")) == nullptr);
    CHECK(doc.Query(StringView("nothing")) != nullptr);      // null という値は存在する
    CHECK(doc.Query(StringView("nothing/x")) == nullptr);
    CHECK(doc.Query(StringView("a/b/c/d")) == nullptr);

    // キー重複 (R2-3) は Find と同じく最初の一致
    CHECK(IsInt(doc.Query(StringView("dup")), 1));
    CHECK(doc.Root().Find(StringView("dup")) == doc.Query(StringView("dup")));

    // 部分木からの相対クエリ(自由関数の形)
    {
      const Value* const a = doc.Query(StringView("a"));
      CHECK(a != nullptr);
      if (a != nullptr) {
        CHECK(IsInt(Query(*a, StringView("b/c")), 42));
        CHECK(Query(*a, StringView("c")) == nullptr);
      }
      // Document::Query は Json::Query(Root(), path) への転送でしかない
      CHECK(doc.Query(StringView("a/b/c")) == Query(doc.Root(), StringView("a/b/c")));
    }
  }

  // -------------------------------------------------------------------------
  // T-44 インデックスの厳格性
  // -------------------------------------------------------------------------

  void TestStrictIndexParsing() {
    GLFD::Test::BeginCase("T-44: array indices are parsed strictly, never via from_chars");

    MockMemoryResource mock;
    Document           doc(&mock);
    CHECK(doc.Parse(StringView("{\"arr\":[10,20,30]}"), ParseFlags::None).IsOk());

    // 通る形はこの 3 つだけ
    CHECK(IsInt(doc.Query(StringView("arr/0")), 10));
    CHECK(IsInt(doc.Query(StringView("arr/1")), 20));
    CHECK(IsInt(doc.Query(StringView("arr/2")), 30));

    struct Case {
      const char* path;
      const char* why;
    };
    static const Case kRejected[] = {
      { "arr/01",          "leading zero" },
      { "arr/00",          "leading zero" },
      { "arr/0000",        "leading zero" },
      { "arr/+1",          "explicit sign" },
      { "arr/-1",          "negative" },
      { "arr/ 1",          "leading space" },
      { "arr/1 ",          "trailing space" },
      { "arr/1x",          "trailing garbage" },
      { "arr/x1",          "leading garbage" },
      { "arr/1.0",         "not an integer" },
      { "arr/0x1",         "hex" },
      { "arr/1e0",         "exponent" },
      { "arr/3",           "out of range" },
      { "arr/4",           "out of range" },
      { "arr/4294967295",  "out of range (UINT32_MAX)" },
      // **桁溢れで小さい添字に化けないこと。** 4294967296 = 2^32
      { "arr/4294967296",  "beyond uint32" },
      { "arr/99999999999", "11 digits" },
      { "arr/18446744073709551616", "beyond uint64 as text" },
    };

    for (const Case& c : kRejected) {
      // 期待値が個別に意味を持つので CHECK(要件書 §8)
      CHECK(doc.Query(StringView(c.path)) == nullptr);
      if (doc.Query(StringView(c.path)) != nullptr) {
        std::printf("    (accepted a bad index: %s -- %s)\n", c.path, c.why);
      }
    }

    // 空セグメントは添字としても不正
    CHECK(doc.Query(StringView("arr/")) == nullptr);
  }

  // -------------------------------------------------------------------------
  // T-45 確保ゼロ
  // -------------------------------------------------------------------------

  void TestQueryDoesNotAllocate() {
    GLFD::Test::BeginCase("T-45: Query never allocates, no matter how often it is called");

    MockMemoryResource mock;
    Document           doc(&mock);
    CHECK(doc.Parse(StringView("{\"a\":{\"b\":[{\"c\":1},{\"c\":2}]},\"arr\":[1,2,3]}"),
                    ParseFlags::None).IsOk());

    static const char* const kPaths[] = {
      "", "a", "a/b", "a/b/0", "a/b/1/c", "arr/2",
      "nosuch", "a/b/99", "arr/01", "a/b/0/c/d", "...", "a//b",
    };

    const int before = mock.AllocateCalls();

    for (int round = 0; round < 5000; ++round) {
      for (const char* path : kPaths) {
        const Value* const v = doc.Query(StringView(path));
        // 同一の不変条件を大量回数まわすので CHECK_QUIET(要件書 §8)
        CHECK_QUIET(v == doc.Query(StringView(path)));   // 決定的である
      }
    }

    CHECK(mock.AllocateCalls() == before);   // **1 回も増えない**
    CHECK(!mock.DoubleFree() && !mock.UnknownFree());
  }

  // -------------------------------------------------------------------------
  // T-46 境界
  // -------------------------------------------------------------------------

  void TestPathBoundaries() {
    GLFD::Test::BeginCase("T-46: empty path is the root; empty segments and '...' are refused");

    MockMemoryResource mock;
    Document           doc(&mock);
    CHECK(doc.Parse(StringView("{\"a\":{\"b\":1},\"arr\":[9]}"), ParseFlags::None).IsOk());

    // 空パス = ルート自身。**ポインタとして一致する**
    CHECK(doc.Query(StringView("")) == &doc.Root());
    CHECK(doc.Query(StringView()) == &doc.Root());   // Data() == nullptr の空ビュー

    // 空パスは部分木でも「その節自身」
    {
      const Value* const a = doc.Query(StringView("a"));
      CHECK(a != nullptr);
      if (a != nullptr) { CHECK(Query(*a, StringView("")) == a); }
    }

    // 空セグメントはすべて不正
    CHECK(doc.Query(StringView("/")) == nullptr);
    CHECK(doc.Query(StringView("/a")) == nullptr);       // 診断側は先頭区切りを出さない
    CHECK(doc.Query(StringView("a/")) == nullptr);
    CHECK(doc.Query(StringView("a//b")) == nullptr);
    // ネストした空キーの診断パスは "a/" になる(ルート直下の空キーだけが
    // ルートと同じ空文字列になる)。末尾の空セグメントとして落ちる
    {
      Document nestedEmptyKey(&mock);
      CHECK(nestedEmptyKey.Parse(StringView("{\"a\":{\"\":1}}"), ParseFlags::None).IsOk());
      CHECK(nestedEmptyKey.Query(StringView("a/")) == nullptr);
      CHECK(nestedEmptyKey.Root().Find(StringView("a"))->Find(StringView("")) != nullptr);
    }
    CHECK(doc.Query(StringView("//")) == nullptr);
    CHECK(doc.Query(StringView("arr//0")) == nullptr);

    // 截断されたパスは解決しない(誤って別のノードを返さない)。
    // **"..." が実在する JSON で確かめる。** 実在しない JSON では、印を無視する
    // 実装でも「キーが無い」で nullptr になり、ガードが効いているか分からない
    {
      Document dots(&mock);
      CHECK(dots.Parse(StringView("{\"...\":{\"b\":1},\"a\":{\"...\":{\"b\":2}}}"),
                       ParseFlags::None).IsOk());
      // どの位置に現れても引けない
      CHECK(dots.Query(StringView("...")) == nullptr);
      CHECK(dots.Query(StringView("a/...")) == nullptr);
      CHECK(dots.Query(StringView(".../b")) == nullptr);
      CHECK(dots.Query(StringView("a/.../b")) == nullptr);
      // DOM には在る = 上の nullptr は「キーが無いから」ではない
      CHECK(dots.Root().Find(StringView("...")) != nullptr);
      CHECK(dots.Query(StringView("a")) != nullptr);
    }

    // "." と ".." は予約しない = ただのキー。この JSON には無いので nullptr
    CHECK(doc.Query(StringView(".")) == nullptr);
    CHECK(doc.Query(StringView("..")) == nullptr);
    CHECK(doc.Query(StringView("....")) == nullptr);

    // パース前 / パース失敗後のルートは Null。空パスは Null を返し、それ以外は nullptr
    {
      Document empty(&mock);
      CHECK(empty.Query(StringView("")) == &empty.Root());
      CHECK(empty.Root().IsNull());
      CHECK(empty.Query(StringView("a")) == nullptr);
    }
    {
      Document broken(&mock);
      CHECK(!broken.Parse(StringView("{\"a\":"), ParseFlags::None).IsOk());
      CHECK(broken.Root().IsNull());   // R2-7: 部分構築の DOM は露出しない
      CHECK(broken.Query(StringView("a")) == nullptr);
    }
  }

  // -------------------------------------------------------------------------
  // T-46b 本物の截断パス(合成した "..." ではなく Materialize が作ったもの)
  // -------------------------------------------------------------------------

  /**
   * @brief `PathStack` が実際に截断したパスを `Query` に渡して `nullptr` を確かめる
   *
   * @details
   *  T-46 の `"a/..."` は**テストが手で書いた**文字列であり、区切りの付き方や
   *  末尾の `"/..."` が本物と一致している保証が無い。截断は 64 段を超えないと
   *  起きないので、境界を明示的に狙わないとこの形式は一度も検証されない
   *  (1-2b の T-6a と同じ話。インライン枠の外側は自分で狙う必要がある)。
   *
   *  @note `Detail::PathStack` を直に叩いているのは、**`ReadArchive` からは
   *        この形式を生成できない**ため。`ReadArchive::kMaxDepth` は
   *        `PathStack::kMaxDepth` と同じ 64 で、枠が先に尽きて
   *        `ContainerFailure`(the archive nesting is too deep)を報告し、
   *        パスが溢れる前に降下をやめる。截断の印は現状 `PathStack` を
   *        直接使う側だけが作れる。
   */
  void TestRealTruncatedPath() {
    GLFD::Test::BeginCase("T-46b: a genuinely truncated path from Materialize resolves to nullptr");

    MockMemoryResource mock;

    // --- 64 段より深く積んで截断させる ---------------------------------------
    GLFD::Json::JsonArena       arena(&mock);
    GLFD::Json::Detail::PathStack stack;

    char keys[GLFD::Json::Detail::PathStack::kMaxDepth + 1][8] = {};
    for (int i = 0; i <= static_cast<int>(GLFD::Json::Detail::PathStack::kMaxDepth); ++i) {
      std::snprintf(keys[i], sizeof(keys[i]), "s%d", i);
      stack.PushKey(StringView(keys[i]));
    }
    CHECK(stack.Truncated());
    CHECK(stack.Depth() == GLFD::Json::Detail::PathStack::kMaxDepth);

    const StringView truncated = stack.Materialize(arena);
    CHECK(truncated.Size() > 0);

    // 期待する形を独立に組み立てて突き合わせる("s0/s1/.../s63/...")
    {
      char   expected[1024];
      size_t offset = 0;
      for (int i = 0; i < static_cast<int>(GLFD::Json::Detail::PathStack::kMaxDepth); ++i) {
        offset += static_cast<size_t>(
          std::snprintf(expected + offset, sizeof(expected) - offset, "%ss%d",
                        (i == 0) ? "" : "/", i));
      }
      offset += static_cast<size_t>(
        std::snprintf(expected + offset, sizeof(expected) - offset, "/..."));

      CHECK(truncated.Size() == offset);
      CHECK(std::memcmp(truncated.Data(), expected, offset) == 0);
    }
    // 先頭に区切りは付かず、末尾は "/..." で終わる
    CHECK(truncated[0] == 's');
    CHECK(truncated.EndsWith(StringView("/...")));

    // --- そのパスが**全段実在する** JSON を組み立てる -------------------------
    // 実在しない JSON だと「キーが無いから nullptr」になり、截断の印を
    // 拒否したのかどうかが区別できない
    char   json[2048];
    size_t offset = 0;
    for (int i = 0; i < static_cast<int>(GLFD::Json::Detail::PathStack::kMaxDepth); ++i) {
      offset += static_cast<size_t>(
        std::snprintf(json + offset, sizeof(json) - offset, "{\"s%d\":", i));
    }
    // 最深部には "..." というキーを置く。ガードが無ければこれが引けてしまう
    offset += static_cast<size_t>(
      std::snprintf(json + offset, sizeof(json) - offset, "{\"...\":42}"));
    for (int i = 0; i < static_cast<int>(GLFD::Json::Detail::PathStack::kMaxDepth); ++i) {
      offset += static_cast<size_t>(
        std::snprintf(json + offset, sizeof(json) - offset, "}"));
    }

    Document doc(&mock);
    CHECK(doc.Parse(StringView(json, offset), ParseFlags::None).IsOk());

    // 截断の印を除いた 64 段は**すべて解決する**
    const StringView withoutMarker = truncated.SubStr(0, truncated.Size() - 4);
    const Value* const deepest = doc.Query(withoutMarker);
    CHECK(deepest != nullptr);
    if (deepest != nullptr) {
      CHECK(deepest->IsObject());
      CHECK(deepest->Find(StringView("...")) != nullptr);   // 最深部に "..." は在る
    }

    // **本物の截断パスは nullptr。** 上の 2 行があるので「キーが無いから」ではない
    CHECK(doc.Query(truncated) == nullptr);
  }

  /**
   * @brief T-46c: 実アーカイブは截断パスを出さない(枠が先に尽きる)
   *
   * @details
   *  T-46b が `PathStack` を直に叩いているのに対し、こちらは**端から端まで**の確認。
   *  1 形態(オブジェクトのネスト)だけ置いてある。押し場所ごとの網羅は
   *  `Detail::PathScope` の assert が Debug で常時見ているので、ここでは
   *  「枠が先に尽きる」という結論だけを Release でも固定する。
   */
  void TestArchiveNeverTruncates() {
    GLFD::Test::BeginCase("T-46c: a real load deeper than the frame limit reports no truncated path");

    constexpr int kLimit = static_cast<int>(GLFD::Json::Detail::PathStack::kMaxDepth);
    constexpr int kDepth = kLimit + 1;   // 枠の上限より 1 段深い

    char   json[4096];
    size_t offset = 0;
    for (int i = 0; i < kDepth; ++i) {
      offset += static_cast<size_t>(
        std::snprintf(json + offset, sizeof(json) - offset, "{\"n\":"));
    }
    offset += static_cast<size_t>(
      std::snprintf(json + offset, sizeof(json) - offset, "{\"leaf\":\"oops\"}"));
    for (int i = 0; i < kDepth; ++i) {
      offset += static_cast<size_t>(std::snprintf(json + offset, sizeof(json) - offset, "}"));
    }

    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);

    {
      QueryTest::Deep<kDepth> value;
      (void)LoadFromJson(value, StringView(json, offset), doc, ctx, ParseFlags::None);
    }

    CHECK(ctx.IssueCount() > 0u);

    int deepest       = 0;
    int truncated     = 0;
    int containerFail = 0;
    for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
      const ArchiveIssue& issue = ctx.Issues()[i];
      if (issue.path.EndsWith(StringView("..."))) { ++truncated; }
      if (issue.kind == ArchiveErrorKind::ContainerFailure) { ++containerFail; }

      int segments = (issue.path.Size() == 0) ? 0 : 1;
      for (size_t k = 0; k < issue.path.Size(); ++k) {
        if (issue.path[k] == '/') { ++segments; }
      }
      if (segments > deepest) { deepest = segments; }
    }

    // 枠が尽きたことは診断に出る
    CHECK(containerFail > 0);
    // **截断パスは 1 件も出ない**
    CHECK(truncated == 0);
    // パスはちょうど上限まで伸びて止まる(余裕は 0)
    CHECK(deepest == kLimit);
  }

  // -------------------------------------------------------------------------
  // T-47 制約の固定(**意図的な制約であってバグではない**)
  // -------------------------------------------------------------------------

  void TestUnaddressableKeys() {
    GLFD::Test::BeginCase("T-47: keys with '/', empty keys and '...' are deliberately unaddressable");

    MockMemoryResource mock;
    Document           doc(&mock);

    // どれも正当な JSON。診断パス形式 (R3-9) では表現できないだけ
    static const char kJson[] =
      "{\"a/b\":\"slash key\","
      " \"\":\"empty key\","
      " \"...\":\"dots key\","
      " \"a\":{\"b\":\"nested\"}}";
    CHECK(doc.Parse(StringView(kJson), ParseFlags::None).IsOk());

    // 4 つとも DOM には存在する
    CHECK(doc.Root().Find(StringView("a/b")) != nullptr);
    CHECK(doc.Root().Find(StringView("")) != nullptr);
    CHECK(doc.Root().Find(StringView("...")) != nullptr);
    CHECK(doc.Root().Find(StringView("a")) != nullptr);

    // (1) '/' を含むキーは区切りと区別できない。**ネストした方が引ける**
    {
      const Value* const v = doc.Query(StringView("a/b"));
      CHECK(v != nullptr);
      if (v != nullptr) {
        StringView text;
        CHECK(v->TryGetString(text));
        CHECK(ViewEquals(text, "nested"));            // "slash key" ではない
        CHECK(v == doc.Root().Find(StringView("a"))->Find(StringView("b")));
      }
    }

    // (2) 空キーはルートと区別が付かない。ルートが返る
    CHECK(doc.Query(StringView("")) == &doc.Root());
    CHECK(doc.Query(StringView("")) != doc.Root().Find(StringView("")));

    // (3) "..." は截断の印と衝突するので引けない
    CHECK(doc.Query(StringView("...")) == nullptr);
    CHECK(doc.Root().Find(StringView("...")) != nullptr);   // DOM には在る
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonQuery (2-1)");

  TestIssuePathRoundTrip();
  TestResolutionRules();
  TestStrictIndexParsing();
  TestQueryDoesNotAllocate();
  TestPathBoundaries();
  TestRealTruncatedPath();
  TestArchiveNeverTruncates();
  TestUnaddressableKeys();

  return GLFD::Test::Summarize();
}
