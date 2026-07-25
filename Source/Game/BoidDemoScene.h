#pragma once

#include "../Scene/IScene.h"

namespace GLFD {

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
  };

}