#pragma once

/**
 * @file  GameConfigLog.h
 * @brief 設定の診断をゲームのログへ流す(フェーズ 2-4)
 *
 * @details
 *  `Core/Json/JsonDiagnostics.h` が**行を組み立て**、ここが**それを `Logger` へ流す**。
 *  境目をこの 1 ファイルに置いてあるのは層の規律のため — `Source/Core/Json/` は
 *  `Logger` を知らない(T-53 が `/showIncludes` で検証する)。
 *
 *  @note `GameConfigLoad.h` と分けてあるのは、あちらを `Logger` から切り離しておく
 *        ため。`JsonRealDataTests` は読み直しの手順(`ReloadGameConfig`)だけを
 *        使うので、そこへ `<iostream>` を引き込みたくない。
 *
 *  @note 以前は同じ内容の `LogConfigIssues` が `Engine.cpp` と
 *        `BoidDemoScene.cpp` に**複製**されており、片方だけが
 *        `DiagnosticsTruncated()` を見ていた。1 本に寄せてある。
 */

#include <cstdint>

#include "GameConfig.h"
#include "GameConfigLoad.h"
#include "Logger.h"
#include "MemoryResource.h"
#include "Json/Json.h"

namespace GLFD {

  /**
   * @brief 読み込みの診断を出す。**Issue が無ければ 1 行も出さない**
   *
   * @param doc  読み込みに使った `Document`(失敗位置の算出に使う)
   * @param ctx  読み込みに使った文脈
   * @param path 表示するファイル名
   *
   * @details
   *  出力は 2 種類:
   *   - **パースの失敗**: `Resource/GameConfig.jsonc(42,9): ObjectMissingName`。
   *     MSVC の診断と同じ形式で、桁はコードポイント単位 (R1-2)
   *   - **Issue**: 読めたが一部おかしい場合。Fatal は `LOG_ERROR`、
   *     非 Fatal は `LOG_WARN` に分ける(「読めなかった」と
   *     「起動はしたが一部おかしい」は別物である)
   */
  inline void LogConfigDiagnostics(const Json::Document& doc, const Json::ArchiveContext& ctx,
                                   const char* path) {
    // パースに失敗していれば位置を出す。成否そのものは呼び出し側が既に出している
    if (doc.HasError()) {
      char         line[Json::kDiagnosticLineCapacity];
      const size_t size = Json::FormatParseError(line, sizeof(line), StringView(path),
                                                 doc.Error().code, doc.Error().offset,
                                                 doc.Source());
      LOG_ERROR("%.*s", static_cast<int>(size), line);
    }

    Json::ForEachIssueLine(ctx, StringView(path), [](StringView line, bool isFatal) {
      if (isFatal) { LOG_ERROR("%.*s", static_cast<int>(line.Size()), line.Data()); }
      else         { LOG_WARN("%.*s", static_cast<int>(line.Size()), line.Data()); }
    });
  }

  /**
   * @brief 2 段ロード (2-3) の診断をまとめて出す。`ReloadGameConfig` の
   *        `onIssues` にそのまま渡せる
   *
   * @details
   *  ファイルごとに `ArchiveContext` が分かれているので、**どちらの由来かは
   *  ヘッダ行のファイル名でそのまま分かる**(2-4 の出力を変えずに済んでいる)。
   *
   *  併せて `$version` を見る。`.local.json` は上書き断片なので**バージョンは
   *  `.jsonc` のものを採用**し、`.local.json` 側に書いてあって食い違う場合だけ
   *  1 行警告する(`$version` は予約キーなので `UnknownField` にはならず、
   *  黙って無視されてしまうため)。
   *
   *  @note 状態(基底の版)を持つので**関数ではなく呼び出し可能なオブジェクト**に
   *        してある。`Engine` と `BoidDemoScene` が同じものを使う
   */
  class ConfigDiagnosticsLogger {
  public:
    void operator()(ConfigLayer layer, const char* path,
                    const Json::Document& doc, const Json::ArchiveContext& ctx) {
      LogConfigDiagnostics(doc, ctx, path);

      if (layer == ConfigLayer::Base) {
        m_sawBase     = true;
        m_baseVersion = ctx.Version();
        LogBaseVersion(path);
        return;
      }

      m_sawLocal = true;

      // 上書き断片は自前の版を持たない (R2-18q)。`$version` を書いても使われない
      // ので、**書いてあること自体**を 1 行警告する。`ctx.Version()` は A-2 以降
      // 基底の版になっているため、ここでは DOM を直接引く(2-1 の `Query`)。
      // 予約キーなので `UnknownField` にはならず、黙って無視されてしまう
      if (!doc.HasError() && doc.Query(Json::kVersionKey) != nullptr) {
        LOG_WARN("%s: $version is ignored in an overlay; the base file's %u is used",
                 path, m_baseVersion);
      }
    }

    /// ゲームが使うバージョン。**常に `.jsonc` 側**
    [[nodiscard]] std::uint32_t Version()  const noexcept { return m_baseVersion; }
    /// `.local.json` が実在して読まれたか(不在は正常なので false になる)
    [[nodiscard]] bool          SawLocal() const noexcept { return m_sawLocal; }

