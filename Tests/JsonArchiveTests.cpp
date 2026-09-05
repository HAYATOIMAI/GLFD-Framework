/**
 * @file  JsonArchiveTests.cpp
 * @brief フェーズ 1-5b: 統一アーカイブ層のテスト
 *
 * @details
 *  対応する要件:
 *   - T-4  バージョン互換(本フェーズの中核テスト)
 *   - T-22 往復 (struct -> JSON -> struct) と、SAX 列による出力順の検証 (R3-3)
 *   - T-24 診断品質(パス形式・各 kind・StrictTypes・既定の静かな無視)
 *   - T-25 確保失敗の総当たり注入(読み・書き双方)
 *
 *  T-23(ヘッダ依存の機械的検証)と T-26(`/EH` 無しでの単体コンパイル)は
 *  コンパイラの出力そのものが結果なので、テスト実行ではなくビルド時に確認する。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 *        日本語の内容はバイト配列で組み立てる。
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "Core/Json/JsonArchive.h"
#include "Core/Json/JsonArchiveTypes.h"
#include "Core/Json/JsonDocument.h"
#include "Core/Json/JsonReadArchive.h"
#include "Core/Json/JsonWriteArchive.h"
#include "Core/Json/JsonWriter.h"

#include "JsonTraceHandler.h"
#include "MockMemoryResource.h"
#include "TestHarness.h"

// T-28 で目標ファイルが作られないことを確かめるために使う。
// Win32 の利用はここだけで、アーカイブ層自体は Windows を知らない
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <cstdio>
#include <windows.h>

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

namespace ArchiveTest {

  struct Stats {
    std::int32_t hp = 0;
    std::int32_t mp = 0;
  };

  template <class Ar>
  void Serialize(Ar& ar, Stats& v) {
    (void)ar.Member("hp", v.hp, 100);
    (void)ar.Member("mp", v.mp, 50);
  }

  struct Weapon {
    std::int32_t id     = 0;
    float        damage = 0.0f;
    StringView   name;
  };

  template <class Ar>
  void Serialize(Ar& ar, Weapon& v) {
    (void)ar.Member("id", v.id, 0);
    (void)ar.Member("damage", v.damage, 1.0f);
    (void)ar.Member("name", v.name);
  }

  struct Player {
    Stats         stats;
    double        speed = 0.0;
    bool          alive = false;
    std::uint8_t  level = 0;
    StringView    name;
    Weapon        weapon;
  };

  /// R3-3: この呼び出し順がそのまま出力順になる。並べ替えてはならない
  template <class Ar>
  void Serialize(Ar& ar, Player& v) {
    (void)ar.Member("stats", v.stats);
    (void)ar.Member("speed", v.speed, 1.0);
    (void)ar.Member("alive", v.alive, false);
    (void)ar.Member("level", v.level, static_cast<std::uint8_t>(1));
    (void)ar.Member("name", v.name);
    (void)ar.Member("weapon", v.weapon);
  }

  /// 固定長のコンテナ。配列プリミティブ(論点3)の検証用
  struct IntList {
    static constexpr std::uint32_t kCapacity = 8;
    std::int32_t  items[kCapacity] = {};
    std::uint32_t count = 0;
  };

  struct Inventory {
    IntList slots;
  };

  template <class Ar>
  void Serialize(Ar& ar, Inventory& v) {
    (void)ar.Member("slots", v.slots);
  }

  /// バージョン分岐 (R3-8) の検証用
  struct Versioned {
    std::int32_t oldField = 0;
    std::int32_t newField = 0;
  };

  template <class Ar>
  void Serialize(Ar& ar, Versioned& v) {
    (void)ar.Member("oldField", v.oldField, 0);
    if (ar.Version() >= 3u) {
      (void)ar.Member("newField", v.newField, 0);
    }
  }

  /// RequiredMember (R3-6) の検証用
  struct Mandatory {
    std::int32_t must   = 0;
    std::int32_t option = 0;
  };

  template <class Ar>
  void Serialize(Ar& ar, Mandatory& v) {
    (void)ar.RequiredMember("must", v.must);
    (void)ar.Member("option", v.option, 7);
  }

}

// `IntList` は配列として出したいので、オブジェクトで包まれない経路を使う。
// これが R3-2 の逃げ道 (JsonSerializer<T> 特殊化) の実用例になっている
namespace GLFD::Json {

  template <>
  struct JsonSerializer<ArchiveTest::IntList> {
    template <class Ar>
    static bool Serialize(Ar& ar, ArchiveTest::IntList& v) {
      std::uint32_t n = ar.BeginArray(v.count);
      if constexpr (Ar::IsReading()) {
        if (n > ArchiveTest::IntList::kCapacity) {
          // 容量超過は「JSON に要素を書きすぎた」であって確保失敗ではない。
          // ContainerFailure は常に Fatal なので、手書き config の書きすぎが
          // ロード失敗になってしまう (1-6 で ArrayLengthMismatch へ変更)
          ar.ReportArrayLengthMismatch();
          n = ArchiveTest::IntList::kCapacity;
        }
        v.count = n;
      }
      for (std::uint32_t i = 0; i < n; ++i) {
        (void)ar.Element(v.items[i]);
      }
      return ar.EndArray();
    }
  };

}

namespace {

  using CompactWriter = JsonWriter<JsonStringBuffer>;
  using TestArchive   = WriteArchive<CompactWriter>;

  // ---------------------------------------------------------------------------
  // 補助
  // ---------------------------------------------------------------------------

  /// 値を JSON へ書き出す。将来の SaveToJson (§5.5) の最小版
  template <class T>
  bool WriteToJson(T& value, JsonArena& arena, std::uint32_t version,
                   ArchiveFlags flags, JsonStringBuffer& out) {
    CompactWriter  writer(out);
    ArchiveContext ctx(arena, version, flags);
    TestArchive    ar(writer, ctx);
    ArchiveTest::Serialize(ar, value);
    return ar.Finish();
  }

  [[nodiscard]] bool TraceEquals(StringView trace, const char* expected) {
    const size_t length = std::strlen(expected);
    return trace.Size() == length
           && (length == 0 || std::memcmp(trace.Data(), expected, length) == 0);
  }

  [[nodiscard]] bool ViewEquals(StringView view, const char* expected) {
    return TraceEquals(view, expected);
  }

  /// 出力された JSON を SAX へ流してイベント列を得る (R3-3 の直接検証)
  [[nodiscard]] bool SaxTraceEquals(StringView json, JsonArena& arena, const char* expected) {
    GLFD::Test::JsonTraceHandler handler;
    GLFD::Json::JsonReader       reader(arena);
    const GLFD::Json::ParseError error = reader.Parse(json, handler, ParseFlags::None);
    if (!error.IsOk() || handler.Overflowed()) {
      return false;
    }
    return TraceEquals(handler.Trace(), expected);
  }

  [[nodiscard]] const ArchiveIssue* FindIssue(const ArchiveContext& ctx,
                                              ArchiveErrorKind kind, const char* path) {
    for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
      const ArchiveIssue& issue = ctx.Issues()[i];
      if (issue.kind == kind && ViewEquals(issue.path, path)) {
        return &ctx.Issues()[i];
      }
    }
    return nullptr;
  }

  // ---------------------------------------------------------------------------
  // 1. 書き出しの基本形と出力順 (R3-3 / R3-7)
  // ---------------------------------------------------------------------------

  void TestWriteShape() {
    GLFD::Test::BeginCase("WriteArchive emits $version first and preserves Member order");

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);

    ArchiveTest::Player player;
    player.stats.hp    = 120;
    player.stats.mp    = 30;
    player.speed       = 5.5;
    player.alive       = true;
    player.level       = 9;
    player.name        = StringView("hero");
    player.weapon.id   = 3;
    player.weapon.damage = 12.5f;
    player.weapon.name = StringView("sword");

    CHECK(WriteToJson(player, arena, 2, ArchiveFlags::None, buffer));
    CHECK(!buffer.HasFailed());

    // ルートに "$version" が必ず出る (R3-7)
    CHECK(ViewEquals(buffer.View(),
      "{\"$version\":2,"
      "\"stats\":{\"hp\":120,\"mp\":30},"
      "\"speed\":5.5,"
      "\"alive\":true,"
      "\"level\":9,"
      "\"name\":\"hero\","
      "\"weapon\":{\"id\":3,\"damage\":12.5,\"name\":\"sword\"}}"));

    // SAX へ流し、Member の呼び出し順とイベント順が一致することを直接見る。
    // 1-3 / 1-4 で SAX 列の厳密比較が 2 回バグを捕らえているため、ここでも使う
    CHECK(SaxTraceEquals(buffer.View(), arena,
      "{k($version)i(2)"
      "k(stats){k(hp)i(120)k(mp)i(30)}2"
      "k(speed)d(5.5)"
      "k(alive)t"
      "k(level)i(9)"
      "k(name)s(hero)"
      "k(weapon){k(id)i(3)k(damage)d(12.5)k(name)s(sword)}3"
      "}7"));
  }

  // ---------------------------------------------------------------------------
  // 2. T-22 往復
  // ---------------------------------------------------------------------------

  void TestRoundTrip() {
    GLFD::Test::BeginCase("T-22: struct -> JSON -> struct keeps every field");

    // 日本語の名前。C5297 を避けるためバイト配列で組み立てる
    // (UTF-8: E3 83 8F E3 82 A4 E3 83 A9 = katakana HA-I-RA)
    static const unsigned char kNameBytes[] = { 0xE3, 0x83, 0x8F, 0xE3, 0x82, 0xA4,
                                                0xE3, 0x83, 0xA9 };
    const StringView japaneseName(reinterpret_cast<const char*>(kNameBytes),
                                  sizeof(kNameBytes));

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);

    ArchiveTest::Player source;
    source.stats.hp      = -32768;
    source.stats.mp      = 2147483647;
    source.speed         = 0.1 + 0.2;          // 二進で割り切れない値
    source.alive         = true;
    source.level         = 255;                // uint8 の上限
    source.name          = japaneseName;
    source.weapon.id     = -1;
    source.weapon.damage = 3.4028234663852886e+38f;   // FLT_MAX
    source.weapon.name   = StringView("");            // 空文字列

    CHECK(WriteToJson(source, arena, 7, ArchiveFlags::None, buffer));

    Document doc(&mock);
    CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());

    JsonArena          readArena(&mock);
    ArchiveContext     ctx(readArena);
    ArchiveTest::Player loaded;
    {
      ReadArchive ar(doc.Root(), ctx);
      ArchiveTest::Serialize(ar, loaded);
    }

    CHECK(!ctx.HasFatal());
    CHECK(!ctx.HasIssues());
    CHECK(ctx.Version() == 7u);

    CHECK(loaded.stats.hp == source.stats.hp);
    CHECK(loaded.stats.mp == source.stats.mp);
    CHECK(loaded.speed == source.speed);              // double は bit 一致すること
    CHECK(loaded.alive == source.alive);
    CHECK(loaded.level == source.level);
    CHECK(loaded.weapon.id == source.weapon.id);
    CHECK(loaded.weapon.damage == source.weapon.damage);

    CHECK(loaded.name.Size() == japaneseName.Size());
    CHECK(std::memcmp(loaded.name.Data(), japaneseName.Data(), japaneseName.Size()) == 0);
    CHECK(loaded.weapon.name.Empty());

    // 読み取った文字列はアリーナへ複製されている。DOM を捨てても有効であること
    CHECK(loaded.name.Data() != japaneseName.Data());
    doc.Clear();
    CHECK(std::memcmp(loaded.name.Data(), japaneseName.Data(), japaneseName.Size()) == 0);
  }

  // ---------------------------------------------------------------------------
  // 3. T-4 バージョン互換(中核)
  // ---------------------------------------------------------------------------

  void TestVersionCompatibility() {
    GLFD::Test::BeginCase("T-4: missing -> default, unknown -> ignored, required -> fatal");

    MockMemoryResource mock;

    // (1) 新フィールド追加後に旧データを読む -> デフォルト値が入る (R3-4)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"$version\":1,\"hp\":42}"), ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      ArchiveTest::Stats stats;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, stats);
      }
      CHECK(stats.hp == 42);
      CHECK(stats.mp == 50);          // 欠損 -> 既定値
      CHECK(!ctx.HasFatal());
      CHECK(!ctx.HasIssues());        // 欠損は Issue にならない
    }

    // (2) フィールド削除後に旧データを読む -> 未知フィールドとして無視 (R3-5)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"$version\":1,\"hp\":10,\"mp\":20,\"removed\":true}"),
                      ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);      // ReportUnknown を指定しない = 既定の静かな無視
      ArchiveTest::Stats stats;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, stats);
      }
      CHECK(stats.hp == 10);
      CHECK(stats.mp == 20);
      CHECK(!ctx.HasFatal());
      CHECK(!ctx.HasIssues());        // 記録されないこと (T-24)
    }

    // (3) RequiredMember の欠損 -> HasFatal() (R3-6)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"$version\":1,\"option\":3}"), ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      ArchiveTest::Mandatory value;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, value);
      }
      CHECK(ctx.HasFatal());
      CHECK(ctx.IssueCount() == 1u);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::MissingRequired);
      CHECK(ViewEquals(ctx.Issues()[0].path, "must"));
      CHECK(value.must == 0);         // 失敗時は触らない
      CHECK(value.option == 3);       // 他のメンバは読めている
    }

    // (4) RequiredMember が存在すれば Fatal にならない
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"must\":5}"), ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      ArchiveTest::Mandatory value;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, value);
      }
      CHECK(!ctx.HasFatal());
      CHECK(value.must == 5);
      CHECK(value.option == 7);       // 既定値
      CHECK(ctx.Version() == 0u);     // "$version" 不在 -> 0 (R3-7)
    }
  }

  // ---------------------------------------------------------------------------
  // 4. R3-8 バージョン分岐が読み書き双方で効くこと
  // ---------------------------------------------------------------------------

  void TestVersionBranching() {
    GLFD::Test::BeginCase("R3-8: ar.Version() branches on both the read and write side");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    ArchiveTest::Versioned value;
    value.oldField = 1;
    value.newField = 2;

    // 書き: version 2 では newField を出さない
    {
      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(value, arena, 2, ArchiveFlags::None, buffer));
      CHECK(ViewEquals(buffer.View(), "{\"$version\":2,\"oldField\":1}"));
    }

    // 書き: version 3 では出す
    {
      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(value, arena, 3, ArchiveFlags::None, buffer));
      CHECK(ViewEquals(buffer.View(), "{\"$version\":3,\"oldField\":1,\"newField\":2}"));
    }

    // 読み: version 2 のデータでは newField を読まないので初期値のまま
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"$version\":2,\"oldField\":9,\"newField\":99}"),
                      ParseFlags::None).IsOk());
      ArchiveContext ctx(arena);
      ArchiveTest::Versioned loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, loaded);
      }
      CHECK(ctx.Version() == 2u);
      CHECK(loaded.oldField == 9);
      CHECK(loaded.newField == 0);
    }

    // 読み: version 3 なら読む
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"$version\":3,\"oldField\":9,\"newField\":99}"),
                      ParseFlags::None).IsOk());
      ArchiveContext ctx(arena);
      ArchiveTest::Versioned loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, loaded);
      }
      CHECK(ctx.Version() == 3u);
      CHECK(loaded.newField == 99);
    }
  }

  // ---------------------------------------------------------------------------
  // 5. 配列プリミティブ (論点3)
  // ---------------------------------------------------------------------------

  void TestArrayPrimitives() {
    GLFD::Test::BeginCase("BeginArray / Element / EndArray round-trip and path indices");

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);

    ArchiveTest::Inventory inventory;
    inventory.slots.count = 3;
    inventory.slots.items[0] = 10;
    inventory.slots.items[1] = 20;
    inventory.slots.items[2] = 30;

    CHECK(WriteToJson(inventory, arena, 1, ArchiveFlags::None, buffer));
    CHECK(ViewEquals(buffer.View(), "{\"$version\":1,\"slots\":[10,20,30]}"));
    CHECK(SaxTraceEquals(buffer.View(), arena, "{k($version)i(1)k(slots)[i(10)i(20)i(30)]3}2"));

    Document doc(&mock);
    CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());

    ArchiveContext ctx(arena);
    ArchiveTest::Inventory loaded;
    {
      ReadArchive ar(doc.Root(), ctx);
      ArchiveTest::Serialize(ar, loaded);
    }
    CHECK(!ctx.HasFatal());
    CHECK(loaded.slots.count == 3u);
    CHECK(loaded.slots.items[0] == 10);
    CHECK(loaded.slots.items[2] == 30);

    // 空配列
    {
      Document empty(&mock);
      CHECK(empty.Parse(StringView("{\"slots\":[]}"), ParseFlags::None).IsOk());
      ArchiveContext emptyCtx(arena);
      ArchiveTest::Inventory value;
      value.slots.count = 5;
      {
        ReadArchive ar(empty.Root(), emptyCtx);
        ArchiveTest::Serialize(ar, value);
      }
      CHECK(!emptyCtx.HasFatal());
      CHECK(value.slots.count == 0u);
    }

    // 配列の位置に配列でないものが来た場合 -> TypeMismatch、EndArray は釣り合う
    {
      Document wrong(&mock);
      CHECK(wrong.Parse(StringView("{\"slots\":123}"), ParseFlags::None).IsOk());
      ArchiveContext wrongCtx(arena);
      ArchiveTest::Inventory value;
      {
        ReadArchive ar(wrong.Root(), wrongCtx);
        ArchiveTest::Serialize(ar, value);
      }
      CHECK(wrongCtx.IssueCount() == 1u);
      CHECK(wrongCtx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
      CHECK(ViewEquals(wrongCtx.Issues()[0].path, "slots"));
      CHECK(!wrongCtx.HasFatal());    // StrictTypes 未指定なので Fatal ではない
    }
  }

  // ---------------------------------------------------------------------------
  // 6. T-24 診断品質
  // ---------------------------------------------------------------------------

  void TestDiagnostics() {
    GLFD::Test::BeginCase("T-24: issue kinds, nested paths and array element paths");

    MockMemoryResource mock;

    // ネストしたユーザー型のパス "weapon/damage"
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView(
        "{\"stats\":{\"hp\":\"x\"},\"weapon\":{\"damage\":\"y\"},\"level\":300}"),
        ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      ArchiveTest::Player player;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, player);
      }

      CHECK(FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "stats/hp") != nullptr);
      CHECK(FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "weapon/damage") != nullptr);
      // 300 は uint8 に収まらない -> 切り詰めずに RangeOverflow (R3-11)
      CHECK(FindIssue(ctx, ArchiveErrorKind::RangeOverflow, "level") != nullptr);
      CHECK(player.level == 0);       // 失敗時は out 不変
      CHECK(!ctx.HasFatal());         // どれも StrictTypes 無しでは Fatal ではない

      // path / detail はヌル終端されている (ログへ流せることが契約)
      const ArchiveIssue* const issue =
        FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "stats/hp");
      CHECK(issue != nullptr);
      if (issue != nullptr) {
        CHECK(issue->path.Data()[issue->path.Size()] == '\0');
        CHECK(issue->detail.Data()[issue->detail.Size()] == '\0');
      }
    }

    // 配列要素のパス "slots/1"
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"slots\":[1,\"bad\",3]}"), ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      ArchiveTest::Inventory inventory;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, inventory);
      }
      CHECK(ctx.IssueCount() == 1u);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
      CHECK(ViewEquals(ctx.Issues()[0].path, "slots/1"));
      CHECK(inventory.slots.items[0] == 1);
      CHECK(inventory.slots.items[2] == 3);   // 1 件失敗しても読み続ける (論点6)
    }

    // ReportUnknown 指定時のみ未知フィールドが記録される (R3-5)
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView(
        "{\"$version\":1,\"hp\":1,\"mp\":2,\"legacyA\":0,\"legacyB\":0}"),
        ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena, 0, ArchiveFlags::ReportUnknown);
      ArchiveTest::Stats stats;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, stats);
      }
      CHECK(ctx.IssueCount() == 2u);
      CHECK(FindIssue(ctx, ArchiveErrorKind::UnknownField, "legacyA") != nullptr);
      CHECK(FindIssue(ctx, ArchiveErrorKind::UnknownField, "legacyB") != nullptr);
      // 予約キー "$version" は未知フィールドとして報告しない (R3-10)
      CHECK(FindIssue(ctx, ArchiveErrorKind::UnknownField, "$version") == nullptr);
      CHECK(!ctx.HasFatal());
    }

    // ネストしたオブジェクトの未知フィールドもパス付きで報告される
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"stats\":{\"hp\":1,\"mp\":2,\"stray\":3}}"),
                      ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena, 0, ArchiveFlags::ReportUnknown);
      ArchiveTest::Player player;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, player);
      }
      CHECK(FindIssue(ctx, ArchiveErrorKind::UnknownField, "stats/stray") != nullptr);
    }

    // StrictTypes で型不一致が Fatal になる
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"hp\":\"x\"}"), ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena, 0, ArchiveFlags::StrictTypes);
      ArchiveTest::Stats stats;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, stats);
      }
      CHECK(ctx.IssueCount() == 1u);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
      CHECK(ctx.HasFatal());
    }

    // ルートがオブジェクトでない場合は StrictTypes 抜きでも Fatal
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("[1,2,3]"), ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      ArchiveTest::Stats stats;
      stats.hp = 5;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, stats);
      }
      CHECK(ctx.HasFatal());
      CHECK(ctx.IssueCount() == 1u);
      CHECK(ctx.Issues()[0].kind == ArchiveErrorKind::TypeMismatch);
      CHECK(ViewEquals(ctx.Issues()[0].path, ""));
      CHECK(stats.hp == 5);           // 既定値の代入も起きない
    }

    // "$version" が壊れている場合は黙って 0 にせず報告する
    {
      Document doc(&mock);
      CHECK(doc.Parse(StringView("{\"$version\":\"two\",\"hp\":1}"),
                      ParseFlags::None).IsOk());

      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      ArchiveTest::Stats stats;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, stats);
      }
      CHECK(ctx.Version() == 0u);
      CHECK(FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "$version") != nullptr);
    }
  }

  // ---------------------------------------------------------------------------
  // 7. SkipDefaults (論点8)
  // ---------------------------------------------------------------------------

  void TestSkipDefaults() {
    GLFD::Test::BeginCase("SkipDefaults omits matching values; non-comparable types are kept");

    MockMemoryResource mock;
    JsonArena          arena(&mock);

    ArchiveTest::Weapon weapon;
    weapon.id     = 0;      // 既定値と一致
    weapon.damage = 1.0f;   // 既定値と一致
    weapon.name   = StringView("axe");

    {
      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(weapon, arena, 1, ArchiveFlags::None, buffer));
      CHECK(ViewEquals(buffer.View(),
                       "{\"$version\":1,\"id\":0,\"damage\":1.0,\"name\":\"axe\"}"));
    }
    {
      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(weapon, arena, 1, ArchiveFlags::SkipDefaults, buffer));
      // 既定値と一致する id / damage は消え、既定値を持たない name は残る
      CHECK(ViewEquals(buffer.View(), "{\"$version\":1,\"name\":\"axe\"}"));
    }

    // 省略されたフィールドは、読み側で既定値として復元される (R3-4)
    {
      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(weapon, arena, 1, ArchiveFlags::SkipDefaults, buffer));

      Document doc(&mock);
      CHECK(doc.Parse(buffer.View(), ParseFlags::None).IsOk());
      ArchiveContext ctx(arena);
      ArchiveTest::Weapon loaded;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, loaded);
      }
      CHECK(!ctx.HasFatal());
      CHECK(loaded.id == 0);
      CHECK(loaded.damage == 1.0f);
      CHECK(ViewEquals(loaded.name, "axe"));
    }

    // 一致しない値は SkipDefaults でも出力される
    {
      ArchiveTest::Weapon other = weapon;
      other.id = 42;
      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(other, arena, 1, ArchiveFlags::SkipDefaults, buffer));
      CHECK(ViewEquals(buffer.View(), "{\"$version\":1,\"id\":42,\"name\":\"axe\"}"));
    }
  }

  // ---------------------------------------------------------------------------
  // 8. 範囲チェック (R3-11)
  // ---------------------------------------------------------------------------

  struct Widths {
    std::int8_t   i8  = 0;
    std::uint16_t u16 = 0;
    std::int64_t  i64 = 0;
    float         f   = 0.0f;
  };

  void ReadWidths(const char* json, MockMemoryResource& mock,
                  Widths& out, ArchiveContext& ctx) {
    Document doc(&mock);
    CHECK_QUIET(doc.Parse(StringView(json), ParseFlags::None).IsOk());
    ReadArchive ar(doc.Root(), ctx);
    (void)ar.Member("i8", out.i8);
    (void)ar.Member("u16", out.u16);
    (void)ar.Member("i64", out.i64);
    (void)ar.Member("f", out.f);
  }

  void TestRangeChecks() {
    GLFD::Test::BeginCase("R3-11: numbers are range-checked, never silently truncated");

    MockMemoryResource mock;

    // 範囲内
    {
      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      Widths w;
      ReadWidths("{\"i8\":-128,\"u16\":65535,\"i64\":9223372036854775807,\"f\":0.5}",
                 mock, w, ctx);
      CHECK(!ctx.HasIssues());
      CHECK(w.i8 == -128);
      CHECK(w.u16 == 65535);
      CHECK(w.i64 == (std::numeric_limits<std::int64_t>::max)());
      CHECK(w.f == 0.5f);
    }

    // 範囲外は全て RangeOverflow。out は不変
    {
      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      Widths w;
      ReadWidths("{\"i8\":-129,\"u16\":65536,\"i64\":1.5,\"f\":true}", mock, w, ctx);
      CHECK(FindIssue(ctx, ArchiveErrorKind::RangeOverflow, "i8") != nullptr);
      CHECK(FindIssue(ctx, ArchiveErrorKind::RangeOverflow, "u16") != nullptr);
      CHECK(FindIssue(ctx, ArchiveErrorKind::RangeOverflow, "i64") != nullptr);
      // bool は数値ではないので範囲外ではなく型違い
      CHECK(FindIssue(ctx, ArchiveErrorKind::TypeMismatch, "f") != nullptr);
      CHECK(w.i8 == 0);
      CHECK(w.u16 == 0);
      CHECK(w.i64 == 0);
      CHECK(w.f == 0.0f);
    }

    // 負数を符号なしへ読むのは範囲外
    {
      JsonArena      arena(&mock);
      ArchiveContext ctx(arena);
      Widths w;
      ReadWidths("{\"u16\":-1}", mock, w, ctx);
      CHECK(FindIssue(ctx, ArchiveErrorKind::RangeOverflow, "u16") != nullptr);
      CHECK(w.u16 == 0);
    }
  }

  // ---------------------------------------------------------------------------
  // 9. T-25 確保失敗の総当たり(書き)
  // ---------------------------------------------------------------------------

  void TestWriteAllocationFailure() {
    GLFD::Test::BeginCase("T-25: exhaustive allocation failure on the write side");

    ArchiveTest::Player player;
    player.name        = StringView("hero");
    player.weapon.name = StringView("sword");

    int total = 0;
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      JsonStringBuffer   buffer(arena);
      CHECK(WriteToJson(player, arena, 1, ArchiveFlags::None, buffer));
      total = mock.AllocateCalls();
      CHECK(total > 0);
    }

    bool allClean = true;
    for (int n = 0; n <= total + 2; ++n) {
      MockMemoryResource mock;
      mock.SetFailAfter(n);

      JsonArena        arena(&mock);
      JsonStringBuffer buffer(arena);
      CompactWriter    writer(buffer);
      ArchiveContext   ctx(arena, 1);
      bool             ok = false;
      {
        TestArchive ar(writer, ctx);
        ArchiveTest::Serialize(ar, player);
        ok = ar.Finish();
      }

      // 失敗したなら必ず ContainerFailure が記録され HasFatal が立つ
      if (!ok) {
        if (!ctx.HasFatal()) { allClean = false; }
      }
      else {
        if (ctx.HasIssues()) { allClean = false; }
      }
      CHECK_QUIET(ok || ctx.HasFatal());
      CHECK_QUIET(!mock.DoubleFree());
      CHECK_QUIET(!mock.UnknownFree());
      if (mock.DoubleFree() || mock.UnknownFree()) { allClean = false; }
    }
    CHECK(allClean);

    // 十分な確保があれば成功する
    {
      MockMemoryResource mock;
      mock.SetFailAfter(total);
      JsonArena        arena(&mock);
      JsonStringBuffer buffer(arena);
      CHECK(WriteToJson(player, arena, 1, ArchiveFlags::None, buffer));
    }
  }

  // ---------------------------------------------------------------------------
  // 10. T-25 確保失敗の総当たり(読み)
  // ---------------------------------------------------------------------------

  void TestReadAllocationFailure() {
    GLFD::Test::BeginCase("T-25: exhaustive allocation failure on the read side");

    const StringView json(
      "{\"$version\":1,\"stats\":{\"hp\":1,\"mp\":2},\"speed\":1.5,\"alive\":true,"
      "\"level\":3,\"name\":\"hero\",\"weapon\":{\"id\":1,\"damage\":2.0,\"name\":\"axe\"},"
      "\"legacy\":0}");

    // 読み側のアリーナだけに失敗を注入する。Document のパースは常に成功させ、
    // アーカイブ層の確保失敗だけを切り分ける
    MockMemoryResource docMock;
    Document           doc(&docMock);
    CHECK(doc.Parse(json, ParseFlags::None).IsOk());

    int total = 0;
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      ArchiveContext     ctx(arena, 0, ArchiveFlags::ReportUnknown);
      ArchiveTest::Player player;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, player);
      }
      CHECK(ctx.IssueCount() == 1u);    // "legacy" のみ
      total = mock.AllocateCalls();
      CHECK(total > 0);
    }

    bool allClean = true;
    for (int n = 0; n <= total + 2; ++n) {
      MockMemoryResource mock;
      mock.SetFailAfter(n);

      JsonArena           arena(&mock);
      ArchiveContext      ctx(arena, 0, ArchiveFlags::ReportUnknown);
      ArchiveTest::Player player;
      {
        ReadArchive ar(doc.Root(), ctx);
        ArchiveTest::Serialize(ar, player);
      }

      // クラッシュしないこと。確保に失敗しても診断機構が壊れないこと
      CHECK_QUIET(!mock.DoubleFree());
      CHECK_QUIET(!mock.UnknownFree());
      if (mock.DoubleFree() || mock.UnknownFree()) { allClean = false; }

      // 文字列の複製に失敗したら ContainerFailure が立つ。
      // 診断用の作業領域が取れなかっただけなら Fatal にしない
      if (ctx.IssuesTruncated() && !ctx.HasFatal()) { allClean = false; }
    }
    CHECK(allClean);
  }

  // ---------------------------------------------------------------------------
  // 11. 静的検証
  // ---------------------------------------------------------------------------

  void TestStaticProperties() {
    GLFD::Test::BeginCase("static properties of the archive surface");

    static_assert(ReadArchive::IsReading() && !ReadArchive::IsWriting(),
                  "ReadArchive must report itself as reading");
    static_assert(!TestArchive::IsReading() && TestArchive::IsWriting(),
                  "WriteArchive must report itself as writing");

    // 組み込みスカラの関門
    static_assert(GLFD::Json::Detail::IsArchiveScalar<std::int32_t>);
    static_assert(GLFD::Json::Detail::IsArchiveScalar<double>);
    static_assert(GLFD::Json::Detail::IsArchiveScalar<StringView>);
    static_assert(!GLFD::Json::Detail::IsArchiveScalar<ArchiveTest::Stats>);

    // ユーザー型は ADL の Serialize で見つかる
    static_assert(GLFD::Json::Detail::HasAdlSerialize<ReadArchive, ArchiveTest::Stats>);
    static_assert(GLFD::Json::Detail::Archivable<ReadArchive, ArchiveTest::Player>);
    static_assert(GLFD::Json::Detail::Archivable<TestArchive, ArchiveTest::Player>);

    // JsonSerializer 特殊化は ADL より優先される(逃げ道として機能する条件)
    static_assert(GLFD::Json::Detail::HasCustomSerializer<ReadArchive, ArchiveTest::IntList>);

    // SkipDefaults の比較可能性
    static_assert(GLFD::Json::Detail::ArchiveComparable<std::int32_t>);
    static_assert(!GLFD::Json::Detail::ArchiveComparable<ArchiveTest::Stats>);

    // 予約キー (R3-10)
    static_assert(GLFD::Json::IsReservedKey(StringView("$version")));
    static_assert(!GLFD::Json::IsReservedKey(StringView("version")));
    static_assert(!GLFD::Json::IsReservedKey(StringView("")));

    ++GLFD::Test::g_checkCount;   // 上の static_assert 群を 1 件として数える

    // Finish() の戻り値は無視できない
    CHECK(GLFD::Json::ToString(ArchiveErrorKind::MissingRequired) != nullptr);
    CHECK(GLFD::Json::ToString(ArchiveErrorKind::ContainerFailure) != nullptr);
  }

  // ---------------------------------------------------------------------------
  // 12. パスの深さと截断
  // ---------------------------------------------------------------------------

  void TestPathTruncation() {
    GLFD::Test::BeginCase("path stack truncates instead of failing");

    MockMemoryResource mock;
    JsonArena          arena(&mock);
    ArchiveContext     ctx(arena);

    GLFD::Json::Detail::PathStack& path = ctx.Path();

    for (std::uint32_t i = 0; i < GLFD::Json::Detail::PathStack::kMaxDepth; ++i) {
      path.PushKey(StringView("a"));
    }
    CHECK(!path.Truncated());
    path.PushKey(StringView("overflow"));
    CHECK(path.Truncated());

    const StringView materialized = path.Materialize(arena);
    CHECK(materialized.Size() > 0);
    // 末尾が "/..." になっていること
    CHECK(materialized.Size() >= 4);
    CHECK(std::memcmp(materialized.Data() + materialized.Size() - 4, "/...", 4) == 0);

    path.Pop();
    CHECK(!path.Truncated());
    for (std::uint32_t i = 0; i < GLFD::Json::Detail::PathStack::kMaxDepth; ++i) {
      path.Pop();
    }
    CHECK(path.Depth() == 0u);
    CHECK(path.Materialize(arena).Empty());

    // 添字の描画
    path.PushKey(StringView("items"));
    path.PushIndex(0);
    CHECK(ViewEquals(path.Materialize(arena), "items/0"));
    path.Pop();
    path.PushIndex(4294967295u);
    CHECK(ViewEquals(path.Materialize(arena), "items/4294967295"));
    path.Pop();
    path.PushIndex(3);
    path.PushKey(StringView("name"));
    CHECK(ViewEquals(path.Materialize(arena), "items/3/name"));
  }

  // ---------------------------------------------------------------------------
  // 13. T-28 Finish() の書き忘れ
  // ---------------------------------------------------------------------------

  bool ToWide(const char* utf8, wchar_t* out, int capacity) {
    const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              utf8, -1, out, capacity);
    return written > 0;
  }

  bool FileExistsUtf8Path(const char* utf8Path) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) { return false; }
    return ::GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
  }

  void DeleteFileUtf8Path(const char* utf8Path) {
    wchar_t wide[1024];
    if (ToWide(utf8Path, wide, 1024)) { ::DeleteFileW(wide); }
  }

  int CountTempFiles(const char* utf8Dir) {
    char pattern[1024];
    std::snprintf(pattern, sizeof(pattern), "%s\\*.tmp", utf8Dir);

    wchar_t wide[1024];
    if (!ToWide(pattern, wide, 1024)) { return -1; }

    WIN32_FIND_DATAW data{};
    const HANDLE     find = ::FindFirstFileW(wide, &data);
    if (find == INVALID_HANDLE_VALUE) { return 0; }
    int count = 0;
    do { ++count; } while (::FindNextFileW(find, &data) != 0);
    ::FindClose(find);
    return count;
  }

  const char* TempRoot() {
    static char root[1024];
    static bool initialized = false;
    if (!initialized) {
      wchar_t     wide[512];
      const DWORD length = ::GetTempPathW(512, wide);
      char        utf8[1024];
      const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                    utf8, sizeof(utf8), nullptr, nullptr);
      std::snprintf(root, sizeof(root), "%.*sglfd_json_archive",
                    (converted > 0 ? converted : 0), utf8);
      wchar_t rootWide[1024];
      if (ToWide(root, rootWide, 1024)) {
        (void)::CreateDirectoryW(rootWide, nullptr);
      }
      initialized = true;
    }
    return root;
  }

  /**
   * @brief T-28: `Finish()` も `Abort()` も呼ばずに破棄したときの挙動
   *
   * @details
   *  R3-21 の assert は「セーブしたつもりで何も書かれていない」を捕まえる安全網である。
   *  Debug では書き忘れの経路そのものを走らせられない(assert が abort する)ため、
   *  役割を分けている:
   *   - Debug / Release 共通: `Abort()` を呼ぶ経路を通し、**デストラクタの assert が
   *     実際にコンパイル・評価されている**ことを担保する(安全網が死んでいない)
   *   - Release のみ: 書き忘れそのものを実行し、クラッシュしないこと・
   *     出力が不完全なままであること・目標ファイルが作られないことを確認する
   */
  void TestForgottenFinish() {
    GLFD::Test::BeginCase("T-28: destroying WriteArchive without Finish() / Abort()");

    // (a) 明示的な Abort。安全網が生きていることの担保も兼ねる
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      JsonStringBuffer   buffer(arena);
      CompactWriter      writer(buffer);
      ArchiveContext     ctx(arena, 1);
      {
        TestArchive        ar(writer, ctx);
        ArchiveTest::Stats stats;
        stats.hp = 1;
        stats.mp = 2;
        ArchiveTest::Serialize(ar, stats);
        ar.Abort();
      }
      // ルートは閉じられていない
      CHECK(!writer.IsComplete());
      CHECK(writer.Depth() == 1u);
      // 途中まで書かれた内容はそのまま残る(捨てるのはストリームの仕事)
      CHECK(ViewEquals(buffer.View(), "{\"$version\":1,\"hp\":1,\"mp\":2"));
      CHECK(!ctx.HasFatal());   // 中止は失敗ではない
    }

    // (b) Finish() の後に Abort() を呼んでも二重終了にならない
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      JsonStringBuffer   buffer(arena);
      CompactWriter      writer(buffer);
      ArchiveContext     ctx(arena, 1);
      {
        TestArchive        ar(writer, ctx);
        ArchiveTest::Stats stats;
        ArchiveTest::Serialize(ar, stats);
        CHECK(ar.Finish());
        CHECK(!ar.Finish());    // 二度目は false
      }
      CHECK(writer.IsComplete());
    }

