/**
 * @file  JsonArchiveContainersTests.cpp
 * @brief フェーズ 1-6: コンテナ型 (`T[N]` / `DynamicArray<T>` / `std::optional<T>`)
 *
 * @details
 *  対応する要件:
 *   - T-30 コンテナの往復
 *   - T-32 診断のパス(配列要素・ネスト・64 段の截断)
 *
 *  T-33(ヘッダ分割)と T-34(`/EH` 無しでの単体コンパイル)は、コンパイラの
 *  出力そのものが結果なのでビルド時に確認する。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include "Core/DynamicArray.h"
#include "Core/Json/JsonArchive.h"
#include "Core/Json/JsonArchiveContainers.h"
#include "Core/Json/JsonArchiveTypes.h"
#include "Core/Json/JsonDocument.h"
#include "Core/Json/JsonReadArchive.h"
#include "Core/Json/JsonWriteArchive.h"
#include "Core/Json/JsonWriter.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::DynamicArray;
using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::ArchiveIssue;
using GLFD::Json::Document;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::JsonWriter;
using GLFD::Json::ParseFlags;
using GLFD::Json::ReadArchive;
using GLFD::Json::WriteArchive;
using GLFD::Test::MockMemoryResource;

// ---------------------------------------------------------------------------
// テスト用のユーザー型
// ---------------------------------------------------------------------------

namespace ContainerTest {

  struct Point {
    float x = 0.0f;
    float y = 0.0f;
  };

  template <class Ar>
  void Serialize(Ar& ar, Point& v) {
    (void)ar.Member("x", v.x, 0.0f);
    (void)ar.Member("y", v.y, 0.0f);
  }

  /// `T[N]`(依存を増やさないので JsonArchiveTypes.h 側の対応)
  struct Transform {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float color[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
  };

  template <class Ar>
  void Serialize(Ar& ar, Transform& v) {
    (void)ar.Member("position", v.position);
    (void)ar.Member("color", v.color);
  }

  /// `DynamicArray<T>` / ネスト / `std::optional<T>`
  struct Level {
    DynamicArray<std::int32_t>              ids;
    DynamicArray<Point>                     points;
    DynamicArray<DynamicArray<std::int32_t>> matrix;
    std::optional<StringView>               nickname;
    std::optional<std::int32_t>             seed;

    explicit Level(GLFD::Memory::IMemoryResource* resource)
      : ids(resource), points(resource), matrix(resource) {
    }
  };

  /**
   * @brief 既定構築できるが、内側の `DynamicArray` がリソース未設定になる型
   *
   * @details
   *  `DynamicArray<Bag>` を読み込むと、`TryResize` が `Bag` を既定構築し、
   *  その `items` はコンストラクタの既定引数 `nullptr` を掴む。
   *  `Bag` は `SetResource` を持たないのでリソースの伝播も効かない。
   *  **フェーズ1のスコープ外**であり、踏んだときの挙動をテストで固定する。
   */
  struct Bag {
    DynamicArray<std::int32_t> items;
  };

  template <class Ar>
  void Serialize(Ar& ar, Bag& v) {
    (void)ar.Member("items", v.items);
  }

  template <class Ar>
  void Serialize(Ar& ar, Level& v) {
    (void)ar.Member("ids", v.ids);
    (void)ar.Member("points", v.points);
    (void)ar.Member("matrix", v.matrix);
    (void)ar.Member("nickname", v.nickname);
    (void)ar.Member("seed", v.seed);
  }

}

namespace {

  using CompactWriter = JsonWriter<JsonStringBuffer>;
  using TestArchive   = WriteArchive<CompactWriter>;

  [[nodiscard]] bool ViewEquals(StringView view, const char* expected) {
    const size_t length = std::strlen(expected);
    return view.Size() == length
           && (length == 0 || std::memcmp(view.Data(), expected, length) == 0);
  }

