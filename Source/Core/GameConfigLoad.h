#pragma once

/**
 * @file  GameConfigLoad.h
 * @brief 設定の読み直し(**2 段レイヤ / 成功したときだけ差し替える**)
 *
 * @details
 *  `GameConfig.h` と分けてあるのは include の重さのため。`GameConfig.h` は
 *  構造体と `Serialize` だけで完結しており、書き出ししかしない翻訳単位からも
 *  使える(README §4)。こちらは DOM と Reader を引き込む。
 *
 *  ## レイヤ設定 (2-3)
 *  ```
 *  Resource/GameConfig.jsonc        共有。手書き。日本語コメント付き。Git 管理下
 *  Resource/GameConfig.local.json   個人の上書き値だけ。Git 管理外
 *  ```
 *  **同じ構造体へ 2 回読む**(方式 B)。DOM はマージしない。R3-4 により
 *  既定値なしの `Member` はキーが無ければ値に触らないので、
 *  「`.local.json` に書いてあるフィールドだけが上書きされる」が自然に成立する。
 *  2 回目に立てる `ArchiveFlags::Overlay` が抑えるのは**既定値の代入**と
 *  **`MissingRequired` の記録**の 2 つだけである。
 *
 *  帰結として得られるもの:
 *   - **深いマージが無償**。`"window"` が `.local.json` に無ければ降りない
 *   - **配列は丸ごと置換**。コンテナの `Serialize` は配列全体を読むため
 *   - **手書きファイルに機械が触らない** = コメントが失われない(CST は恒久的にスコープ外)
 *
 *  @note 差し替えの手順をここへ切り出してあるのは、**実行経路を1本にする**ため。
 *        `BoidDemoScene::ReloadConfig` と `JsonLayeredConfigTests` / `JsonRealDataTests`
 *        はどれもこの関数を呼ぶ。写した実装をテストしても意味がない。
 */

#include <cstdint>
#include <memory>
#include <utility>

#include "GameConfig.h"
#include "Json/Json.h"

namespace GLFD {

  /// どちらのファイルの診断かを呼び出し側へ伝える (2-3 論点2)
  enum class ConfigLayer : std::uint8_t {
    Base,    ///< `GameConfig.jsonc`。共有される土台
    Local,   ///< `GameConfig.local.json`。個人の上書き。**無いのが普通**
  };