#ifdef NDEBUG
    // (c) 書き忘れそのもの。Debug では assert が発火するため Release でのみ実行する
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      JsonStringBuffer   buffer(arena);
      CompactWriter      writer(buffer);
      ArchiveContext     ctx(arena, 1);
      {
        TestArchive        ar(writer, ctx);
        ArchiveTest::Stats stats;
        stats.hp = 1;
        stats.mp = 2;
        ArchiveTest::Serialize(ar, stats);
        // Finish() も Abort() も呼ばない
      }
      CHECK(!writer.IsComplete());                        // 未クローズ
      CHECK(buffer.Size() > 0);                           // 途中まで残る
      CHECK(buffer.View()[buffer.Size() - 1] != '}');     // 出力は不完全
      CHECK(ViewEquals(buffer.View(), "{\"$version\":1,\"hp\":1,\"mp\":2"));
    }

    // (d) JsonFileStream と組にした場合、目標ファイルは作られない (R1-20 との結合)
    {
      char path[1024];
      std::snprintf(path, sizeof(path), "%s\\forgotten_finish.json", TempRoot());
      DeleteFileUtf8Path(path);
      CHECK(!FileExistsUtf8Path(path));

      MockMemoryResource mock;
      JsonArena          arena(&mock);
      {
        using FileWriter = GLFD::Json::JsonWriter<GLFD::Json::JsonFileStream>;

        GLFD::Json::JsonFileStream stream(path, arena);
        FileWriter                 writer(stream);
        ArchiveContext             ctx(arena, 1);
        {
          WriteArchive<FileWriter> ar(writer, ctx);
          ArchiveTest::Stats       stats;
          ArchiveTest::Serialize(ar, stats);
          // WriteArchive も JsonFileStream も終わらせない
        }
      }
      // 一時ファイル方式なので、置き換えが起きず目標ファイルは存在しない
      CHECK(!FileExistsUtf8Path(path));
      CHECK(CountTempFiles(TempRoot()) == 0);   // 一時ファイルも片付いている
    }
#endif
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonArchive (1-5b)");

  TestWriteShape();
  TestRoundTrip();
  TestVersionCompatibility();
  TestVersionBranching();
  TestArrayPrimitives();
  TestDiagnostics();
  TestSkipDefaults();
  TestRangeChecks();
  TestWriteAllocationFailure();
  TestReadAllocationFailure();
  TestStaticProperties();
  TestPathTruncation();
  TestForgottenFinish();

  return GLFD::Test::Summarize();
}
