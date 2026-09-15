#pragma once

#include "../Scene/IScene.h"
#include "../Resource/ResourceHandle.h"
#include "../Graphics/Texture.h"

#include <memory>

#include "../Core/FailureGate.h"

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


    Resource::ResourceHandle<Graphics::Texture> m_textureHandle;

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
    Core::FailureGate m_frameGate;    ///< 更新の表(どれかが Failed / Skipped)
    Core::FailureGate m_renderGate;   ///< 描画(確保失敗)
    Core::FailureGate m_eventOverflowGate;   ///< イベントキューの取りこぼし (1-7)
    Core::FailureGate m_commandDropGate;     ///< コマンドの取りこぼし (1-8 / R-46)

    /**
     * 衝突の観測点 (1-7 / R-27)。**購読者が数えるだけ**で、
     * `CommandBuffer` には何も積まない(破棄を積むのは 1-8)。
     *
     * @note 配信は `DispatchAll`(`OnUpdate` の後)なので、**ここに溜まるのは
     *       1 フレーム前のぶん**である
     */
    std::uint32_t m_collisionsDelivered = 0;
    bool          m_loggedFirstCollision = false;
  };

}