  [[nodiscard]] const ArchiveIssue* FindIssue(const ArchiveContext& ctx,
                                              ArchiveErrorKind kind, const char* path) {
    for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
      if (ctx.Issues()[i].kind == kind && ViewEquals(ctx.Issues()[i].path, path)) {
        return &ctx.Issues()[i];
      }
    }
    return nullptr;
  }

  template <class T>
  bool WriteToJson(T& value, JsonArena& arena, std::uint32_t version,
                   ArchiveFlags flags, JsonStringBuffer& out) {
    CompactWriter  writer(out);
    ArchiveContext ctx(arena, version, flags);
    TestArchive    ar(writer, ctx);
    ContainerTest::Serialize(ar, value);
    return ar.Finish();
  }

  // ---------------------------------------------------------------------------
  // 1. T[N]
  // ---------------------------------------------------------------------------

  void TestFixedArray() {
    GLFD::Test::BeginCase("T-30: T[N] round-trips and reports length mismatches");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // 往復
    {
      JsonStringBuffer buffer(arena);
      ContainerTest::Transform source;
      source.position[0] = 1.5f;
      source.position[1] = -2.25f;
      source.position[2] = 0.0f;
      source.color[3]    = 0.5f;

      CHECK(WriteToJson(source, arena, 1, ArchiveFlags::None, buffer));
      CHECK(ViewEquals(buffer.View(),
        "{\"$version\":1,\"position\":[1.5,-2.25,0.0],\"color\":[1.0,1.0,1.0,0.5]}"));

      Document doc(&mock);
      CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());
      ArchiveContext          ctx(arena);
      ContainerTest::Transform loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(!ctx.HasIssues());
      CHECK(loaded.position[0] == 1.5f);
      CHECK(loaded.position[1] == -2.25f);
      CHECK(loaded.color[3] == 0.5f);
    }

    // JSON が短い: 先頭だけ読み、残りは触らない
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"position\":[7,8]}"), ParseFlags::None).IsOk());
      ArchiveContext           ctx(arena);
      ContainerTest::Transform loaded;
      loaded.position[2] = 99.0f;   // 触られないことを見るための番人
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(loaded.position[0] == 7.0f);
      CHECK(loaded.position[1] == 8.0f);
      CHECK(loaded.position[2] == 99.0f);   // 残りは不変
      CHECK(FindIssue(ctx, ArchiveErrorKind::ArrayLengthMismatch, "position") != nullptr);
      CHECK(!ctx.HasFatal());               // Fatal ではない
    }

    // JSON が長い: 先頭 N 個だけ読み、超過分は無視
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"position\":[1,2,3,4,5]}"), ParseFlags::None).IsOk());
      ArchiveContext           ctx(arena);
      ContainerTest::Transform loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(loaded.position[0] == 1.0f);
      CHECK(loaded.position[2] == 3.0f);
      CHECK(FindIssue(ctx, ArchiveErrorKind::ArrayLengthMismatch, "position") != nullptr);
      CHECK(!ctx.HasFatal());
    }

    // 配列でない: TypeMismatch であって ArrayLengthMismatch ではない
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"position\":42}"), ParseFlags::None).IsOk());
      ArchiveContext           ctx(arena);
      ContainerTest::Transform loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "position") != nullptr);
      CHECK(FindIssue(ctx, ArchiveErrorKind::ArrayLengthMismatch, "position") == nullptr);
    }

    // StrictTypes でも ArrayLengthMismatch は Fatal にならない。
    // 「型には厳しく、長さには寛容に」を選べることがこの kind を分けた理由
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"position\":[1,2]}"), ParseFlags::None).IsOk());
      ArchiveContext           ctx(arena, 0, ArchiveFlags::StrictTypes);
      ContainerTest::Transform loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(ctx.IssueCount() == 1);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::ArrayLengthMismatch);
      CHECK(!ctx.HasFatal());
    }

    // 読み書きで長さが変わり得ること (ヘッダの @warning をテストで固定する)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"position\":[1,2]}"), ParseFlags::None).IsOk());
      ArchiveContext           ctx(arena);
      ContainerTest::Transform loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }

      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(loaded, arena, 1, ArchiveFlags::None, buffer));
      // [1, 2] が [1.0, 2.0, 0.0] になって書き戻る。意図した挙動
      CHECK(ViewEquals(buffer.View(),
        "{\"$version\":1,\"position\":[1.0,2.0,0.0],\"color\":[1.0,1.0,1.0,1.0]}"));
    }
  }

  // ---------------------------------------------------------------------------
  // 2. DynamicArray<T> / ネスト / optional
  // ---------------------------------------------------------------------------

  void TestDynamicArrayRoundTrip() {
    GLFD::Test::BeginCase("T-30: DynamicArray / nested containers / optional round-trip");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // 空
    {
      JsonStringBuffer      buffer(arena);
      ContainerTest::Level  source(&mock);
      CHECK(WriteToJson(source, arena, 1, ArchiveFlags::None, buffer));
      CHECK(ViewEquals(buffer.View(),
        "{\"$version\":1,\"ids\":[],\"points\":[],\"matrix\":[],"
        "\"nickname\":null,\"seed\":null}"));

      Document doc(&mock);
      CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      ContainerTest::Level loaded(&mock);
      loaded.nickname = StringView("stale");   // null で消えることを見る
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(!ctx.HasIssues());
      CHECK(loaded.ids.GetSize() == 0);
      CHECK(!loaded.nickname.has_value());
      CHECK(!loaded.seed.has_value());
    }

    // 1 要素 / 多要素 / ネスト / optional 値あり
    {
      ContainerTest::Level source(&mock);
      CHECK(source.ids.TryPushBack(10));
      CHECK(source.ids.TryPushBack(20));
      CHECK(source.ids.TryPushBack(30));
      CHECK(source.points.TryPushBack(ContainerTest::Point{ 1.0f, 2.0f }));

      DynamicArray<std::int32_t> row0(&mock);
      CHECK(row0.TryPushBack(1));
      CHECK(row0.TryPushBack(2));
      DynamicArray<std::int32_t> row1(&mock);
      CHECK(row1.TryPushBack(3));
      CHECK(source.matrix.TryPushBack(row0));
      CHECK(source.matrix.TryPushBack(row1));

      source.nickname = StringView("hero");
      source.seed     = 12345;

      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(source, arena, 1, ArchiveFlags::None, buffer));
      CHECK(ViewEquals(buffer.View(),
        "{\"$version\":1,\"ids\":[10,20,30],"
        "\"points\":[{\"x\":1.0,\"y\":2.0}],"
        "\"matrix\":[[1,2],[3]],"
        "\"nickname\":\"hero\",\"seed\":12345}"));

      Document doc(&mock);
      CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      ContainerTest::Level loaded(&mock);
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(!ctx.HasIssues());
      CHECK(loaded.ids.GetSize() == 3);
      CHECK(loaded.ids[0] == 10);
      CHECK(loaded.ids[2] == 30);
      CHECK(loaded.points.GetSize() == 1);
      CHECK(loaded.points[0].x == 1.0f);
      CHECK(loaded.points[0].y == 2.0f);
      CHECK(loaded.matrix.GetSize() == 2);
      // 中身が空のときに添字を取ると Release では assert が消えて範囲外読みになる。
      // 失敗しても後続の検証まで到達できるよう、必ず大きさを確かめてから触る
      if (loaded.matrix.GetSize() == 2) {
        CHECK(loaded.matrix[0].GetSize() == 2);
        CHECK(loaded.matrix[1].GetSize() == 1);
        if (loaded.matrix[0].GetSize() == 2) { CHECK(loaded.matrix[0][1] == 2); }
        if (loaded.matrix[1].GetSize() == 1) { CHECK(loaded.matrix[1][0] == 3); }
      }
      CHECK(loaded.nickname.has_value());
      CHECK(ViewEquals(*loaded.nickname, "hero"));
      CHECK(loaded.seed.has_value());
      CHECK(*loaded.seed == 12345);
    }

    // 既存の中身が縮む方向でも正しく揃うこと
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"ids\":[7]}"), ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      ContainerTest::Level loaded(&mock);
      CHECK(loaded.ids.TryResize(5));
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(loaded.ids.GetSize() == 1);
      CHECK(loaded.ids[0] == 7);
    }
  }

  // ---------------------------------------------------------------------------
  // 3. optional の 4 通り
  // ---------------------------------------------------------------------------

  void TestOptional() {
    GLFD::Test::BeginCase("T-30: optional handles value / null / missing / SkipDefaults");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // 値あり / null / 欠損
    struct Case {
      const char* json;
      bool        expectValue;
      std::int32_t expected;
    };
    const Case cases[] = {
      { "{\"seed\":5}",    true,  5 },
      { "{\"seed\":null}", false, 0 },
      { "{}",              false, 0 },
    };

    for (const Case& c : cases) {
      Document doc(&mock);
      CHECK_QUIET(doc.Parse(StringView(c.json), ParseFlags::None).IsOk());
      ArchiveContext              ctx(arena);
      std::optional<std::int32_t> value;
      {
        ReadArchive ar(doc.Root(), ctx);
        // 欠損で無効値にしたい場合は既定値つき Member を使う (R3-4 の一様な扱い)
        (void)ar.Member("seed", value, std::optional<std::int32_t>{});
      }
      CHECK_QUIET(value.has_value() == c.expectValue);
      CHECK_QUIET(!c.expectValue || *value == c.expected);
      CHECK_QUIET(!ctx.HasIssues());
    }
    ++GLFD::Test::g_checkCount;

    // 型が違う場合は値が入らず、呼び出し前の状態に戻る
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"seed\":\"x\"}"), ParseFlags::None).IsOk());
      ArchiveContext              ctx(arena);
      std::optional<std::int32_t> value;
      {
        ReadArchive ar(doc.Root(), ctx);
        CHECK(!ar.Member("seed", value));
      }
      CHECK(!value.has_value());   // emplace した分は巻き戻る
      CHECK(ctx.IssueCount() == 1);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
    }

    // 既に値を持っている optional は、読めなければ元の値のまま
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"seed\":\"x\"}"), ParseFlags::None).IsOk());
      ArchiveContext              ctx(arena);
      std::optional<std::int32_t> value = 77;
      {
        ReadArchive ar(doc.Root(), ctx);
        CHECK(!ar.Member("seed", value));
      }
      CHECK(value.has_value());
      CHECK(*value == 77);
    }

    // SkipDefaults を使えば省略できる(専用の機構は無い)
    {
      JsonStringBuffer            buffer(arena);
      CompactWriter               writer(buffer);
      ArchiveContext              ctx(arena, 1, ArchiveFlags::SkipDefaults);
      std::optional<std::int32_t> empty;
      std::optional<std::int32_t> filled = 3;
      {
        TestArchive ar(writer, ctx);
        (void)ar.Member("a", empty, std::optional<std::int32_t>{});
        (void)ar.Member("b", filled, std::optional<std::int32_t>{});
        CHECK(ar.Finish());
      }
      CHECK(ViewEquals(buffer.View(), "{\"$version\":1,\"b\":3}"));
    }
  }

  // ---------------------------------------------------------------------------
  // 4. TryResize の失敗 / アロケータ未設定
  // ---------------------------------------------------------------------------

  void TestContainerFailure() {
    GLFD::Test::BeginCase("T-30: TryResize failure is reported as ContainerFailure");

    MockMemoryResource docMock;
    Document           doc(&docMock);
    CHECK(doc.Parse(StringView("{\"ids\":[1,2,3]}"), ParseFlags::None).IsOk());

    // 確保失敗を注入する
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      ContainerTest::Level loaded(&mock);

      mock.SetFailAfter(mock.AllocateCalls());
      ArchiveContext ctx(arena);
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(ctx.HasFatal());
      CHECK(loaded.ids.GetSize() == 0);

      // ここでは**アリーナごと**確保が失敗しているため、Issue のパス文字列も
      // 組み立てられない (Materialize は確保失敗時に空を返す。診断のために
      // ロードを落とさない設計)。したがってパスでは照合できない。
      // 記録そのものが積めなかった場合は IssuesTruncated が立つ
      bool sawContainerFailure = false;
      for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
        if (ctx.Issues()[i].kind == ArchiveErrorKind::ContainerFailure) {
          sawContainerFailure = true;
        }
      }
      CHECK(sawContainerFailure || ctx.IssuesTruncated());
    }

    // アリーナを生かしたままコンテナの確保だけを失敗させる経路
    // (リソース未設定の DynamicArray)は、下の #ifdef NDEBUG 側で確認する。
    // Debug ではコンストラクタの assert が構築時点で発火して到達できない

