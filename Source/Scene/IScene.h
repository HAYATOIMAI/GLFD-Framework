#pragma once

namespace GLFD { struct GameContext; }

namespace GLFD::Scene {
  class IScene {
  public:
    virtual ~IScene() = default;

    virtual void OnEnter(GameContext& ctx) = 0;
    virtual void OnUpdate(GameContext& ctx) = 0;
    virtual void OnRender(GameContext& ctx) = 0;
    virtual void OnExit(GameContext& ctx) = 0;
  };
}