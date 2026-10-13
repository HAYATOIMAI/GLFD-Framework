#pragma once

#include "../Scene/IScene.h"

#include <memory>

#include "../Core/FailureGate.h"
#include "../Graphics/ScreenMapping.h"
#include "../Events/EventChannel.h"

#include <cstdint>

namespace GLFD::Json { class Document; }
namespace GLFD::ECS  { class CommandBuffer; }

namespace GLFD {

  struct GameConfig;

  class BoidDemoScene : public Scene::IScene {
  public:
    BoidDemoScene() = default;
    ~BoidDemoScene() override = default;

    BoidDemoScene(const BoidDemoScene&) = delete;
    BoidDemoScene& operator=(const BoidDemoScene&) = delete;

    void OnEnter(GameContext& ctx) override;
    void OnUpdate(GameContext& ctx) override;
    void OnRender(GameContext& ctx) override;
    void OnExit(GameContext& ctx) override;

  private:
    void ApplyWorldBounds(GameContext& ctx);
    /// 世界と画面の対応。描画とクリックが**同じこれ**を使う (ECS 2-13)
    Graphics::ScreenMapping ScreenMappingFor(const GameContext& ctx) const;

    /**
     * @brief 設定を読み直す。**成功したときだけ差し替える**
     *
     * @details
     *  新しい Document へ読み、成功したときだけメンバへムーブする。
     *  失敗した場合は今の設定が完全に保たれる(文字列も含めて)。
     *  同じ Document へ読み直すと、パースを始めた時点で前回の文字列が
     *  死ぬため、この用途では使えない (R0-5)。
     *
     *  呼ばれるのは入室時 (OnEnter) と F5 の2箇所。F5 ではエンティティを
     *  作り直さないため、その場で効くのは毎フレーム読む値だけ。
     */
    void ReloadConfig(GameContext& ctx);


    // Document と GameConfig は同じ寿命で持つ。GameConfig の StringView が
    // Document のアリーナ上にあるため (R0-5)
    std::unique_ptr<Json::Document> m_configDoc;
    /// 2-3: 個人の上書き (.local.json)。**config と同じ寿命で持つ** (R0-5)
    std::unique_ptr<Json::Document> m_configLocalDoc;
    std::unique_ptr<GameConfig>     m_config;

    /// 1 回目の適用だけは中身が空でもログに出す。**「動いた」と「コマンドを
    /// 1 つも積まないので動いた」を区別できるようにするため** (1-4)
    bool m_loggedFirstApply = false;

    /**
     * 診断の門 (1-6)。**状態が変わったときだけ通す。**
     *
     * @note **シーンが所有する。** 状態を持つので誰かが持たねばならず、
     *       関数ローカルの `static` にすると N-3 に当たる。
     *       `RenderSystem` は `static` クラスなので自分では持てない
     */
    Core::FailureGate m_stageGate;    ///< 更新の表(どれかが Failed / Skipped)
    Core::FailureGate m_renderGate;   ///< 描画(確保失敗)
    Core::FailureGate m_eventOverflowGate;   ///< イベントキューの取りこぼし (1-7)
    Core::FailureGate m_commandDropGate;     ///< コマンドの取りこぼし (1-8 / R-46)

    /**
     * 衝突の観測点 (1-7 / R-27。2-12 で数に替えた)。**2-12 から `CollisionEvent` を購読しない**
     * (購読者が居なければ `CollisionSystem` は発行しない)。系が返す検出の数で観測する
     */
    bool              m_loggedFirstCollision = false;
    Core::FailureGate m_noContactsGate;          ///< 衝突体が居るのに 0 件(検出が壊れた)
    std::uint32_t     m_contacts     = 0;        ///< この刻みに検出した重なり
    std::size_t       m_colliders    = 0;        ///< この刻みの衝突体の数
    bool              m_collisionRan = false;    ///< この刻みに Collision の段が動いたか
  };

}