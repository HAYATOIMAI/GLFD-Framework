/**
 * @file  JsonShippedConfigTests.cpp
 * @brief A-1: 出荷 config の健全性 — T-60
 *
 * @details
 *  **ここが赤くなったら直すのは config であって、テストでも実装でもない。**
 *  対象はリポジトリの成果物そのもの (`Resource/GameConfig.jsonc` と
 *  `Resource/GameConfig.local.json.sample`)。`JsonRealData` (T-38) が
 *  「実データが読めること」を見るのに対し、こちらは
 *  「**出荷されているファイルが健全であること**」を見る。落ちたときに
 *  何を直すかが違うのでスイートを分けてある。
 *
 *  ## 何を防ぐのか
 *  `GameConfig` の構造体を変えて `.jsonc` を直し忘れると、**ゲームは起動する**。
 *  旧フィールドは `UnknownField`、新フィールドは既定値のまま、型を変えれば
 *  `TypeMismatch` で既定値のまま。2-4 の診断で気づけはするが、気づくのは
 *  ゲームを起動して F5 を押したときになる。ここで捕まえる。
 *
 *  ## **値を固定しない**
 *  `maxSpeed == 2.0f` のような検査は書かない。config を触るたびに落ちるテストは
 *  「直すもの」ではなく「合わせるもの」になり、無意味になる。
 *  見るのは**構造の健全性**であって値ではない。
 *  「読み取りが実際に起きたか」は、**ファイルの DOM と構造体を突き合わせる**形で
 *  直接確かめる(何が書いてあっても通り、読み取りが壊れたときだけ落ちる)。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "Core/GameConfig.h"
#include "Core/GameConfigLoad.h"
#include "Core/Json/Json.h"

#include "MockMemoryResource.h"
#include "RepositoryPath.h"
#include "TestHarness.h"

using GLFD::ConfigLayer;
using GLFD::GameConfig;
using GLFD::ReloadGameConfig;
using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::Document;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::LoadFromJson;
using GLFD::Json::ParseFlags;
using GLFD::Json::SaveToJson;
using GLFD::Json::Value;
using GLFD::Test::MockMemoryResource;

namespace {

  // -------------------------------------------------------------------------
  // 対象のパス。解決規則は RepositoryPath.h を参照(CWD に依存しない)
  // -------------------------------------------------------------------------

  const char* ShippedConfigPath() {
    static char path[1024];
    static bool initialized = false;
    if (!initialized) {
      GLFD::Test::RepositoryFile(path, sizeof(path), "Resource\\GameConfig.jsonc");
      initialized = true;
    }
    return path;
  }

  const char* SamplePath() {
    static char path[1024];
    static bool initialized = false;
    if (!initialized) {
      GLFD::Test::RepositoryFile(path, sizeof(path), "Resource\\GameConfig.local.json.sample");
      initialized = true;
    }
    return path;
  }

  /**
   * @brief 上書きレイヤとして**絶対に存在しない**パス
   *
   * @note 開発者の実物 `Resource/GameConfig.local.json` を渡してはならない。
   *       あれは Git 管理外の個人ファイルで、**手元にあるかどうかで結果が変わる**。
   *       出荷物の健全性を見るテストが、個人の設定に左右されてはいけない
   */
  const char* AbsentLocalPath() {
    static char path[1024];
    static bool initialized = false;
    if (!initialized) {
      GLFD::Test::RepositoryFile(path, sizeof(path),
                                 "Resource\\__no_local_overlay_for_tests__.json");
      initialized = true;
    }
    return path;
  }

  // -------------------------------------------------------------------------
  // 落ちたときに「何を直せばよいか」が分かる受け皿
  // -------------------------------------------------------------------------

  /// `ReloadGameConfig` の `onIssues`。**Issue をそのまま出力する** (2-4 の整形を流用)
  class ShippedObserver {
  public:
    static constexpr int kBase  = 0;
    static constexpr int kLocal = 1;

    void operator()(ConfigLayer layer, const char* path,
                    const Document& doc, const ArchiveContext& ctx) {
      const int index = (layer == ConfigLayer::Base) ? kBase : kLocal;
      ++m_calls[index];
      m_issues[index]      = ctx.IssueCount();
      m_fatal[index]       = ctx.HasFatal();
      m_version[index]     = ctx.Version();
      m_parseFailed[index] = doc.HasError();

      if (ctx.IssueCount() == 0u && !doc.HasError()) { return; }

      // ここが本題。`CHECK(!ctx.HasIssues())` が落ちるだけでは何を直すか分からない
      std::printf("    --- %s ---\n", path);
      if (doc.HasError()) {
        char         line[GLFD::Json::kDiagnosticLineCapacity];
        const size_t size = GLFD::Json::FormatParseError(
          line, sizeof(line), StringView(path), doc.Error().code, doc.Error().offset,
          doc.Source());
        std::printf("    %.*s\n", static_cast<int>(size), line);
      }
      GLFD::Json::ForEachIssueLine(ctx, StringView(path), [](StringView line, bool isFatal) {
        std::printf("    %s%.*s\n", isFatal ? "[fatal]" : "       ",
                    static_cast<int>(line.Size()), line.Data());
      });
      std::fflush(stdout);
    }

    [[nodiscard]] int           Calls(int i)       const { return m_calls[i]; }
    [[nodiscard]] std::uint32_t Issues(int i)      const { return m_issues[i]; }
    [[nodiscard]] bool          Fatal(int i)       const { return m_fatal[i]; }
    [[nodiscard]] std::uint32_t Version(int i)     const { return m_version[i]; }
    [[nodiscard]] bool          ParseFailed(int i) const { return m_parseFailed[i]; }

  private:
    int           m_calls[2]       = { 0, 0 };
    std::uint32_t m_issues[2]      = { 0, 0 };
    bool          m_fatal[2]       = { false, false };
    std::uint32_t m_version[2]     = { 0, 0 };
    bool          m_parseFailed[2] = { false, false };
  };

  /// 出荷 config を**本番の 2 段ロードで**読む(手順を写さない)
  struct ShippedLoad {
    MockMemoryResource          mock;
    std::unique_ptr<Document>   baseDoc;
    std::unique_ptr<Document>   localDoc;
    std::unique_ptr<GameConfig> config;
    ShippedObserver             observer;

    [[nodiscard]] bool Run(const char* localPath) {
      return ReloadGameConfig(baseDoc, localDoc, config,
                              ShippedConfigPath(), localPath, &mock, observer);
    }

    ~ShippedLoad() {
      config.reset();     // Document より先に捨てる (R0-5)
      localDoc.reset();
      baseDoc.reset();
    }
  };

  [[nodiscard]] bool ContainsNonAscii(StringView text) {
    for (size_t i = 0; i < text.Size(); ++i) {
      if (static_cast<unsigned char>(text[i]) >= 0x80u) { return true; }
    }
    return false;
  }

  [[nodiscard]] bool Contains(StringView haystack, const char* needle) {
    const size_t length = std::strlen(needle);
    if (length == 0 || haystack.Size() < length) { return false; }
    for (size_t i = 0; i + length <= haystack.Size(); ++i) {
      if (std::memcmp(haystack.Data() + i, needle, length) == 0) { return true; }
    }
    return false;
  }

  /// DOM に書いてある float と、構造体に入った値が一致すること(**値は固定しない**)
  void CheckFloatMatchesFile(const Document& doc, const char* path, float loaded) {
    const Value* const fromFile = doc.Query(StringView(path));
    CHECK(fromFile != nullptr);
    if (fromFile == nullptr) { std::printf("    (no such path: %s)\n", path); return; }

    float expected = 0.0f;
    CHECK(fromFile->TryGetFloat(expected));
    CHECK(loaded == expected);
    if (loaded != expected) {
      std::printf("    (%s: file=%f struct=%f)\n", path,
                  static_cast<double>(expected), static_cast<double>(loaded));
    }
  }

  // -------------------------------------------------------------------------
  // T-60 (1) 出荷 .jsonc の健全性
  // -------------------------------------------------------------------------

  void TestShippedConfigIsHealthy() {
    GLFD::Test::BeginCase("T-60: the shipped GameConfig.jsonc loads with zero issues");

    ShippedLoad load;
    const bool  ok = load.Run(AbsentLocalPath());

    if (!ok) {
      // パス解決の誤りと config の不在を切り分けられるようにする
      std::printf("    (resolved path: %s)\n", ShippedConfigPath());
    }
    CHECK(ok);
    CHECK(!load.observer.ParseFailed(ShippedObserver::kBase));

    // **Issue が 0 件**。1 件でもあれば上の observer が kind/path/detail を出している
    CHECK(load.observer.Issues(ShippedObserver::kBase) == 0u);
    CHECK(!load.observer.Fatal(ShippedObserver::kBase));

    // `$version` がコード側の期待値と一致すること
    CHECK(load.observer.Version(ShippedObserver::kBase) == GLFD::kGameConfigVersion);

    // 上書きは渡していないので Local の診断は呼ばれない(不在は正常)
    CHECK(load.observer.Calls(ShippedObserver::kLocal) == 0);

    if (!ok || load.config == nullptr) { return; }
    const GameConfig& config = *load.config;

    // --- JSONC が効いていること。ファイルに実際にコメントがある ---------------
    // `Document::Source()` は ParseFile 経路で読んだ本文そのもの (2-4)
    const StringView source = load.baseDoc->Source();
    CHECK(!source.Empty());
    CHECK(Contains(source, "//"));   // コメントを含むのにパースが通っている

    // --- 日本語が往復すること。**文字列そのものは固定しない** ----------------
    // タイトルは変えたくなる典型なので、空でないことと非 ASCII を含むことだけ見る
    CHECK(!config.window.title.Empty());
    CHECK(ContainsNonAscii(config.window.title));

    // --- ネストと配列が読めること。**件数は固定しない** ----------------------
    CHECK(config.boidProfiles.GetSize() > 0);   // 0 件なら構造として壊れている
    bool everyProfileHasName = true;
    for (size_t i = 0; i < config.boidProfiles.GetSize(); ++i) {
      if (config.boidProfiles[i].name.Empty()) { everyProfileHasName = false; }
    }
    CHECK(everyProfileHasName);

    // --- 読み取りが実際に起きたことを、DOM と突き合わせて直接確かめる ---------
    // 代理指標を使わない。何が書いてあっても通り、読み取りが壊れたときだけ落ちる
    {
      // T[N](固定長配列)
      CheckFloatMatchesFile(*load.baseDoc, "world/halfExtent/0", config.world.halfExtent[0]);
      CheckFloatMatchesFile(*load.baseDoc, "world/halfExtent/1", config.world.halfExtent[1]);

      // T-64: 出荷ファイルが**移行前の名前を残していない**こと。
      // Issue 0 件でも捕まる(旧名は UnknownField になる)が、`$version` を
      // 上げ忘れたまま名前だけ直した場合と区別が付くよう、名前を名指しで見る
      CHECK(load.baseDoc->Query(StringView("world/bounds")) == nullptr);
      CHECK(load.baseDoc->Query(StringView("world/halfExtent")) != nullptr);

      // T-ECS-10 (1-2): **出荷 config が上限を越えていないこと。**
      // 越えると `RangeOverflow` が出て既定値へ戻るので、上の
      // 「Issue 0 件」でも間接的には捕まる。ただしそれでは
      // **「なぜ落ちたか」が上限の話だと分からない**ので名指しで見る。
      // 値そのものは固定しない(チューニングで動いてよい)
      CHECK(config.simulation.entityCount <= GLFD::kMaxConfigurableEntityCount);
      CHECK(config.simulation.entityCount > 0);

      // ネストしたスカラ
      const Value* const width = load.baseDoc->Query(StringView("window/width"));
      CHECK(width != nullptr);
      if (width != nullptr) {
        std::int32_t expected = 0;
        CHECK(width->TryGetInt(expected));
        CHECK(config.window.width == expected);
      }

      // 配列要素の中の文字列
      const Value* const firstName = load.baseDoc->Query(StringView("boidProfiles/0/name"));
      CHECK(firstName != nullptr);
      if (firstName != nullptr && config.boidProfiles.GetSize() > 0) {
        StringView expected;
        CHECK(firstName->TryGetString(expected));
        CHECK(config.boidProfiles[0].name == expected);
      }
    }
  }

  // -------------------------------------------------------------------------
  // T-60 (2) .sample の健全性
  // -------------------------------------------------------------------------

  void TestSampleOverlayIsHealthy() {
    GLFD::Test::BeginCase("T-60: GameConfig.local.json.sample applies as an overlay with zero issues");

    ShippedLoad load;
    const bool  ok = load.Run(SamplePath());

    if (!ok) { std::printf("    (resolved path: %s)\n", SamplePath()); }
    CHECK(ok);

    // **利用者が最初に見る雛形**が古いと「サンプルどおりに書いたのに UnknownField」
    // という第一印象になる。ここで捕まえる
    CHECK(load.observer.Calls(ShippedObserver::kLocal) == 1);
    CHECK(!load.observer.ParseFailed(ShippedObserver::kLocal));
    CHECK(load.observer.Issues(ShippedObserver::kLocal) == 0u);
    CHECK(!load.observer.Fatal(ShippedObserver::kLocal));

    // 基底側も汚れていないこと
    CHECK(load.observer.Issues(ShippedObserver::kBase) == 0u);
  }

  // -------------------------------------------------------------------------
  // T-60 (3) 往復
  // -------------------------------------------------------------------------

  void TestShippedConfigRoundTrips() {
    GLFD::Test::BeginCase("T-60: the shipped config survives save -> load with zero issues");

    ShippedLoad load;
    CHECK(load.Run(AbsentLocalPath()));
    if (load.config == nullptr) { return; }

    // 書き出す(コメントは保持されない = クリーン再生成)
    JsonArena        arena(&load.mock);
    JsonStringBuffer buffer(arena);
    CHECK(SaveToJson(*load.config, buffer, GLFD::kGameConfigVersion, /*pretty=*/true));

    // 読み戻す。**`Serialize` の読み書きが非対称なら Issue が出る**
    // (書いているのに読んでいない = UnknownField、逆 = 既定値のまま)
    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, GLFD::Json::ArchiveFlags::ReportUnknown);
    GameConfig         reloaded(&mock);

    CHECK(LoadFromJson(reloaded, buffer.View(), doc, ctx, ParseFlags::None));
    if (ctx.IssueCount() != 0u) {
      GLFD::Json::ForEachIssueLine(ctx, StringView("<round trip>"),
                                   [](StringView line, bool isFatal) {
        std::printf("    %s%.*s\n", isFatal ? "[fatal]" : "       ",
                    static_cast<int>(line.Size()), line.Data());
      });
    }
    CHECK(ctx.IssueCount() == 0u);
    CHECK(!ctx.HasFatal());
    CHECK(ctx.Version() == GLFD::kGameConfigVersion);

    // 構造が保たれていること(**値も件数も固定しない**)
    CHECK(reloaded.boidProfiles.GetSize() == load.config->boidProfiles.GetSize());
    CHECK(!reloaded.window.title.Empty());
    CHECK(ContainsNonAscii(reloaded.window.title));
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonShippedConfig (A-1)");

  TestShippedConfigIsHealthy();
  TestSampleOverlayIsHealthy();
  TestShippedConfigRoundTrips();

  return GLFD::Test::Summarize();
}