  /**
   * @brief 設定を 2 段で読み直し、**基底が成功したときだけ**差し替える
   *
   * @param baseDoc   成功時に差し替わる。`.jsonc` の DOM
   * @param localDoc  成功時に差し替わる。`.local.json` の DOM(**無くてもよい**)
   * @param config    成功時に差し替わる
   * @param basePath  共有される設定ファイル(UTF-8)
   * @param localPath 個人の上書き(UTF-8)。**存在しないのが既定の状態**
   * @param resource  3 つすべての確保元。それらより長生きすること
   * @param onIssues  `(ConfigLayer, const char* path,
   *                    const Json::Document&, const Json::ArchiveContext&)`
   *                  を取る呼び出し可能なもの。**読んだファイルごとに 1 回**呼ぶ
   *                  (`.local.json` が存在しない場合は Local の呼び出しが**無い**)。
   *                  呼ばれた時点でその `Document` は生きているので、
   *                  `ArchiveIssue::path` も `Version()` も、失敗位置の算出に要る
   *                  `Source()` / `Error()` もここで読めばよい。
   *                  **戻った後では、失敗時のパス文字列は死んでいる**
   * @return `.jsonc` が読めて差し替えが起きたとき `true`
   *
   * @warning **`baseDoc` と `localDoc` の両方を `config` と同じ寿命で保持すること。**
   *          `.local.json` が `title` などの文字列を上書きすると、`config` の
   *          `StringView` は `localDoc` のアリーナを指す (R0-5)。
   *          片方だけ捨てると文字列がダングリングする
   *
   * @note `.local.json` の不在は**エラーではない**。`FileNotFound` は正常な状態として
   *       握り、診断も出さない(出すと既定の状態で毎回警告が出ることになる)。
   *       壊れている場合は `.jsonc` の結果を保ったまま診断だけを出す。
   *       `.local.json` の `$version` は**採用しない** — 上書き断片であって
   *       独立したドキュメントではないため。不一致の警告は呼び出し側の仕事。
   *       2 段目は**基底と同じ版で読む** (A-2 論点4)。同じ構造体への 2 回の
   *       読みなので、片方だけ別の語彙で読むと「どちらの名前で書けばいいか」が
   *       ファイルごとに変わる
   *
   * @note **基底が このビルドより新しい版なら読み込まない** (A-2 論点2)。
   *       誤った値で走り続けるより、既定値で走って 1 行怒る方が説明できる
   *       (R1-1f)。`false` を返すので R3-37 のとおり差し替えは起きない
   *
   * @note `.local.json` が壊れている間は、**直前まで効いていた上書きも失われて
   *       基底の値に戻る**。部分的に前回の上書きを保つことはしない。
   *       2 段目は「基底へ読んだ直後の `loaded`」に対して走るため、
   *       `.local.json` が読めなければ上書きが 1 つも適用されない状態で
   *       差し替えが起きる。R3-37(基底が成功したときだけ差し替える)に沿った
   *       挙動であり、綴りを間違えて F5 したときに実効値が基底の値へ戻るのは
   *       正常である(実機で観測済み: `maxSpeed` が 6.5 -> 2.00 に戻る)。
   *       **保たないことが正しい。** 保つには壊れたファイルの内容を推測して
   *       部分適用することになり、どの値が生きているのかを誰も説明できなくなる
   */
  template <class OnIssues>
  [[nodiscard]] bool ReloadGameConfig(std::unique_ptr<Json::Document>& baseDoc,
                                      std::unique_ptr<Json::Document>& localDoc,
                                      std::unique_ptr<GameConfig>&     config,
                                      const char*                      basePath,
                                      const char*                      localPath,
                                      Memory::IMemoryResource*         resource,
                                      OnIssues&&                       onIssues) {
    auto freshBase  = std::make_unique<Json::Document>(resource);
    auto freshLocal = std::make_unique<Json::Document>(resource);
    auto loaded     = std::make_unique<GameConfig>(resource);

    // --- 1 段目: 共有される土台 ----------------------------------------------
    Json::ArchiveContext baseCtx(freshBase->Arena(), 0, Json::ArchiveFlags::ReportUnknown);
    const bool           baseOk = Json::LoadFromJsonFile(*loaded, basePath, *freshBase, baseCtx);

    // 診断は `freshBase` が生きているうちに渡す。捨ててから読むとパス文字列が死ぬ
    onIssues(ConfigLayer::Base, basePath,
             static_cast<const Json::Document&>(*freshBase),
             static_cast<const Json::ArchiveContext&>(baseCtx));

    if (!baseOk) {
      // ここで `fresh*` / `loaded` だけが捨てられる。呼び出し側の 3 本は無傷
      return false;
    }

    // --- 未来の版は読まない (A-2 論点2) --------------------------------------
    // Json 層ではなくここで見るのは、`ArchiveContext` が「このコードが対応できる
    // 最大の版」を知らないため。読みの外から `ctx.Report()` を呼ぶと `PathStack`
    // が空でパスの無い Issue になる。`ArchiveIssue` は**どのフィールドが**を
    // 言うためのものなので、文書全体の話はそこへ載せない。
    // 診断は上の `onIssues` で既に渡してあるので、呼び出し側が
    // `IsSupportedGameConfigVersion` を引けば理由を出せる
    if (!IsSupportedGameConfigVersion(baseCtx.Version())) {
      return false;
    }

    // --- 2 段目: 個人の上書き(無いのが普通)---------------------------------
    {
      // **基底と同じ版で読む** (A-2 論点4)。`Overlay` を立てているので
      // `ReadVersion` はここで渡した版を保つ(`.local.json` 側の `$version` は
      // 読まれない)。渡さないと `ar.Version()` が 0 になり、2 段目だけが
      // 移行前の名前で読まれる
      Json::ArchiveContext localCtx(freshLocal->Arena(), baseCtx.Version(),
                                    Json::ArchiveFlags::ReportUnknown
                                    | Json::ArchiveFlags::Overlay);
      const bool localOk =
        Json::LoadFromJsonFile(*loaded, localPath, *freshLocal, localCtx);

      // **不在は正常。** 診断も出さない(既定の状態で毎回警告が出てしまう)。
      // ファイルの有無を別途調べる API を足さずに済むよう、読んでみて
      // `FileNotFound` だったかで判定する
      const bool missing = !localOk && freshLocal->HasError()
                        && freshLocal->Error().code == Json::ErrorCode::FileNotFound;

      if (!missing) {
        // 壊れていても基底の結果は捨てない (R3-37)。診断だけ出す
        onIssues(ConfigLayer::Local, localPath,
                 static_cast<const Json::Document&>(*freshLocal),
                 static_cast<const Json::ArchiveContext&>(localCtx));
      }
    }

    baseDoc  = std::move(freshBase);
    localDoc = std::move(freshLocal);
    config   = std::move(loaded);
    return true;
  }

}
