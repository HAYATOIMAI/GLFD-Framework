/**
 * @file  JsonRealDataTests.cpp
 * @brief フェーズ 1-7: 実データ (`Resource/GameConfig.jsonc`) の疎通 — T-38
 *
 * @note **役割分担 (A-1)**
 *       - **T-38(ここ)= 実データが読めることの検証。** 落ちたら
 *         テストか実装を直す
 *       - **T-60 (`JsonShippedConfigTests`) = 出荷 config が健全であることの検証。**
 *         落ちたら config を直す
 *
 *       **ここでは値を固定しない。config を変えても落ちないこと。**
 *       健全性(Issue 0 件 / `$version` の一致 / `.sample` の有効性)は T-60 が見る。
 *       以前は `maxSpeed == 2.0f` のような検査が並んでおり、チューニングのたびに
 *       落ちる状態だった。分けた理由が残っていないとまた混ざる
 *
 * @details
 *  ゲームが実際に読む設定ファイルそのものを対象にする。
 *   - 値が構造体に入ること
 *   - **コメントと末尾カンマを含む**ファイルが読めること (JSONC)
 *   - **日本語のコメント・値**が正しく往復すること (R2-12 / R1-13)
 *   - 壊れたファイルで**既定値のまま継続し診断が出る**こと
 *   - ファイルが無い状態でも**既定値のまま継続する**こと
 *   - 読み直しに失敗しても**直前の設定がそのまま残る**こと (T-41)
 *
 *  ファイルの場所は実行ファイルの位置から辿る。テストの起動ディレクトリに
 *  依存させないため(`Tests/build/*.exe` から 2 つ上がリポジトリのルート)。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 *        日本語はバイト配列で組み立てる。
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

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

using GLFD::GameConfig;
using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::Document;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::LoadFromJsonFile;
using GLFD::ReloadGameConfig;
using GLFD::Json::SaveToJson;
using GLFD::Test::MockMemoryResource;

namespace {

  [[nodiscard]] bool ViewEquals(StringView view, const void* bytes, size_t size) {
    return view.Size() == size && (size == 0 || std::memcmp(view.Data(), bytes, size) == 0);
  }

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

  /// 出荷されている設定ファイル。パスの解決規則は `RepositoryPath.h` を参照
  const char* ShippedConfigPath() {
    static char path[1024];
    static bool initialized = false;
    if (!initialized) {
      GLFD::Test::RepositoryFile(path, sizeof(path), "Resource\\GameConfig.jsonc");
      initialized = true;
    }
    return path;
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
    const DWORD length = ::GetTempPathW(512, wide);
    char        utf8[1024];
    const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                  utf8, sizeof(utf8), nullptr, nullptr);
    std::snprintf(path, sizeof(path), "%.*s%s", (converted > 0 ? converted : 0), utf8, leaf);
    return path;
  }

  /**
   * @brief "GLFD ボイド" の UTF-8 バイト列
   * @note  **T-41 が自分で書いた一時 config の検証に使う。** 出荷ファイルの値では
   *        ないので固定してよい(出荷ファイルのタイトルは固定しない。上の @note)。
   *        テストに非 ASCII リテラルを書かない (C5297) ためバイト配列で持つ
   */
  const unsigned char kExpectedTitle[] = {
    'G', 'L', 'F', 'D', ' ',
    0xE3, 0x83, 0x9C,   // ボ
    0xE3, 0x82, 0xA4,   // イ
    0xE3, 0x83, 0x89,   // ド
  };

  [[nodiscard]] bool ContainsNonAscii(StringView text) {
    for (size_t i = 0; i < text.Size(); ++i) {
      if (static_cast<unsigned char>(text[i]) >= 0x80u) { return true; }
    }
    return false;
  }

  [[nodiscard]] bool ContainsBytes(StringView haystack, const char* needle) {
    const size_t length = std::strlen(needle);
    if (length == 0 || haystack.Size() < length) { return false; }
    for (size_t i = 0; i + length <= haystack.Size(); ++i) {
      if (std::memcmp(haystack.Data() + i, needle, length) == 0) { return true; }
    }
    return false;
  }

  /// DOM に書いてある値と構造体の値が一致すること。**値そのものは固定しない**
  void CheckFloatMatchesFile(const Document& doc, const char* path, float loaded) {
    const GLFD::Json::Value* const fromFile = doc.Query(StringView(path));
    CHECK(fromFile != nullptr);
    if (fromFile == nullptr) { std::printf("    (no such path: %s)\n", path); return; }

    float expected = 0.0f;
    CHECK(fromFile->TryGetFloat(expected));
    CHECK(loaded == expected);
  }

  // -------------------------------------------------------------------------
  // 1. 実ファイルを読む
  // -------------------------------------------------------------------------

  void TestShippedConfigLoads() {
    GLFD::Test::BeginCase("T-38: the shipped GameConfig.jsonc loads with comments and trailing commas");

    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);
    GameConfig         config(&mock);

    const bool ok = LoadFromJsonFile(config, ShippedConfigPath(), doc, ctx);
    if (!ok) {
      std::printf("    (path = %s, error = %d)\n",
                  ShippedConfigPath(), static_cast<int>(doc.Error().code));
    }
    CHECK(ok);
    CHECK(!ctx.HasFatal());

    // 未知フィールドが無いこと = 構造体と設定ファイルが一致していること。
    // フィールド名を打ち間違えるとここで落ちる
    CHECK(!ctx.HasIssues());

    // $version が読めていること (R3-7)
    CHECK(ctx.Version() == GLFD::kGameConfigVersion);

    // --- ここから下は**値を固定しない**。config を変えても落ちない形にする ---

    // JSONC が効いていること。ファイルに実際にコメントがあり、かつ読めている
    // (`Document::Source()` は ParseFile 経路で読んだ本文そのもの。2-4)
    CHECK(!doc.Source().Empty());
    CHECK(ContainsBytes(doc.Source(), "//"));

    // 日本語が読めていること。**文字列そのものは固定しない**
    // (タイトルは変えたくなる典型。ここで固定すると config を触れなくなる)
    CHECK(!config.window.title.Empty());
    CHECK(ContainsNonAscii(config.window.title));

    // ネストと DynamicArray<UserType> が読めていること。**件数は固定しない**
    CHECK(config.boidProfiles.GetSize() > 0);
    bool everyProfileHasName = true;
    for (size_t i = 0; i < config.boidProfiles.GetSize(); ++i) {
      if (config.boidProfiles[i].name.Empty()) { everyProfileHasName = false; }
    }
    CHECK(everyProfileHasName);

    // T[N] とネストしたスカラが読めていること。**DOM と突き合わせて直接確かめる**。
    // 「両方が既定値なら等しいはず」のような代理指標にしない
    // (画面が正方形なら halfExtent[0] == halfExtent[1] は自然に起こり得る)
    CheckFloatMatchesFile(doc, "world/halfExtent/0", config.world.halfExtent[0]);
    CheckFloatMatchesFile(doc, "world/halfExtent/1", config.world.halfExtent[1]);
    CheckFloatMatchesFile(doc, "world/spawnRange/0", config.world.spawnRange[0]);
    CheckFloatMatchesFile(doc, "simulation/maxSpeed", config.simulation.maxSpeed);
  }

  // -------------------------------------------------------------------------
  // 2. 日本語を含む往復
  // -------------------------------------------------------------------------

  void TestJapaneseRoundTrip() {
    GLFD::Test::BeginCase("T-38: Japanese values survive load -> save -> load");

    MockMemoryResource mock;

    Document       doc(&mock);
    ArchiveContext ctx(doc.Arena());
    GameConfig     config(&mock);
    CHECK(LoadFromJsonFile(config, ShippedConfigPath(), doc, ctx));

    // 書き出す(コメントは保持されない = クリーン再生成。§0 の確定事項)
    JsonArena        arena(&mock);
    JsonStringBuffer buffer(arena);
    CHECK(SaveToJson(config, buffer, GLFD::kGameConfigVersion, /*pretty=*/true));

    // 読み戻す
    Document       doc2(&mock);
    ArchiveContext ctx2(doc2.Arena());
    GameConfig     reloaded(&mock);
    CHECK(GLFD::Json::LoadFromJson(reloaded, buffer.View(), doc2, ctx2,
                                   GLFD::Json::ParseFlags::None));
    CHECK(!ctx2.HasIssues());

    // 往復しても日本語が壊れないこと。**元の文字列とは比較するが、
    // 特定の文字列には固定しない**(config を変えても落ちない)
    CHECK(!reloaded.window.title.Empty());
    CHECK(ContainsNonAscii(reloaded.window.title));
    CHECK(reloaded.window.title == config.window.title);
    CHECK(reloaded.boidProfiles.GetSize() == config.boidProfiles.GetSize());
    if (reloaded.boidProfiles.GetSize() == config.boidProfiles.GetSize()) {
      bool namesMatch = true;
      for (size_t i = 0; i < config.boidProfiles.GetSize(); ++i) {
        if (reloaded.boidProfiles[i].name != config.boidProfiles[i].name) { namesMatch = false; }
        if (reloaded.boidProfiles[i].viewRadius != config.boidProfiles[i].viewRadius) {
          namesMatch = false;
        }
      }
      CHECK(namesMatch);   // 日本語のプロファイル名がバイト単位で一致する
    }
  }

  // -------------------------------------------------------------------------
  // 3. 壊れたファイル / 無いファイル
  // -------------------------------------------------------------------------

  void TestBrokenAndMissingFiles() {
    GLFD::Test::BeginCase("T-38: broken or missing config keeps the built-in defaults");

    MockMemoryResource mock;

    // (a) 構文が壊れている
    {
      const char* const path = TempPath("glfd_broken_config.jsonc");
      static const char kBroken[] = "{ \"window\": { \"width\": 1280,";
      CHECK(WriteBytesUtf8Path(path, kBroken, sizeof(kBroken) - 1));

      Document       doc(&mock);
      ArchiveContext ctx(doc.Arena());
      GameConfig     config(&mock);
      CHECK(!LoadFromJsonFile(config, path, doc, ctx));
      CHECK(doc.HasError());
      // 既定値のまま = ゲームは起動できる
      CHECK(config.window.width == 1024);
      CHECK(config.simulation.entityCount == 20000);
      DeleteFileUtf8(path);
    }

    // (b) 構文は正しいが値の型が違う。**部分的に読めるので診断が出る**
    {
      const char* const path = TempPath("glfd_typo_config.jsonc");
      static const char kTypo[] =
        "{ \"$version\": 1,"
        "  \"window\": { \"width\": \"wide\", \"height\": 900 },"
        "  \"simulation\": { \"entityCont\": 5 } }";   // フィールド名の打ち間違い
      CHECK(WriteBytesUtf8Path(path, kTypo, sizeof(kTypo) - 1));

      Document       doc(&mock);
      ArchiveContext ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);
      GameConfig     config(&mock);

      // 型違いも未知フィールドも Fatal ではないので、ロード自体は成功する
      CHECK(LoadFromJsonFile(config, path, doc, ctx));
      CHECK(!ctx.HasFatal());

      CHECK(config.window.width == 1024);        // 読めなかったので既定値
      CHECK(config.window.height == 900);        // 読めた
      CHECK(config.simulation.entityCount == 20000);

      bool sawTypeMismatch = false;
      bool sawUnknown      = false;
      for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
        const GLFD::Json::ArchiveIssue& issue = ctx.Issues()[i];
        if (issue.kind == ArchiveErrorKind::TypeMismatch
            && issue.path == StringView("window/width")) {
          sawTypeMismatch = true;
        }
        if (issue.kind == ArchiveErrorKind::UnknownField
            && issue.path == StringView("simulation/entityCont")) {
          sawUnknown = true;
        }
      }
      // **打ち間違えたフィールド名がパス付きで報告される**。これが R3-9 の目的
      CHECK(sawTypeMismatch);
      CHECK(sawUnknown);
      DeleteFileUtf8(path);
    }

    // (c) ファイルが存在しない
    {
      const char* const path = TempPath("glfd_no_such_config.jsonc");
      DeleteFileUtf8(path);

      Document       doc(&mock);
      ArchiveContext ctx(doc.Arena());
      GameConfig     config(&mock);
      CHECK(!LoadFromJsonFile(config, path, doc, ctx));
      CHECK(doc.Error().code == GLFD::Json::ErrorCode::FileNotFound);
      CHECK(config.window.width == 1024);
      CHECK(config.boidProfiles.GetSize() == 0);
    }
  }

  // -------------------------------------------------------------------------
  // 4. 旧バージョンのファイル(フィールドの増減)
  // -------------------------------------------------------------------------

  void TestOlderConfigStillLoads() {
    GLFD::Test::BeginCase("T-38: an older, smaller config still loads (R3-4 / R3-5)");

    MockMemoryResource mock;

    const char* const path = TempPath("glfd_old_config.jsonc");
    // boidProfiles も interaction も無い「古い」設定 + 既に消えたフィールド。
    // **$version 1 のままにしてある** (A-2)。実際に古いファイルなので、
    // ここを 2 に上げると「古い設定が読める」という検査の意味が薄れる
    static const char kOld[] =
      "// old file\n"
      "{ \"$version\": 1,\n"
      "  \"window\": { \"width\": 640, \"height\": 480 },\n"
      "  \"particleCount\": 100,\n"   // 昔あって今は無いフィールド
      "}\n";
    CHECK(WriteBytesUtf8Path(path, kOld, sizeof(kOld) - 1));

    Document       doc(&mock);
    ArchiveContext ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);
    GameConfig     config(&mock);

    CHECK(LoadFromJsonFile(config, path, doc, ctx));
    CHECK(!ctx.HasFatal());

    CHECK(config.window.width == 640);                 // 書いてある値
    CHECK(config.simulation.entityCount == 20000);     // 欠損 -> 既定値 (R3-4)
    CHECK(config.interaction.explosionForce == 50.0f); // 欠損 -> 既定値
    CHECK(config.boidProfiles.GetSize() == 0);         // 欠損 -> 触られない

    // 消えたフィールドは未知フィールドとして無視される (R3-5)
    bool sawUnknown = false;
    for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
      if (ctx.Issues()[i].kind == ArchiveErrorKind::UnknownField
          && ctx.Issues()[i].path == StringView("particleCount")) {
        sawUnknown = true;
      }
    }
    CHECK(sawUnknown);

    DeleteFileUtf8(path);
  }

  // -------------------------------------------------------------------------
  // 5. 読み直し(成功したときだけ差し替える)
  // -------------------------------------------------------------------------

  /**
   * @brief T-41: `ReloadGameConfig` は失敗しても直前の設定を壊さない
   *
   * @details
   *  `BoidDemoScene::ReloadConfig`(入室時と F5 で走る)が呼ぶ関数そのものを
   *  叩く。シーンを丸ごとリンクできないので**手順を写す**という手もあるが、
   *  写した実装をテストしても本番の経路は一度も実行されない。
   *
   *  本命は 2 の「壊してから読み直す」。文字列は `Document` のアリーナ上にあり
   *  (R0-5)、同じ `Document` へ読み直すと**失敗しても**前回の文字列が死ぬ。
   *  差し替えを成功時に限る設計はそのためのもので、ここで固定しておく。
   */
  void TestReloadKeepsPreviousOnFailure() {
    GLFD::Test::BeginCase("T-41: a failed reload keeps the previous config intact");

    MockMemoryResource mock;

    // TempPath は static バッファを返す。2 本のパスを同時に使うので、
    // **どちらも自分の配列へ写す**(写さないと後から取った方に上書きされ、
    // 基底と上書きが同じファイルを指す)
    char path[1024];
    std::snprintf(path, sizeof(path), "%s", TempPath("glfd_reload_config.jsonc"));

    // 日本語のタイトルを持たせる。**文字列が生き残ること**が確認したい中身なので、
    // スカラだけでは足りない(非 ASCII のリテラルは書かない: C5297)
    char      json[512];
    const int written = std::snprintf(
        json, sizeof(json),
        "{ \"$version\": 2,"
        "  \"window\": { \"width\": 800, \"title\": \"%.*s\" },"
        "  \"simulation\": { \"entityCount\": 5000, \"maxSpeed\": 3.5 },"
        "  \"boidProfiles\": [ { \"viewRadius\": 7.0 } ] }",
        static_cast<int>(sizeof(kExpectedTitle)),
        reinterpret_cast<const char*>(kExpectedTitle));
    CHECK(written > 0 && written < static_cast<int>(sizeof(json)));

    std::unique_ptr<Document>   doc;
    std::unique_ptr<Document>   localDoc;   // 2-3: 上書きレイヤ(このテストでは使わない)
    std::unique_ptr<GameConfig> config;

    // 実在しないパスを渡す。**不在は正常**なので何も起こらないことも併せて確かめる。
    // TempPath は static バッファを返すため、後続の呼び出しで内容が変わる。
    // 自分の配列へ写しておかないと、下の `path` と同じ文字列を指してしまう
    char noLocal[1024];
    std::snprintf(noLocal, sizeof(noLocal), "%s", TempPath("glfd_no_such_local.json"));
    DeleteFileUtf8(noLocal);

    std::uint32_t issueCount = 0;
    std::uint32_t version    = 0;
    // onIssues はファイルごとに 1 回、Document も添えて呼ばれる
    // (2-4: 失敗位置の算出 / 2-3: どちらのレイヤか)
    std::uint32_t localCalls = 0;
    const auto    observe    = [&issueCount, &version, &localCalls](
                                 GLFD::ConfigLayer layer, const char*,
                                 const Document&, const ArchiveContext& ctx) {
      if (layer == GLFD::ConfigLayer::Local) { ++localCalls; return; }
      issueCount = ctx.IssueCount();
      version    = ctx.Version();
    };

    // --- 1. 正常な設定を読む -------------------------------------------------
    CHECK(WriteBytesUtf8Path(path, json, static_cast<size_t>(written)));
    CHECK(ReloadGameConfig(doc, localDoc, config, path, noLocal, &mock, observe));
    CHECK(doc != nullptr && config != nullptr);
    CHECK(issueCount == 0);
    CHECK(version == GLFD::kGameConfigVersion);
    CHECK(config->window.width == 800);
    CHECK(config->simulation.entityCount == 5000);
    CHECK(config->simulation.maxSpeed == 3.5f);
    CHECK(config->boidProfiles.GetSize() == 1);
    CHECK(ViewEquals(config->window.title, kExpectedTitle, sizeof(kExpectedTitle)));

    const Document* const   documentBefore = doc.get();
    const GameConfig* const configBefore   = config.get();
    // 文字列の実体そのものが動いていないことも見る
    const char* const       titleBytes     = config->window.title.Data();

    // --- 2. 壊して読み直す(本命)-------------------------------------------
    {
      // **わざと大きく作る。** 同じ Document へ読み直す実装だと、パースの前に
      // アリーナが Reset され、この内容が前回の文字列と同じ番地へ載る。
      // 小さい壊れファイルでは古いバイトが残ったままになり、ダングリングを
      // 見逃す(実際にこの検証を素朴な実装へ当てて確かめてある)
      char        broken[4096];
      std::memset(broken, 'Z', sizeof(broken));
      std::memcpy(broken, "{ \"pad\": \"", 10);
      // 閉じないまま終わらせる = 構文エラー
      std::memcpy(broken + sizeof(broken) - 12, "\", \"w\": 640,", 12);
      CHECK(WriteBytesUtf8Path(path, broken, sizeof(broken)));

      issueCount = 0xFFFFFFFFu;
      CHECK(!ReloadGameConfig(doc, localDoc, config, path, noLocal, &mock, observe));

      // 失敗でも診断の口は必ず1回呼ばれる(呼ばれなければ 0xFFFFFFFF のまま)
      CHECK(issueCount != 0xFFFFFFFFu);

      // 差し替えが起きていない = 前の Document / GameConfig がそのまま
      CHECK(doc.get() == documentBefore);
      CHECK(config.get() == configBefore);

      // 値が前回のまま
      CHECK(config->window.width == 800);
      CHECK(config->simulation.entityCount == 5000);
      CHECK(config->simulation.maxSpeed == 3.5f);
      CHECK(config->boidProfiles.GetSize() == 1);
      CHECK(config->boidProfiles[0].viewRadius == 7.0f);

      // **文字列も無傷。** 同じ Document へ読み直していたらここが壊れる
      CHECK(config->window.title.Data() == titleBytes);
      CHECK(ViewEquals(config->window.title, kExpectedTitle, sizeof(kExpectedTitle)));
    }

    // --- 3. 直して読み直す ---------------------------------------------------
    {
      static const char kFixed[] =
        "{ \"$version\": 2,"
        "  \"window\": { \"width\": 1600 },"
        "  \"simulation\": { \"entityCount\": 7000, \"maxSpeed\": 1.25 } }";
      CHECK(WriteBytesUtf8Path(path, kFixed, sizeof(kFixed) - 1));

      CHECK(ReloadGameConfig(doc, localDoc, config, path, noLocal, &mock, observe));
      CHECK(doc.get() != documentBefore);     // 差し替わった
      CHECK(config.get() != configBefore);
      CHECK(config->window.width == 1600);
      CHECK(config->simulation.entityCount == 7000);
      CHECK(config->simulation.maxSpeed == 1.25f);

      // 書いていないフィールドは既定値へ戻る。**前回の値は引き継がれない**
      CHECK(config->boidProfiles.GetSize() == 0);
      CHECK(config->window.title == StringView("ECS Boids Engine"));
    }

    // `.local.json` が無い場合、Local の診断は**一度も呼ばれない**(不在は正常)
    CHECK(localCalls == 0u);

    // Document を先に捨てると config->window.title が宙に浮く (R0-5)。
    // 実際の保持側 (BoidDemoScene / GameEngine) と同じ順で捨てる
    config.reset();
    localDoc.reset();
    doc.reset();

    DeleteFileUtf8(path);
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonRealData (1-7)");

  TestShippedConfigLoads();
  TestJapaneseRoundTrip();
  TestBrokenAndMissingFiles();
  TestOlderConfigStillLoads();
  TestReloadKeepsPreviousOnFailure();

  return GLFD::Test::Summarize();
}
