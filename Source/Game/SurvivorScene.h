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
 *  ## 起動
 *  `GameConfig::startScene` が `"survivor"` のときだけ使われる。**実行中の切り替えは無い。**
 *
 *  @warning **このシーンは `EventBus` より先に破棄されてはならない。**
 *           `AttachSurvivor` が購読者に `m_state` への参照を渡し、`EventBus` には
 *           購読解除が無い(1-8 §1-A)。今は起動時に 1 度積まれて最後まで残るので
 *           成立している。シーン遷移を入れるときは、先に購読解除を作ること。
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