    /// 古い版を読んで移行分岐を通ったか (A-2)。**基底を読んだ後にだけ意味がある**
    [[nodiscard]] bool Migrated() const noexcept {
      return m_sawBase && GameConfigWasMigrated(m_baseVersion);
    }

    /// このビルドより新しい版で、`ReloadGameConfig` が読み込みを断ったか (A-2)
    [[nodiscard]] bool Rejected() const noexcept {
      return m_sawBase && !IsSupportedGameConfigVersion(m_baseVersion);
    }

  private:
    /// 基底の版について伝えるべきことを出す (A-2 論点2/3/5)
    void LogBaseVersion(const char* path) const {
      if (!IsSupportedGameConfigVersion(m_baseVersion)) {
        LOG_ERROR("%s: $version %u is newer than this build (max %u). the file is not loaded",
                  path, m_baseVersion, kGameConfigVersion);
        return;
      }
      if (!GameConfigWasMigrated(m_baseVersion)) {
        return;
      }
      if (m_baseVersion == 0u) {
        // 論点3: `$version` の書き忘れ。旧名の枝で読まれるので黙ってはいけない
        LOG_WARN("%s: no $version; read as the oldest format", path);
      }
      LOG_WARN("%s: migrated $version %u -> %u (%s)",
               path, m_baseVersion, kGameConfigVersion,
               GameConfigMigrationNote(m_baseVersion));
      LOG_WARN("%s: writing this file out will produce the v%u form",
               path, kGameConfigVersion);
    }

    std::uint32_t m_baseVersion = 0;
    bool          m_sawBase     = false;
    bool          m_sawLocal    = false;
  };

  /**
   * @brief 実効値をダンプする(F6)
   *
   * @details
   *  **ファイルに書いてある値ではなく、エンジンが実際に使っている値**が出る。
   *  欠損は既定値で埋まり (R3-4)、未知フィールドは捨てられ (R3-5)、
   *  範囲外は拒否されて既定のまま残る (R3-11)。チューニング中に見たいのは後者。
   *
   *  数十行になるので**常時は出さない**。専用キーに割り当ててある。
   *
   *  @param resource ダンプ用の一時アリーナの確保元。**config の `Document` の
   *         アリーナは使わない** — ダンプのたびにその寿命ぶん増え続けるため
   */
  inline void LogConfigDump(const GameConfig& config, Memory::IMemoryResource* resource) {
    Json::JsonArena        arena(resource);
    Json::JsonStringBuffer buffer(arena);

    if (!Json::SaveToJson(config, buffer, kGameConfigVersion, /*pretty=*/true)) {
      LOG_WARN("config dump failed (out of memory?)");
      return;
    }

    LOG_INFO("config in effect (%zu bytes):", buffer.Size());
    // 1 行ずつ出す。Logger は 1 回 1023 バイトで切れるうえ、時刻とレベルの
    // 前置きが付くのは行頭だけなので、1 本の文字列では渡せない
    Json::ForEachLine(buffer.View(), [](StringView line) {
      LOG_INFO("  %.*s", static_cast<int>(line.Size()), line.Data());
    });
  }

  /**
   * @brief 1 枚ぶんの差分を出す。**変化が無ければ 1 行も出さない**
   * @return 変化の件数
   * @note  出力されるパスは R3-9 と同じ形式で、そのまま `doc.Query()` に貼れる
   */
  [[nodiscard]] inline std::uint32_t LogConfigLayerDiff(const Json::Value& before,
                                                        const Json::Value& after,
                                                        const char* path) {
    std::uint32_t changes = 0;
    Json::DiffValues(before, after, [&changes](StringView) { ++changes; });
    if (changes == 0u) { return 0u; }

    LOG_INFO("reload: %s %u change(s)", path, changes);
    Json::DiffValues(before, after, [](StringView line) {
      LOG_INFO("%.*s", static_cast<int>(line.Size()), line.Data());
    });
    return changes;
  }

  /**
   * @brief 読み直しで**変わったところだけ**を、**ファイルごとに**出す(F5。2-3 論点5)
   *
   * @details
   *  方式 B は DOM をマージしないので「マージ後の DOM」が存在しない。
   *  ファイルごとに出せば **どちらを編集したのかが直接分かる**ので、
   *  チューニング中はむしろこちらが読みやすい。
   *  両方とも無変化のときだけ `reload: no change` を 1 行出す
   */
  inline void LogConfigDiff(const Json::Value& baseBefore, const Json::Value& baseAfter,
                            const char* basePath,
                            const Json::Value& localBefore, const Json::Value& localAfter,
                            const char* localPath) {
    std::uint32_t changes = LogConfigLayerDiff(baseBefore, baseAfter, basePath);
    changes += LogConfigLayerDiff(localBefore, localAfter, localPath);
    if (changes == 0u) { LOG_INFO("reload: no change"); }
  }

}
