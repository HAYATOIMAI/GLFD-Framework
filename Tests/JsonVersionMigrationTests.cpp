/**
 * @file  JsonVersionMigrationTests.cpp
 * @brief A-2: バージョン移行を 1 回通す — T-61 / T-62 / T-63
 *
 * @details
 *  R3-7 / R3-8(バージョンによる移行)は設計してから一度も実行されていなかった。
 *  `$version` を書いて読んでいるだけで、`ar.Version()` の分岐を通ったことがない。
 *  ここで**実害のない場所で 1 回通す**。
 *
 *  題材は v1 -> v2 の改名 `world.bounds` -> `world.halfExtent`。
 *  実体は半分の寸法なのに全体の幅と読める名前だったので、人工的な改名ではない。
 *
 *   - **T-61** 移行そのもの。v1 が読め、旧名が新メンバに入り、Issue が 0 件で、
 *     書き出しは常に最新版になる
 *   - **T-62** 版の境界。現在より大きい / 不在 / 型が違う / 範囲外
 *   - **T-63** レイヤとの結合。**上書きは基底と同じ版で読む**(A-2 論点4)
 *
 *  @note **本番の経路 (`ReloadGameConfig`) をそのまま呼ぶ。** 版の判定も
 *        `IsSupportedGameConfigVersion` / `GameConfigWasMigrated` を呼ぶ。
 *        写した判定をテストしても本番は一度も実行されない。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 *
 *  @note 一時ファイルのヘルパは `JsonLayeredConfigTests.cpp` と同じ形をしている。
 *        1 本に寄せるなら `LayerObserver` / `LayeredLoad` ごと共有ヘッダへ出す
 *        ことになり、A-2 の範囲を超えるので今回は写していない
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <memory>

#include "Core/GameConfig.h"
#include "Core/GameConfigLoad.h"
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

using GLFD::ConfigLayer;
using GLFD::GameConfig;
using GLFD::GameConfigMigrationNote;
using GLFD::GameConfigWasMigrated;
using GLFD::IsSupportedGameConfigVersion;
using GLFD::ReloadGameConfig;
using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::Document;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::LoadFromJson;
using GLFD::Json::ParseFlags;
using GLFD::Json::SaveToJson;
using GLFD::Test::MockMemoryResource;

namespace {

  // ---------------------------------------------------------------------------
  // フィクスチャ
  // ---------------------------------------------------------------------------

  /// **v1。旧名 `"bounds"` を使う。** これが移行の入力になる
  const char* const kV1Config =
    "// a v1 file, kept as a fixture\n"
    "{ \"$version\": 1,\n"
    "  \"window\": { \"width\": 640, \"height\": 480, \"title\": \"v1\" },\n"
    "  \"world\": { \"bounds\": [12.0, 34.0], \"colliderRadius\": 0.5 },\n"
    "  \"simulation\": { \"entityCount\": 100, \"maxSpeed\": 1.5 },\n"
    "}\n";

  /// v2(現行形式)。同じ値を新名で書いたもの
  const char* const kV2Config =
    "{ \"$version\": 2,\n"
    "  \"window\": { \"width\": 640, \"height\": 480, \"title\": \"v2\" },\n"
    "  \"world\": { \"halfExtent\": [12.0, 34.0], \"colliderRadius\": 0.5 },\n"
    "  \"simulation\": { \"entityCount\": 100, \"maxSpeed\": 1.5 },\n"
    "}\n";

  // ---------------------------------------------------------------------------
  // 一時ファイル
  // ---------------------------------------------------------------------------

  bool ToWide(const char* utf8, wchar_t* out, int capacity) {
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, capacity) > 0;
  }

  void DeleteFileUtf8(const char* utf8Path) {
    wchar_t wide[1024];
    if (ToWide(utf8Path, wide, 1024)) { ::DeleteFileW(wide); }
  }

  bool WriteTextUtf8Path(const char* utf8Path, const char* text) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) { return false; }
    const HANDLE handle = ::CreateFileW(wide, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { return false; }
    const DWORD size    = static_cast<DWORD>(std::strlen(text));
    DWORD       written = 0;
    const bool  ok      = (::WriteFile(handle, text, size, &written, nullptr) != 0)
                       && (written == size);
    ::CloseHandle(handle);
    return ok;
  }

  /// @warning 呼び出し側の配列へ書く。**共有の static を返さない**
  ///          (2-3 の T-41 で、2 つのパスが同じ実体を指す事故を起こしている)
  void MakeTempPath(char* out, size_t capacity, const char* leaf) {
    wchar_t     wide[512];
    const DWORD length    = ::GetTempPathW(512, wide);
    char        utf8[1024];
    const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                  utf8, sizeof(utf8), nullptr, nullptr);
    std::snprintf(out, capacity, "%.*s%s", (converted > 0 ? converted : 0), utf8, leaf);
  }

  // ---------------------------------------------------------------------------
  // 本番の 2 段ロードをそのまま呼ぶ受け皿
  // ---------------------------------------------------------------------------

  class VersionObserver {
  public:
    static constexpr int kBase  = 0;
    static constexpr int kLocal = 1;

    void operator()(ConfigLayer layer, const char* /*path*/,
                    const Document& doc, const ArchiveContext& ctx) {
      const int index = (layer == ConfigLayer::Base) ? kBase : kLocal;
      ++m_calls[index];
      m_issueCount[index]  = ctx.IssueCount();
      m_fatal[index]       = ctx.HasFatal();
      m_version[index]     = ctx.Version();
      m_parseFailed[index] = doc.HasError();
      m_unknown[index]     = false;
      m_typeMismatch[index] = false;

      for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
        const auto kind = ctx.Issues()[i].kind;
        if (kind == ArchiveErrorKind::UnknownField)  { m_unknown[index] = true; }
        if (kind == ArchiveErrorKind::TypeMismatch)  { m_typeMismatch[index] = true; }
      }
    }

    [[nodiscard]] int           Calls(int i)       const { return m_calls[i]; }
    [[nodiscard]] std::uint32_t IssueCount(int i)  const { return m_issueCount[i]; }
    [[nodiscard]] bool          Fatal(int i)       const { return m_fatal[i]; }
    [[nodiscard]] std::uint32_t Version(int i)     const { return m_version[i]; }
    [[nodiscard]] bool          ParseFailed(int i) const { return m_parseFailed[i]; }
    [[nodiscard]] bool          SawUnknown(int i)  const { return m_unknown[i]; }
    [[nodiscard]] bool          SawMismatch(int i) const { return m_typeMismatch[i]; }

  private:
    int           m_calls[2]        = { 0, 0 };
    std::uint32_t m_issueCount[2]   = { 0, 0 };
    bool          m_fatal[2]        = { false, false };
    std::uint32_t m_version[2]      = { 0, 0 };
    bool          m_parseFailed[2]  = { false, false };
    bool          m_unknown[2]      = { false, false };
    bool          m_typeMismatch[2] = { false, false };
  };

  int g_loadCount = 0;

  /// 2 つのファイルを書いてから **`ReloadGameConfig` をそのまま**回す
  struct MigrationLoad {
    MockMemoryResource          mock;
    std::unique_ptr<Document>   baseDoc;
    std::unique_ptr<Document>   localDoc;
    std::unique_ptr<GameConfig> config;
    VersionObserver             observer;

    MigrationLoad() {
      char      leaf[64];
      const int id = ++g_loadCount;
      std::snprintf(leaf, sizeof(leaf), "glfd_mig_%d_base.jsonc", id);
      MakeTempPath(m_basePath, sizeof(m_basePath), leaf);
      std::snprintf(leaf, sizeof(leaf), "glfd_mig_%d_local.json", id);
      MakeTempPath(m_localPath, sizeof(m_localPath), leaf);
    }

    ~MigrationLoad() {
      // Document より先に config を捨てる (R0-5)
      config.reset();
      localDoc.reset();
      baseDoc.reset();
      DeleteFileUtf8(m_basePath);
      DeleteFileUtf8(m_localPath);
    }

    MigrationLoad(const MigrationLoad&)            = delete;
    MigrationLoad& operator=(const MigrationLoad&) = delete;

    /// `local == nullptr` なら `.local.json` を**置かない**(不在は正常)
    [[nodiscard]] bool Run(const char* base, const char* local) {
      if (!WriteTextUtf8Path(m_basePath, base)) { return false; }
      if (local != nullptr) {
        if (!WriteTextUtf8Path(m_localPath, local)) { return false; }
      }
      else {
        DeleteFileUtf8(m_localPath);
      }
      return ReloadGameConfig(baseDoc, localDoc, config, m_basePath, m_localPath,
                              &mock, observer);
    }

  private:
    char m_basePath[1024]  = {};
    char m_localPath[1024] = {};
  };

  /// 出力バッファに `needle` が現れるか
  [[nodiscard]] bool Contains(StringView text, const char* needle) {
    const size_t length = std::strlen(needle);
    if (length == 0 || text.Size() < length) { return false; }
    for (size_t i = 0; i + length <= text.Size(); ++i) {
      if (std::memcmp(text.Data() + i, needle, length) == 0) { return true; }
    }
    return false;
  }

  // ===========================================================================
  // T-61 バージョン移行(中核)
  // ===========================================================================

  void TestV1FixtureLoadsIntoNewMember() {
    GLFD::Test::BeginCase("T-61: a v1 file loads and the old key lands in the new member");

    MigrationLoad load;
    CHECK(load.Run(kV1Config, nullptr));
    CHECK(load.config != nullptr);
    if (load.config == nullptr) { return; }

    // 版が読めていること
    CHECK(load.observer.Version(VersionObserver::kBase) == 1u);

    // **旧名が `UnknownField` にならないこと。** ここが移行の要
    CHECK(load.observer.IssueCount(VersionObserver::kBase) == 0u);
    CHECK(!load.observer.SawUnknown(VersionObserver::kBase));
    CHECK(!load.observer.Fatal(VersionObserver::kBase));

    // 旧名 "bounds" の値が新メンバ halfExtent に入っていること
    CHECK(load.config->world.halfExtent[0] == 12.0f);
    CHECK(load.config->world.halfExtent[1] == 34.0f);

    // 移行と無関係なフィールドも普通に読めていること
    CHECK(load.config->window.width == 640);
    CHECK(load.config->world.colliderRadius == 0.5f);
    CHECK(load.config->simulation.entityCount == 100);
  }

  void TestV2FixtureLoads() {
    GLFD::Test::BeginCase("T-61: a v2 file loads through the current branch");

    MigrationLoad load;
    CHECK(load.Run(kV2Config, nullptr));
    CHECK(load.config != nullptr);
    if (load.config == nullptr) { return; }

    CHECK(load.observer.Version(VersionObserver::kBase) == GLFD::kGameConfigVersion);
    CHECK(load.observer.IssueCount(VersionObserver::kBase) == 0u);
    CHECK(load.config->world.halfExtent[0] == 12.0f);
    CHECK(load.config->world.halfExtent[1] == 34.0f);

    // v2 のファイルに旧名を書いても読まれない(移行は一方向で、v2 の枝には無い)
    MigrationLoad stale;
    CHECK(stale.Run("{ \"$version\": 2, \"world\": { \"bounds\": [1.0, 2.0] } }", nullptr));
    if (stale.config != nullptr) {
      CHECK(stale.observer.SawUnknown(VersionObserver::kBase));
      CHECK(stale.config->world.halfExtent[0] == 85.0f);   // 構造体の既定値
    }
  }

  void TestWritingIsAlwaysNewest() {
    GLFD::Test::BeginCase("T-61: writing always produces the newest version");

    MigrationLoad load;
    CHECK(load.Run(kV1Config, nullptr));
    CHECK(load.config != nullptr);
    if (load.config == nullptr) { return; }

    JsonArena        arena(&load.mock);
    JsonStringBuffer buffer(arena);
    CHECK(SaveToJson(*load.config, buffer, GLFD::kGameConfigVersion, /*pretty=*/true));

    // v1 を読んで保存すると v2 になる。**旧名は 1 度も出ない**
    CHECK(Contains(buffer.View(), "\"$version\": 2"));
    CHECK(Contains(buffer.View(), "\"halfExtent\""));
    CHECK(!Contains(buffer.View(), "\"bounds\""));

    // 読み戻して同じ値になること(移行が往復で壊れていないこと)
    MockMemoryResource mock;
    Document           doc(&mock);
    ArchiveContext     ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);
    GameConfig         reloaded(&mock);

    CHECK(LoadFromJson(reloaded, buffer.View(), doc, ctx, ParseFlags::None));
    CHECK(ctx.IssueCount() == 0u);
    CHECK(ctx.Version() == GLFD::kGameConfigVersion);
    CHECK(reloaded.world.halfExtent[0] == load.config->world.halfExtent[0]);
    CHECK(reloaded.world.halfExtent[1] == load.config->world.halfExtent[1]);
  }

  void TestMigrationIsDetectable() {
    GLFD::Test::BeginCase("T-61: a caller can tell that a migration happened");

    // 判定は本番と同じ 1 本を呼ぶ(`ConfigDiagnosticsLogger` もこれを引く)
    MigrationLoad old;
    CHECK(old.Run(kV1Config, nullptr));
    CHECK(GameConfigWasMigrated(old.observer.Version(VersionObserver::kBase)));
    CHECK(IsSupportedGameConfigVersion(old.observer.Version(VersionObserver::kBase)));

    MigrationLoad current;
    CHECK(current.Run(kV2Config, nullptr));
    CHECK(!GameConfigWasMigrated(current.observer.Version(VersionObserver::kBase)));

    // 何が変わったかの 1 行。**移行が起きた版でだけ中身がある**
    CHECK(std::strlen(GameConfigMigrationNote(0u)) > 0);
    CHECK(std::strlen(GameConfigMigrationNote(1u)) > 0);
    CHECK(std::strlen(GameConfigMigrationNote(GLFD::kGameConfigVersion)) == 0);
  }

  // ===========================================================================
  // T-62 バージョンの境界
  // ===========================================================================

  void TestFutureVersionIsRejected() {
    GLFD::Test::BeginCase("T-62: a version newer than this build is refused, not guessed at");

    MigrationLoad load;

    // まず正常に読んでおく。**直前の設定が残ることまで見る** (R3-37)
    CHECK(load.Run(kV2Config, nullptr));
    CHECK(load.config != nullptr);
    if (load.config == nullptr) { return; }
    const GameConfig* const before = load.config.get();

    CHECK(!load.Run("{ \"$version\": 99, \"window\": { \"width\": 111 } }", nullptr));
    CHECK(load.config.get() == before);          // 差し替わっていない
    CHECK(load.config->window.width == 640);     // 前回の値のまま

    // 版そのものは読めている(拒否したのは版を見た上での判断である)
    CHECK(load.observer.Version(VersionObserver::kBase) == 99u);
    CHECK(!IsSupportedGameConfigVersion(99u));

    // 2 段目へは進まない。上書きを未来の版の語彙で読むことになるため
    CHECK(load.observer.Calls(VersionObserver::kLocal) == 0);
  }

  void TestMissingVersionReadsAsOldest() {
    GLFD::Test::BeginCase("T-62: a file with no $version is read as the oldest format");

    MigrationLoad load;
    CHECK(load.Run("{ \"world\": { \"bounds\": [7.0, 8.0] } }", nullptr));
    CHECK(load.config != nullptr);
    if (load.config == nullptr) { return; }

    CHECK(load.observer.Version(VersionObserver::kBase) == 0u);   // R3-7
    CHECK(load.observer.IssueCount(VersionObserver::kBase) == 0u);

    // 0 < 2 なので旧名の枝を通る。**書き忘れた人には警告が出る**(論点3)
    CHECK(load.config->world.halfExtent[0] == 7.0f);
    CHECK(GameConfigWasMigrated(0u));
  }

  void TestBrokenVersionDoesNotCrash() {
    GLFD::Test::BeginCase("T-62: a malformed $version is diagnosed, not guessed at");

    // (a) 型が違う
    {
      MigrationLoad load;
      CHECK(load.Run("{ \"$version\": \"two\", \"world\": { \"bounds\": [3.0, 4.0] } }",
                     nullptr));
      CHECK(load.config != nullptr);
      if (load.config != nullptr) {
        CHECK(load.observer.SawMismatch(VersionObserver::kBase));
        CHECK(!load.observer.Fatal(VersionObserver::kBase));   // 読み自体は続く
        CHECK(load.observer.Version(VersionObserver::kBase) == 0u);
        CHECK(load.config->world.halfExtent[0] == 3.0f);       // 0 なので旧名の枝
      }
    }

    // (b) 32 ビットに収まらない
    {
      MigrationLoad load;
      CHECK(load.Run("{ \"$version\": 99999999999, \"world\": { \"bounds\": [5.0, 6.0] } }",
                     nullptr));
      CHECK(load.config != nullptr);
      if (load.config != nullptr) {
        CHECK(load.observer.SawMismatch(VersionObserver::kBase));
        CHECK(load.observer.Version(VersionObserver::kBase) == 0u);
        CHECK(load.config->world.halfExtent[0] == 5.0f);
      }
    }

    // (c) 負の数
    {
      MigrationLoad load;
      CHECK(load.Run("{ \"$version\": -1, \"world\": { \"bounds\": [9.0, 10.0] } }", nullptr));
      CHECK(load.config != nullptr);
      if (load.config != nullptr) {
        CHECK(load.observer.SawMismatch(VersionObserver::kBase));
        CHECK(load.observer.Version(VersionObserver::kBase) == 0u);
      }
    }
  }

  // ===========================================================================
  // T-63 レイヤとの結合(A-2 論点4)
  // ===========================================================================

  void TestOverlayFollowsTheBaseVersion() {
    GLFD::Test::BeginCase("T-63: the overlay is read with the base file's version");

    // --- 基底が v1: 上書きも**旧名**で書く ------------------------------------
    {
      MigrationLoad load;
      CHECK(load.Run(kV1Config, "{ \"world\": { \"bounds\": [1.0, 2.0] } }"));
      CHECK(load.config != nullptr);
      if (load.config != nullptr) {
        CHECK(load.observer.Version(VersionObserver::kLocal) == 1u);   // 基底の版
        CHECK(!load.observer.SawUnknown(VersionObserver::kLocal));
        CHECK(load.config->world.halfExtent[0] == 1.0f);
        CHECK(load.config->world.halfExtent[1] == 2.0f);
      }
    }

    // 基底が v1 のときに**新名**で書いても効かない。同じ語彙で書くのが規則である
    {
      MigrationLoad load;
      CHECK(load.Run(kV1Config, "{ \"world\": { \"halfExtent\": [1.0, 2.0] } }"));
      if (load.config != nullptr) {
        CHECK(load.observer.SawUnknown(VersionObserver::kLocal));
        CHECK(load.config->world.halfExtent[0] == 12.0f);   // 基底の値のまま
      }
    }

    // --- 基底が v2(通常のケース)---------------------------------------------
    {
      MigrationLoad load;
      CHECK(load.Run(kV2Config, "{ \"world\": { \"halfExtent\": [1.0, 2.0] } }"));
      CHECK(load.config != nullptr);
      if (load.config != nullptr) {
        CHECK(load.observer.Version(VersionObserver::kLocal) == GLFD::kGameConfigVersion);
        CHECK(!load.observer.SawUnknown(VersionObserver::kLocal));
        CHECK(load.config->world.halfExtent[0] == 1.0f);
      }
    }

    // **これが論点4 の核心。** 直す前は上書きの版が常に 0 になり、基底が v2 でも
    // 2 段目だけ v1 として読まれていた。その状態ではここが逆の結果になる
    {
      MigrationLoad load;
      CHECK(load.Run(kV2Config, "{ \"world\": { \"bounds\": [1.0, 2.0] } }"));
      if (load.config != nullptr) {
        CHECK(load.observer.SawUnknown(VersionObserver::kLocal));
        CHECK(load.config->world.halfExtent[0] == 12.0f);   // 基底の値のまま
      }
    }
  }

  void TestOverlayVersionIsIgnored() {
    GLFD::Test::BeginCase("T-63: a $version written in the overlay is ignored (R2-18q)");

    MigrationLoad load;
    CHECK(load.Run(kV2Config,
                   "{ \"$version\": 1, \"world\": { \"halfExtent\": [1.0, 2.0] } }"));
    CHECK(load.config != nullptr);
    if (load.config == nullptr) { return; }

    // 宣言された 1 ではなく、基底の 2 が使われる
    CHECK(load.observer.Version(VersionObserver::kLocal) == GLFD::kGameConfigVersion);

    // したがって上書きは**新名**として効く(1 が効いていれば無視されていた)
    CHECK(load.config->world.halfExtent[0] == 1.0f);
    CHECK(load.config->world.halfExtent[1] == 2.0f);

    // 予約キーなので `UnknownField` にはならない (R3-10)。
    // 書いてあること自体の警告は `ConfigDiagnosticsLogger` が出す(Logger 層)
    CHECK(!load.observer.SawUnknown(VersionObserver::kLocal));
    CHECK(load.observer.IssueCount(VersionObserver::kLocal) == 0u);

    // 観測子が DOM から拾えること。ログの警告はこの判定を使っている
    CHECK(load.localDoc != nullptr);
    if (load.localDoc != nullptr) {
      CHECK(load.localDoc->Query(GLFD::Json::kVersionKey) != nullptr);
    }
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonVersionMigration (A-2)");

  TestV1FixtureLoadsIntoNewMember();
  TestV2FixtureLoads();
  TestWritingIsAlwaysNewest();
  TestMigrationIsDetectable();

  TestFutureVersionIsRejected();
  TestMissingVersionReadsAsOldest();
  TestBrokenVersionDoesNotCrash();

  TestOverlayFollowsTheBaseVersion();
  TestOverlayVersionIsIgnored();

  return GLFD::Test::Summarize();
}