#ifdef NDEBUG
    // アロケータ未設定の DynamicArray へ読み込む。
    // **Debug ではコンストラクタの assert(m_resource) が構築時点で発火するため、
    //   この経路は Release でしか実行できない。**
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);

      DynamicArray<std::int32_t> noResource;   // 既定引数 nullptr
      ArchiveContext             ctx(arena);
      {
        ReadArchive ar(doc.Root(), ctx);
        (void)ar.Member("ids", noResource);
      }
      // クラッシュせず ContainerFailure になること
      CHECK(ctx.HasFatal());
      CHECK(FindIssue(ctx, ArchiveErrorKind::ContainerFailure, "ids") != nullptr);
      CHECK(noResource.GetSize() == 0);
    }

    // **空配列はリソース未設定でも通る。** TryResize(0) は確保を伴わないため。
    // これは意図的な非対称であり、空配列で不要な ContainerFailure を出さない
    // ための挙動である。「なぜ空だけ通るのか」と後から直さないこと
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);

      Document emptyDoc(&mock);
      CHECK(emptyDoc.Parse(StringView("{\"ids\":[]}"), ParseFlags::None).IsOk());

      DynamicArray<std::int32_t> noResource;
      ArchiveContext             ctx(arena);
      {
        ReadArchive ar(emptyDoc.Root(), ctx);
        CHECK(ar.Member("ids", noResource));
      }
      CHECK(!ctx.HasFatal());
      CHECK(!ctx.HasIssues());
      CHECK(noResource.GetSize() == 0);
    }

    // **フェーズ1のスコープ外の境界を固定する。**
    // `DynamicArray` メンバを持つユーザー型を `DynamicArray` に入れると、
    // 外側の TryResize が内側の型を既定構築し、その中の DynamicArray は
    // リソース未設定になる。ユーザー型は SetResource を持たないので
    // 伝播も効かない。エンジンのアロケータ伝播の設計に属する話であり、
    // JSON 側では解決しない。
    //
    // 踏んだときに何が起きるかは決めてある:
    //   Release -> ContainerFailure (パスと detail で原因が分かる)
    //   Debug   -> DynamicArray のコンストラクタの assert で構築時点に落ちる
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);

      Document bagDoc(&mock);
      CHECK(bagDoc.Parse(StringView("{\"bags\":[{\"items\":[1,2]}]}"),
                         ParseFlags::None).IsOk());

      DynamicArray<ContainerTest::Bag> bags(&mock);
      ArchiveContext                   ctx(arena);
      {
        ReadArchive ar(bagDoc.Root(), ctx);
        (void)ar.Member("bags", bags);
      }

      CHECK(bags.GetSize() == 1);            // 外側は伸びる
      CHECK(ctx.HasFatal());                 // 内側が確保できない
      const ArchiveIssue* const issue =
        FindIssue(ctx, ArchiveErrorKind::ContainerFailure, "bags/0/items");
      CHECK(issue != nullptr);

      // detail からリソース未設定が原因だと分かること。
      // 「リサイズに失敗」だけでは気づけない
      if (issue != nullptr) {
        CHECK(std::strstr(issue->detail.Data(), "IMemoryResource") != nullptr);
      }
    }
