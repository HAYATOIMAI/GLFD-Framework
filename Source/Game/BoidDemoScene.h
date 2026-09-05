#pragma once

#include "../Scene/IScene.h"
#include "../Resource/ResourceHandle.h"
#include "../Graphics/Texture.h"

#include <memory>

namespace GLFD::Json { class Document; }

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
  };

}