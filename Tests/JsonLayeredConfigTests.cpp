/**
 * @file  JsonLayeredConfigTests.cpp
 * @brief フェーズ 2-3: レイヤ設定 — T-56(まずは Overlay フラグの効果)
 *
 * @details
 *  方式 B は「同じ構造体へ 2 回読む」。R3-4 により既定値なしの `Member` は
 *  キーが無ければ値に触らないので、上書きの意味論は**自然に**成立する。
 *  邪魔をするのは次の 2 つだけで、`ArchiveFlags::Overlay` はそれを抑える。
 *
 *   - 既定値つき `Member`: キーが無いと `v = def` で **1 回目の値を潰す**
 *   - `RequiredMember`:    キーが無いと `MissingRequired` で Fatal になる
 *
 *  **T-56 の核心は「1 回目(通常モード)が 1 バイトも変わっていない」こと**でもある。
 *  抑制が通常経路へ漏れていないかを、同じ入力の対で確かめる。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <memory>

#include "Core/DynamicArray.h"
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

using GLFD::DynamicArray;
using GLFD::StringView;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::Document;
using GLFD::Json::LoadFromJson;
using GLFD::Json::ParseFlags;
using GLFD::Test::MockMemoryResource;
using GLFD::ConfigLayer;
using GLFD::GameConfig;
using GLFD::ReloadGameConfig;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonStringBuffer;

// ---------------------------------------------------------------------------
// 2 段ロードの対象。GameConfig と同じ形(ネスト / T[N] / DynamicArray)をなぞる
// ---------------------------------------------------------------------------

namespace LayerTest {

  struct Window {
    std::int32_t width  = 1024;
    std::int32_t height = 768;
  };

  template <class Ar>
  void Serialize(Ar& ar, Window& v) {
    (void)ar.Member("width", v.width, 1024);
    (void)ar.Member("height", v.height, 768);
  }

  struct Profile {
    StringView name;
    float      weight = 1.0f;
  };

  template <class Ar>
  void Serialize(Ar& ar, Profile& v) {
    (void)ar.Member("name", v.name);
    (void)ar.Member("weight", v.weight, 1.0f);
  }

  struct Settings {
    Window                window;
    float                 maxSpeed  = 2.0f;
    float                 bounds[2] = { 85.0f, 64.0f };
    StringView            title;
    DynamicArray<Profile> profiles;

    explicit Settings(GLFD::Memory::IMemoryResource* resource) : profiles(resource) {}
    Settings() = delete;
  };

  template <class Ar>
  void Serialize(Ar& ar, Settings& v) {
    (void)ar.Member("window", v.window);
    (void)ar.Member("maxSpeed", v.maxSpeed, 2.0f);
    (void)ar.Member("bounds", v.bounds);
    (void)ar.Member("title", v.title);          // 既定値なし = 無ければ触らない
    (void)ar.Member("profiles", v.profiles);
  }

  /// `RequiredMember` の抑制を見るための型
  struct Mandatory {
    std::int32_t must  = 0;
    std::int32_t extra = 0;
  };

  template <class Ar>
  void Serialize(Ar& ar, Mandatory& v) {
    (void)ar.RequiredMember("must", v.must);
    (void)ar.Member("extra", v.extra, 0);
  }

}

namespace {

  /// 1 回目(通常)と 2 回目(指定のフラグ)を続けて読む
  struct TwoPass {
    MockMemoryResource mock;
    Document           baseDoc{ &mock };
    Document           localDoc{ &mock };
    ArchiveContext     baseCtx{ baseDoc.Arena(), 0, ArchiveFlags::ReportUnknown };
    ArchiveContext     localCtx;

    explicit TwoPass(ArchiveFlags overlayFlags)
      : localCtx(localDoc.Arena(), 0, overlayFlags) {
    }
  };

  // -------------------------------------------------------------------------
  // T-56 Overlay フラグの効果
  // -------------------------------------------------------------------------

  void TestOverlayKeepsDefaults() {
    GLFD::Test::BeginCase("T-56: with Overlay the second pass does not apply defaults");

    // 2 回目に "maxSpeed" と "window/height" を書かない。
    // 通常モードなら既定値 (2.0 / 768) で潰れ、Overlay なら 1 回目の値が残る
    static const char kBase[]  = "{\"window\":{\"width\":1280,\"height\":720},"
                                 " \"maxSpeed\":6.5}";
    static const char kLocal[] = "{\"window\":{\"width\":800}}";

    // --- Overlay あり: 書いていないフィールドは 1 回目のまま ------------------
    {
      TwoPass          t(ArchiveFlags::ReportUnknown | ArchiveFlags::Overlay);
      LayerTest::Settings cfg(&t.mock);

      CHECK(LoadFromJson(cfg, StringView(kBase), t.baseDoc, t.baseCtx, ParseFlags::None));
      CHECK(cfg.window.width == 1280);
      CHECK(cfg.window.height == 720);
      CHECK(cfg.maxSpeed == 6.5f);

      CHECK(LoadFromJson(cfg, StringView(kLocal), t.localDoc, t.localCtx, ParseFlags::None));
      CHECK(cfg.window.width == 800);      // 上書きされた
      CHECK(cfg.window.height == 720);     // **既定値 768 で潰れていない**
      CHECK(cfg.maxSpeed == 6.5f);         // **既定値 2.0 で潰れていない**
      CHECK(!t.localCtx.HasFatal());
    }

    // --- Overlay なし(通常モード): 従来どおり既定値で潰れる ------------------
    // これが「フラグを立てたときだけ挙動が変わる」ことの対照実験になる
    {
      TwoPass          t(ArchiveFlags::ReportUnknown);
      LayerTest::Settings cfg(&t.mock);

      CHECK(LoadFromJson(cfg, StringView(kBase), t.baseDoc, t.baseCtx, ParseFlags::None));
      CHECK(LoadFromJson(cfg, StringView(kLocal), t.localDoc, t.localCtx, ParseFlags::None));
      CHECK(cfg.window.width == 800);
      CHECK(cfg.window.height == 768);     // 既定値で潰れる(従来の挙動)
      CHECK(cfg.maxSpeed == 2.0f);         // 同上
    }
  }

  void TestOverlaySuppressesRequired() {
    GLFD::Test::BeginCase("T-56: with Overlay a missing RequiredMember is not fatal");

    // --- Overlay あり: 必須フィールドが無くても Fatal にならない -------------
    {
      TwoPass             t(ArchiveFlags::Overlay);
      LayerTest::Mandatory value;

      CHECK(LoadFromJson(value, StringView("{\"must\":7}"), t.baseDoc, t.baseCtx,
                         ParseFlags::None));
      CHECK(value.must == 7);

      CHECK(LoadFromJson(value, StringView("{\"extra\":3}"), t.localDoc, t.localCtx,
                         ParseFlags::None));
      CHECK(!t.localCtx.HasFatal());        // **Fatal にならない**
      CHECK(t.localCtx.IssueCount() == 0u); // Issue も記録しない
      CHECK(value.must == 7);               // 1 回目の値が残る
      CHECK(value.extra == 3);              // 書いてある方は上書きされる
    }

    // --- Overlay なし(通常モード): 従来どおり Fatal --------------------------
    {
      TwoPass             t(ArchiveFlags::None);
      LayerTest::Mandatory value;

      CHECK(LoadFromJson(value, StringView("{\"must\":7}"), t.baseDoc, t.baseCtx,
                         ParseFlags::None));
      CHECK(!LoadFromJson(value, StringView("{\"extra\":3}"), t.localDoc, t.localCtx,
                          ParseFlags::None));
      CHECK(t.localCtx.HasFatal());
      CHECK(t.localCtx.IssueCount() == 1u);
      CHECK(t.localCtx.Issues()[0].kind == ArchiveErrorKind::MissingRequired);
    }
  }

  void TestOverlayStillReportsMistakes() {
    GLFD::Test::BeginCase("T-56: Overlay suppresses only those two; diagnostics still fire");

    TwoPass             t(ArchiveFlags::ReportUnknown | ArchiveFlags::Overlay);
    LayerTest::Settings cfg(&t.mock);

    CHECK(LoadFromJson(cfg, StringView("{\"maxSpeed\":6.5}"), t.baseDoc, t.baseCtx,
                       ParseFlags::None));

    // 綴り間違い (maxSpeeed) と型違い (window/width) を含む上書き
    CHECK(LoadFromJson(cfg, StringView("{\"maxSpeeed\":9.0,\"window\":{\"width\":\"wide\"}}"),
                       t.localDoc, t.localCtx, ParseFlags::None));

    bool sawUnknown      = false;
    bool sawTypeMismatch = false;
    for (std::uint32_t i = 0; i < t.localCtx.IssueCount(); ++i) {
      const auto& issue = t.localCtx.Issues()[i];
      if (issue.kind == ArchiveErrorKind::UnknownField
          && issue.path == StringView("maxSpeeed")) {
        sawUnknown = true;
      }
      if (issue.kind == ArchiveErrorKind::TypeMismatch
          && issue.path == StringView("window/width")) {
        sawTypeMismatch = true;
      }
    }
    // **綴り間違いは検出される。** 抑えたら .local.json の書き損じが黙って無効になる
    CHECK(sawUnknown);
    CHECK(sawTypeMismatch);
    CHECK(!t.localCtx.HasFatal());

    // 型違いだったので 1 回目の値が残る
    CHECK(cfg.window.width == 1024);   // どちらのファイルにも無い = 1 回目の既定値
    CHECK(cfg.maxSpeed == 6.5f);
  }

  // -------------------------------------------------------------------------
  // ArrayWasOpened(2-3 論点4)
  // -------------------------------------------------------------------------

  void TestArrayOverwriteFailureKeepsContents() {
    GLFD::Test::BeginCase("T-56: a non-array overwrite keeps the container; [] clears it");

    static const char kBase[] = "{\"profiles\":[{\"name\":\"a\",\"weight\":2.0},"
                                "              {\"name\":\"b\",\"weight\":3.0}]}";

    // --- 型違いの上書き: 中身を保つ -----------------------------------------
    {
      TwoPass             t(ArchiveFlags::Overlay);
      LayerTest::Settings cfg(&t.mock);

      CHECK(LoadFromJson(cfg, StringView(kBase), t.baseDoc, t.baseCtx, ParseFlags::None));
      CHECK(cfg.profiles.GetSize() == 2);

      CHECK(LoadFromJson(cfg, StringView("{\"profiles\":42}"), t.localDoc, t.localCtx,
                         ParseFlags::None));
      // **消えない。** 上書きの失敗で直前のレイヤの値を壊さない
      CHECK(cfg.profiles.GetSize() == 2);
      if (cfg.profiles.GetSize() == 2) { CHECK(cfg.profiles[1].weight == 3.0f); }
      // 失敗したことは診断で分かる
      CHECK(t.localCtx.IssueCount() >= 1u);
      bool sawTypeMismatch = false;
      for (std::uint32_t i = 0; i < t.localCtx.IssueCount(); ++i) {
        if (t.localCtx.Issues()[i].kind == ArchiveErrorKind::TypeMismatch) {
          sawTypeMismatch = true;
        }
      }
      CHECK(sawTypeMismatch);
    }

    // --- 空配列の上書き: 意図どおり空にする(区別できていること)-------------
    {
      TwoPass             t(ArchiveFlags::Overlay);
      LayerTest::Settings cfg(&t.mock);

      CHECK(LoadFromJson(cfg, StringView(kBase), t.baseDoc, t.baseCtx, ParseFlags::None));
      CHECK(cfg.profiles.GetSize() == 2);

      CHECK(LoadFromJson(cfg, StringView("{\"profiles\":[]}"), t.localDoc, t.localCtx,
                         ParseFlags::None));
      CHECK(cfg.profiles.GetSize() == 0);   // `[]` は「空にする」という正当な指示
      CHECK(!t.localCtx.HasIssues());
    }

    // --- 通常モードでも同じ扱い(このガードは Overlay 専用ではない)----------
    {
      TwoPass             t(ArchiveFlags::None);
      LayerTest::Settings cfg(&t.mock);

      CHECK(LoadFromJson(cfg, StringView(kBase), t.baseDoc, t.baseCtx, ParseFlags::None));
      CHECK(LoadFromJson(cfg, StringView("{\"profiles\":\"oops\"}"), t.localDoc, t.localCtx,
                         ParseFlags::None));
      CHECK(cfg.profiles.GetSize() == 2);
    }
  }

  // --- 一時ファイル -----------------------------------------------------------

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

  void MakeTempPath(char* out, size_t capacity, const char* leaf) {
    wchar_t     wide[512];
    const DWORD length    = ::GetTempPathW(512, wide);
    char        utf8[1024];
    const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                  utf8, sizeof(utf8), nullptr, nullptr);
    std::snprintf(out, capacity, "%.*s%s", (converted > 0 ? converted : 0), utf8, leaf);
  }

  // -------------------------------------------------------------------------
  // 本番の 2 段ロードをそのまま呼ぶ受け皿
  // -------------------------------------------------------------------------

  /// `ReloadGameConfig` の `onIssues` を受けて、レイヤごとの結果を覚えておく
  class LayerObserver {
  public:
    static constexpr int kBase  = 0;
    static constexpr int kLocal = 1;

    void operator()(ConfigLayer layer, const char* path,
                    const Document& doc, const ArchiveContext& ctx) {
      const int index = (layer == ConfigLayer::Base) ? kBase : kLocal;
      ++m_calls[index];
      m_path[index]        = path;
      m_issueCount[index]  = ctx.IssueCount();
      m_fatal[index]       = ctx.HasFatal();
      m_version[index]     = ctx.Version();
      m_parseFailed[index] = doc.HasError();

      // どちらのファイル由来かは、ctx が分かれているだけで判別できる (2-3 論点2)
      for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
        const auto kind = ctx.Issues()[i].kind;
        if (kind == ArchiveErrorKind::UnknownField) { m_unknown[index] = true; }
        if (kind == ArchiveErrorKind::TypeMismatch) { m_typeMismatch[index] = true; }
        if (kind == ArchiveErrorKind::ArrayLengthMismatch) { m_arrayLength[index] = true; }
      }
    }

    [[nodiscard]] int           Calls(int i)       const { return m_calls[i]; }
    [[nodiscard]] std::uint32_t IssueCount(int i)  const { return m_issueCount[i]; }
    [[nodiscard]] bool          Fatal(int i)       const { return m_fatal[i]; }
    [[nodiscard]] std::uint32_t Version(int i)     const { return m_version[i]; }
    [[nodiscard]] bool          ParseFailed(int i) const { return m_parseFailed[i]; }
    [[nodiscard]] bool          SawUnknown(int i)  const { return m_unknown[i]; }
    [[nodiscard]] bool          SawMismatch(int i) const { return m_typeMismatch[i]; }
    [[nodiscard]] bool          SawArrayLength(int i) const { return m_arrayLength[i]; }

  private:
    int           m_calls[2]        = { 0, 0 };
    const char*   m_path[2]         = { nullptr, nullptr };
    std::uint32_t m_issueCount[2]   = { 0, 0 };
    bool          m_fatal[2]        = { false, false };
    std::uint32_t m_version[2]      = { 0, 0 };
    bool          m_parseFailed[2]  = { false, false };
    bool          m_unknown[2]      = { false, false };
    bool          m_typeMismatch[2] = { false, false };
    bool          m_arrayLength[2]  = { false, false };
  };

  int g_layeredLoadCount = 0;

  /// 2 つのファイルを書いてから**本番の 2 段ロード**を回す
  struct LayeredLoad {
    MockMemoryResource          mock;
    std::unique_ptr<Document>   baseDoc;
    std::unique_ptr<Document>   localDoc;
    std::unique_ptr<GameConfig> config;
    LayerObserver               observer;

    LayeredLoad() {
      // インスタンスごとに別のファイル名にする(同時に生きていても衝突しない)
      char leaf[64];
      const int id = ++g_layeredLoadCount;
      std::snprintf(leaf, sizeof(leaf), "glfd_layer_%d_base.jsonc", id);
      MakeTempPath(m_basePath, sizeof(m_basePath), leaf);
      std::snprintf(leaf, sizeof(leaf), "glfd_layer_%d_local.json", id);
      MakeTempPath(m_localPath, sizeof(m_localPath), leaf);
    }

    ~LayeredLoad() {
      // Document より先に config を捨てる (R0-5)
      config.reset();
      localDoc.reset();
      baseDoc.reset();
      DeleteFileUtf8(m_basePath);
      DeleteFileUtf8(m_localPath);
    }

    LayeredLoad(const LayeredLoad&)            = delete;
    LayeredLoad& operator=(const LayeredLoad&) = delete;

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

  /// 実物の `GameConfig` と同じ形の土台。**日本語は書かない (C5297)**
  const char* const kBaseConfig =
    "// shared, hand written. comments survive because nothing rewrites this file\n"
    "{ \"$version\": 2,\n"
    "  \"window\": { \"width\": 1280, \"height\": 720, \"title\": \"shared\" },\n"
    "  \"world\": { \"halfExtent\": [85.0, 64.0] },\n"
    "  \"simulation\": { \"entityCount\": 20000, \"maxSpeed\": 2.0 },\n"
    "  \"boidProfiles\": [ { \"name\": \"a\", \"viewRadius\": 6.0 },\n"
    "                     { \"name\": \"b\", \"viewRadius\": 7.0 } ],\n"
    "}\n";

  // -------------------------------------------------------------------------
  // T-55 上書きの意味論(中核)
  // -------------------------------------------------------------------------

  void TestOverlaySemantics() {
    GLFD::Test::BeginCase("T-55: only what the local layer writes is overwritten");

    // --- 深いマージ: window/width だけ書く。height と title は残る -----------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, "{ \"window\": { \"width\": 800 } }"));
      CHECK(load.config != nullptr);
      if (load.config != nullptr) {
        CHECK(load.config->window.width == 800);       // 上書きされた
        CHECK(load.config->window.height == 720);      // **.jsonc の値が残る**
        CHECK(load.config->window.title == StringView("shared"));
        CHECK(load.config->simulation.maxSpeed == 2.0f);
        CHECK(load.config->world.halfExtent[0] == 85.0f);
        CHECK(load.config->boidProfiles.GetSize() == 2);
      }
      CHECK(load.observer.Calls(LayerObserver::kBase) == 1);
      CHECK(load.observer.Calls(LayerObserver::kLocal) == 1);
    }

    // --- スカラの上書き ------------------------------------------------------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, "{ \"simulation\": { \"maxSpeed\": 6.5 } }"));
      if (load.config != nullptr) {
        CHECK(load.config->simulation.maxSpeed == 6.5f);
        CHECK(load.config->simulation.entityCount == 20000);   // 兄弟は無傷
        CHECK(load.config->window.width == 1280);
      }
    }

    // --- 配列は**丸ごと置換** ------------------------------------------------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, "{ \"boidProfiles\": [ { \"name\": \"solo\" } ] }"));
      if (load.config != nullptr) {
        // 2 -> 1。要素単位のマージという曖昧な仕様を避けている
        CHECK(load.config->boidProfiles.GetSize() == 1);
        if (load.config->boidProfiles.GetSize() == 1) {
          CHECK(load.config->boidProfiles[0].name == StringView("solo"));
          // **書いていないフィールドは構造体の既定値**になる。
          // 置換前の 1 番目のプロファイル (6.0) が混ざらないこと
          CHECK(load.config->boidProfiles[0].viewRadius == 5.0f);
        }
      }
    }

    // --- T[N] も置換。**全要素を書いたときだけ**上書きされる -----------------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, "{ \"world\": { \"halfExtent\": [10.0, 20.0] } }"));
      if (load.config != nullptr) {
        CHECK(load.config->world.halfExtent[0] == 10.0f);
        CHECK(load.config->world.halfExtent[1] == 20.0f);
      }
    }

    // --- T[N] を**部分的に**書いた場合: 上書きを丸ごと無視する ---------------
    // 固定長配列には「既定へ戻す」手段が無いので、部分的に書かせると残りが
    // .jsonc の値のまま混ざる。DynamicArray を丸ごと置換にしたのと同じ理由で
    // 添字ごとに混ざる形を避け、**全要素を書くことを要求する**
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, "{ \"world\": { \"halfExtent\": [10.0] } }"));
      if (load.config != nullptr) {
        CHECK(load.config->world.halfExtent[0] == 85.0f);   // 1 要素目も書かれない
        CHECK(load.config->world.halfExtent[1] == 64.0f);
      }
      // 黙って無視はしない。要素数が違うことは診断に出る
      CHECK(load.observer.SawArrayLength(LayerObserver::kLocal));
    }

    // --- 空オブジェクト: 何も変わらない --------------------------------------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, "{}"));
      if (load.config != nullptr) {
        CHECK(load.config->window.width == 1280);
        CHECK(load.config->window.height == 720);
        CHECK(load.config->simulation.maxSpeed == 2.0f);
        CHECK(load.config->boidProfiles.GetSize() == 2);
      }
      CHECK(load.observer.IssueCount(LayerObserver::kLocal) == 0u);
    }

    // --- `.local.json` が無い: 正常に完了し、診断の口すら呼ばれない ----------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, nullptr));           // **エラーにしない**
      if (load.config != nullptr) {
        CHECK(load.config->window.width == 1280);
        CHECK(load.config->boidProfiles.GetSize() == 2);
      }
      CHECK(load.observer.Calls(LayerObserver::kBase) == 1);
      // 不在は既定の状態。呼ぶと毎回警告が出ることになる
      CHECK(load.observer.Calls(LayerObserver::kLocal) == 0);
    }
  }

  // -------------------------------------------------------------------------
  // T-57 失敗時
  // -------------------------------------------------------------------------

  void TestLocalFailuresKeepBase() {
    GLFD::Test::BeginCase("T-57: a broken local layer keeps the base values and is reported");

    // --- 型不一致: .jsonc の値が保たれる -------------------------------------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig,
                     "{ \"simulation\": { \"maxSpeed\": \"fast\" },"
                     "  \"window\": { \"width\": \"wide\" },"
                     "  \"boidProfiles\": 42 }"));
      if (load.config != nullptr) {
        CHECK(load.config->simulation.maxSpeed == 2.0f);      // **保たれる**
        CHECK(load.config->window.width == 1280);             // **保たれる**
        CHECK(load.config->boidProfiles.GetSize() == 2);      // **消えない**
      }
      CHECK(load.observer.SawMismatch(LayerObserver::kLocal));
      CHECK(!load.observer.Fatal(LayerObserver::kLocal));
      // 基底側には診断が出ていない = 出所が分かる (論点2)
      CHECK(load.observer.IssueCount(LayerObserver::kBase) == 0u);
    }

    // --- 構文エラー: .jsonc の結果が保たれ、診断が出る -----------------------
    {
      LayeredLoad load;
      CHECK(load.Run(kBaseConfig, "{ \"simulation\": "));      // 途中で切れている
      if (load.config != nullptr) {
        CHECK(load.config->simulation.maxSpeed == 2.0f);
        CHECK(load.config->window.width == 1280);
        CHECK(load.config->boidProfiles.GetSize() == 2);
      }
      CHECK(load.observer.Calls(LayerObserver::kLocal) == 1);
      CHECK(load.observer.ParseFailed(LayerObserver::kLocal));
      CHECK(!load.observer.ParseFailed(LayerObserver::kBase));
    }

    // --- 基底が壊れていれば従来どおり失敗する(差し替えない)-----------------
    {
      LayeredLoad load;
      CHECK(!load.Run("{ \"window\": ", "{ \"simulation\": { \"maxSpeed\": 6.5 } }"));
      CHECK(load.config == nullptr);                           // 差し替えが起きていない
      CHECK(load.observer.ParseFailed(LayerObserver::kBase));
      CHECK(load.observer.Calls(LayerObserver::kLocal) == 0);  // 2 段目へ進まない
    }
  }

  // -------------------------------------------------------------------------
  // T-58 診断・ダンプとの結合
  // -------------------------------------------------------------------------

  void TestDiagnosticsIntegration() {
    GLFD::Test::BeginCase("T-58: the dump shows merged values; local typos surface as UnknownField");

    LayeredLoad load;
    CHECK(load.Run(kBaseConfig,
                   "{ \"simulation\": { \"maxSpeed\": 6.5, \"maxSpeeed\": 9.0 } }"));

    // **綴り間違いが UnknownField として出る**(Overlay でも診断は抑えない)
    CHECK(load.observer.SawUnknown(LayerObserver::kLocal));
    CHECK(!load.observer.SawUnknown(LayerObserver::kBase));

    // $version は .jsonc 側のものが採られる
    CHECK(load.observer.Version(LayerObserver::kBase) == GLFD::kGameConfigVersion);

    CHECK(load.config != nullptr);
    if (load.config == nullptr) { return; }

    // --- 2-4 の実効値ダンプが**マージ後の値**を出すこと ----------------------
    {
      JsonArena        arena(&load.mock);
      JsonStringBuffer buffer(arena);
      // **書き出しは常に最新版で行う** (A-2 §2.1)。古い版を渡すと移行分岐の
      // どちらの枝も通らず world のフィールドが落ちる。Debug では
      // GameConfig の Serialize が assert で止める
      CHECK(GLFD::Json::SaveToJson(*load.config, buffer, GLFD::kGameConfigVersion,
                                   /*pretty=*/true));

      char         dump[4096] = {};
      const size_t size = (buffer.Size() < sizeof(dump) - 1) ? buffer.Size() : sizeof(dump) - 1;
      std::memcpy(dump, buffer.View().Data(), size);

      CHECK(std::strstr(dump, "\"maxSpeed\": 6.5") != nullptr);   // 上書き後の値
      CHECK(std::strstr(dump, "\"width\": 1280") != nullptr);     // 基底の値
      CHECK(std::strstr(dump, "maxSpeeed") == nullptr);           // 未知フィールドは出ない
    }

    // --- リロード差分が**両方のファイル**の変更を拾うこと --------------------
    // 方式 B はマージ後の DOM を持たないので、2 枚をそれぞれ比べる (論点5)
    {
      LayeredLoad again;
      CHECK(again.Run(kBaseConfig,
                      "{ \"simulation\": { \"maxSpeed\": 8.0, \"maxSpeeed\": 9.0 } }"));
      CHECK(again.config != nullptr);
      if (again.config != nullptr) {
        std::uint32_t localChanges = 0;
        GLFD::Json::DiffValues(load.localDoc->Root(), again.localDoc->Root(),
                               [&localChanges](StringView) { ++localChanges; });
        CHECK(localChanges == 1u);      // maxSpeed 6.5 -> 8

        std::uint32_t baseChanges = 0;
        GLFD::Json::DiffValues(load.baseDoc->Root(), again.baseDoc->Root(),
                               [&baseChanges](StringView) { ++baseChanges; });
        CHECK(baseChanges == 0u);       // .jsonc は変えていない
      }
    }
  }

}

int main() {
  GLFD::Test::BeginSuite("JsonLayeredConfig (2-3)");

  TestOverlayKeepsDefaults();
  TestOverlaySuppressesRequired();
  TestOverlayStillReportsMistakes();
  TestArrayOverwriteFailureKeepsContents();
  TestOverlaySemantics();
  TestLocalFailuresKeepBase();
  TestDiagnosticsIntegration();

  return GLFD::Test::Summarize();
}
