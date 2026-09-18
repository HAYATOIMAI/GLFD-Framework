#pragma once

/**
 * @file  SurvivorScene.h
 * @brief ECS 1-8: 敵 / 弾 / 経験値の最小ループを動かすシーン
 *
 * @details
 *  **ループの本体は `SurvivorLoop.h` にあり、ここは呼ぶだけ。** テストとベンチが回して
 *  いるのと同じ `RunSurvivorFrame` を呼ぶ。シーンが持つのは、シーンにしかできないこと
 *  (テクスチャ、描画、ログ)だけである。
 *
 *  ## 起動と切り替え
 *  `GameConfig::startScene` が `"survivor"` のとき起動シーンになり、実行中は
 *  `2` キーで入れる (`SceneCatalog.h`)。
 *
 *  ## `OnExit` の契約 (2-1)
 *  `DetachSurvivor` で購読を外し、`DestroyAll()` でエンティティを消す。
 *  **1-8 ではどちらも無く、抜けた時点で use-after-free だった** (§1-A)。
 *  守れているかは `SceneManager` が `TransitionReport` で照合する。
 */

#include "../Core/FailureGate.h"
#include "../Core/SystemSchedule.h"
#include "../Graphics/Texture.h"
#include "../Resource/ResourceHandle.h"
#include "../Scene/IScene.h"
#include "EcsDiagnosticsLog.h"
#include "SurvivorLoop.h"

namespace GLFD {

  class SurvivorScene : public Scene::IScene {
  public:
    SurvivorScene() = default;
    ~SurvivorScene() override = default;

    SurvivorScene(const SurvivorScene&) = delete;
    SurvivorScene& operator=(const SurvivorScene&) = delete;

    void OnEnter(GameContext& ctx) override;
    void OnUpdate(GameContext& ctx) override;
    void OnRender(GameContext& ctx) override;
    void OnExit(GameContext& ctx) override;

  private:
    Resource::ResourceHandle<Graphics::Texture> m_textureHandle;

    /// ループの状態。**購読者が参照で掴んでいる**(ファイル冒頭の @warning)
    Game::SurvivorState m_state;
    Core::FrameReport   m_frameReport;

    // 診断の門 (R-46)。状態を持つので**シーンが所有する**(N-3)
    Core::FailureGate m_frameGate;           ///< 順序表のどれかが Failed / Skipped
    Core::FailureGate m_renderGate;          ///< 描画の確保失敗
    Core::FailureGate m_commandDropGate;     ///< コマンドの取りこぼし
    Core::FailureGate m_creationGate;        ///< 作ろうとして作れなかった
    Core::FailureGate m_eventOverflowGate;   ///< イベントキューの取りこぼし

    /// 最初の適用だけは空でも出す (1-4)
    bool m_loggedFirstApply = false;
    /// 各段が初めて起きたフレームを 1 回ずつ出す
    Game::SurvivorLapLog m_lap;
  };

}