#endif
  }

  // ---------------------------------------------------------------------------
  // 4b. リソースの伝播 (1-6 (A))
  // ---------------------------------------------------------------------------

  void TestResourcePropagation() {
    GLFD::Test::BeginCase("nested DynamicArray receives the outer array's resource");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // 追加した 2 メソッドそのものの検証。
    // (DynamicArrayTests ではなくここに置いてある。この API は入れ子のために
    //  1-6 で足したものであり、1-5a のスイートを 9 cases / 186 checks のまま
    //  保って T-35 の非回帰指標を残すため)
    {
      DynamicArray<std::int32_t> a(&mock);
      CHECK(a.GetResource() == &mock);

      MockMemoryResource other;
      a.SetResource(&other);
      CHECK(a.GetResource() == &other);
      CHECK(a.TryPushBack(1));
      CHECK(other.AllocateCalls() > 0);       // 新しいリソースから確保している
    }
    // Reserve 済みで空なら差し替えられる。予約した容量は解放される
    {
      MockMemoryResource first;
      MockMemoryResource second;
      DynamicArray<std::int32_t> a(&first);
      CHECK(a.TryReserve(32));
      CHECK(a.GetCapacity() == 32);
      CHECK(first.LiveBlockCount() == 1);

      a.SetResource(&second);
      CHECK(a.GetCapacity() == 0);            // 容量は失われる(文書化済み)
      CHECK(first.LiveBlockCount() == 0);     // **旧リソースへ返している**
      CHECK(!first.UnknownFree());
      CHECK(a.GetResource() == &second);
    }

    // 入れ子の往復(これが (A) で解決した本体)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"matrix\":[[1,2],[3]]}"), ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      ContainerTest::Level loaded(&mock);
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(!ctx.HasIssues());
      CHECK(loaded.matrix.GetSize() == 2);
      if (loaded.matrix.GetSize() == 2) {
        CHECK(loaded.matrix[0].GetResource() == &mock);   // 伝播している
        CHECK(loaded.matrix[1].GetResource() == &mock);
      }
    }

    // 縮小 -> 再拡大で、再構築された要素にもリソースが配られること。
    // construct_at が走る範囲だけに配る実装なので、ここが漏れると
    // 「2 回目の読み込みだけ ContainerFailure」という分かりにくい形で出る
    {
      ContainerTest::Level loaded(&mock);

      // 1 回目: 3 要素
      {
        Document doc(&mock);
        CHECK(doc.Parse(StringView("{\"matrix\":[[1],[2],[3]]}"),
                        ParseFlags::None).IsOk());
        ArchiveContext ctx(arena);
        {
          ReadArchive ar(doc.Root(), ctx);
          ContainerTest::Serialize(ar, loaded);
        }
        CHECK(!ctx.HasIssues());
        CHECK(loaded.matrix.GetSize() == 3);
      }

      // 2 回目: 1 要素へ縮小
      {
        Document doc(&mock);
        CHECK(doc.Parse(StringView("{\"matrix\":[[9]]}"), ParseFlags::None).IsOk());
        ArchiveContext ctx(arena);
        {
          ReadArchive ar(doc.Root(), ctx);
          ContainerTest::Serialize(ar, loaded);
        }
        CHECK(!ctx.HasIssues());
        CHECK(loaded.matrix.GetSize() == 1);
      }

      // 3 回目: 再び伸ばす。破棄された位置が construct_at を通り直すので、
      // そこにもリソースが配られていなければならない
      {
        Document doc(&mock);
        CHECK(doc.Parse(StringView("{\"matrix\":[[1],[2,2],[3,3,3],[4]]}"),
                        ParseFlags::None).IsOk());
        ArchiveContext ctx(arena);
        {
          ReadArchive ar(doc.Root(), ctx);
          ContainerTest::Serialize(ar, loaded);
        }
        CHECK(!ctx.HasIssues());
        CHECK(loaded.matrix.GetSize() == 4);
        if (loaded.matrix.GetSize() == 4) {
          CHECK(loaded.matrix[2].GetSize() == 3);
          CHECK(loaded.matrix[3].GetSize() == 1);
          CHECK(loaded.matrix[3].GetResource() == &mock);
        }
      }

      // Clear() を挟んでから、より長い配列を読む
      {
        loaded.matrix.Clear();
        CHECK(loaded.matrix.GetSize() == 0);

        Document doc(&mock);
        CHECK(doc.Parse(StringView("{\"matrix\":[[1],[2],[3],[4],[5]]}"),
                        ParseFlags::None).IsOk());
        ArchiveContext ctx(arena);
        {
          ReadArchive ar(doc.Root(), ctx);
          ContainerTest::Serialize(ar, loaded);
        }
        CHECK(!ctx.HasIssues());
        CHECK(loaded.matrix.GetSize() == 5);
        if (loaded.matrix.GetSize() == 5) {
          CHECK(loaded.matrix[4].GetSize() == 1);
          CHECK(loaded.matrix[4][0] == 5);
        }
      }
    }
  }

  // ---------------------------------------------------------------------------
  // 5. T-32 診断のパス
  // ---------------------------------------------------------------------------

  void TestDiagnosticPaths() {
    GLFD::Test::BeginCase("T-32: array and nested-container paths");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    // 配列要素 + ネストしたユーザー型 -> "points/1/y"
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView(
        "{\"points\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":\"bad\"}]}"),
        ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      ContainerTest::Level loaded(&mock);
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "points/1/y") != nullptr);
      CHECK(loaded.points.GetSize() == 2);
      CHECK(loaded.points[0].x == 1.0f);
      CHECK(loaded.points[1].x == 3.0f);   // 1 件失敗しても読み続ける
    }

    // ネストしたコンテナ -> "matrix/2/1"
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"matrix\":[[1],[2],[3,\"bad\"]]}"),
                      ParseFlags::None).IsOk());
      ArchiveContext       ctx(arena);
      ContainerTest::Level loaded(&mock);
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(ctx.IssueCount() == 1);
      CHECK(ViewEquals(ctx.Issues()[0].path, "matrix/2/1"));
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
    }

    // T[N] のネスト -> "position/1"
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"position\":[1,\"bad\",3]}"),
                      ParseFlags::None).IsOk());
      ArchiveContext           ctx(arena);
      ContainerTest::Transform loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ContainerTest::Serialize(ar, loaded);
      }
      CHECK(FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "position/1") != nullptr);
      CHECK(loaded.position[0] == 1.0f);
      CHECK(loaded.position[2] == 3.0f);
    }
  }

  // ---------------------------------------------------------------------------
  // 6. 静的検証
  // ---------------------------------------------------------------------------

  void TestStaticProperties() {
    GLFD::Test::BeginCase("static properties of the container dispatch");

    namespace D = GLFD::Json::Detail;

    static_assert(D::HasCustomSerializer<ReadArchive, DynamicArray<std::int32_t>>);
    static_assert(D::HasCustomSerializer<ReadArchive, std::optional<std::int32_t>>);
    static_assert(D::HasCustomSerializer<ReadArchive, float[3]>);

    // ネストしても再帰的にディスパッチできる
    static_assert(D::Archivable<ReadArchive, DynamicArray<DynamicArray<std::int32_t>>>);
    static_assert(D::Archivable<ReadArchive, DynamicArray<ContainerTest::Point>>);
    static_assert(D::Archivable<TestArchive, DynamicArray<DynamicArray<std::int32_t>>>);

    // T[N] はスカラ表には無い(JsonSerializer の部分特殊化で拾われる)
    static_assert(!D::IsArchiveScalar<float[3]>);

    // アーカイブに追加した口
    static_assert(std::is_same_v<decltype(std::declval<ReadArchive&>().IsNull()), bool>);

    ++GLFD::Test::g_checkCount;
    CHECK(true);
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonArchiveContainers (1-6)");

  TestFixedArray();
  TestDynamicArrayRoundTrip();
  TestOptional();
  TestContainerFailure();
  TestResourcePropagation();
  TestDiagnosticPaths();
  TestStaticProperties();

  return GLFD::Test::Summarize();
}
