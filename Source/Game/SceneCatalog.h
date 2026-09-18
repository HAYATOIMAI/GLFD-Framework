#pragma once

/**
 * @file  SceneCatalog.h
 * @brief シーンの名前と切り替えキーの表 (ECS 2-1 / 論点4)
 *
 * @details
 *  **表は 1 つだけにする** (§7.3: 規約を変えたら、それが書かれている場所を
 *  全部変える)。起動時の選択 (`GameConfig::startScene`) と実行中のキー切り替えが
 *  同じ表を引く。1-8 では `Engine.cpp` の if 文に名前が直書きされており、
 *  キーを足すと名前が 2 か所になるところだった。
 *
 *  ## `$version` は上げない
 *  `startScene` は 1-8 で既に入っている。キーの割り当てはコード側の話なので
 *  config には出さない。**無い設定を読んでも初期値が残る** (R3-4)。
 *
 *  ## 名前が知らないものだったら
 *  **起動不可能にはしない。** `MakeScene` が nullptr を返し、呼び出し側が
 *  既定のシーンへ倒して警告を残す(1-8 と同じ方針)。
 */

#include "../Core/StringView.h"
#include "../Scene/IScene.h"
#include "BoidDemoScene.h"
#include "SurvivorScene.h"

#include <cstddef>
#include <memory>

namespace GLFD::Game {

  /// 表の 1 行。`key` は `InputSystem::IsTriggered` にそのまま渡せる値
  struct SceneEntry {
    const char* name;
    int         key;
    std::unique_ptr<Scene::IScene> (*make)();
  };

  namespace Detail {
    inline std::unique_ptr<Scene::IScene> MakeBoid()     { return std::make_unique<BoidDemoScene>(); }
    inline std::unique_ptr<Scene::IScene> MakeSurvivor() { return std::make_unique<SurvivorScene>(); }
  }

  /**
   * @brief 使えるシーンの一覧
   * @note  **既定は先頭**である。`kDefaultScene` を別に持つと 2 か所になる
   */
  inline constexpr SceneEntry kSceneCatalog[] = {
    { "boids",    '1', &Detail::MakeBoid     },
    { "survivor", '2', &Detail::MakeSurvivor },
  };

  inline constexpr std::size_t kSceneCount = sizeof(kSceneCatalog) / sizeof(kSceneCatalog[0]);

  static_assert(kSceneCount >= 1,
                "the catalog must have at least one row: MakeDefaultScene() takes the first.");

  /// 名前で引く。**知らない名前なら nullptr**(呼び出し側が倒す)
  [[nodiscard]] inline std::unique_ptr<Scene::IScene> MakeScene(StringView name) {
    for (std::size_t i = 0; i < kSceneCount; ++i) {
      if (name == StringView(kSceneCatalog[i].name)) {
        return kSceneCatalog[i].make();
      }
    }
    return nullptr;
  }

  [[nodiscard]] inline std::unique_ptr<Scene::IScene> MakeDefaultScene() {
    return kSceneCatalog[0].make();
  }

  [[nodiscard]] inline const char* DefaultSceneName() noexcept { return kSceneCatalog[0].name; }

}
