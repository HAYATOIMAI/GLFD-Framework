#include "BoidDemoScene.h"

#include "../Core/GameContext.h"
#include "../Core/GameConfig.h"
#include "../Core/Logger.h"

#include "../ECS/Registry.h"
#include "../ECS/Components.h"

#include "../Events/EventBus.h"
#include "../Events/Events.h"

#include "../Graphics/RenderSystem.h"
#include "../Graphics/DX11Renderer.h"
#include "../Graphics/Texture.h"

#include "../Physics/CollisionSystem.h"

#include "InteractionSystem.h"
#include "BoidSystems.h"
#include "BoidAgent.h"
#include "GridBulidSystem.h"
#include "SimdSystem.h"

#include <random>
#include <cmath>

namespace {
  constexpr auto NUM_ENTITIES = 20000;
}

namespace GLFD {

  void BoidDemoScene::OnEnter(GameContext& ctx) {
    LOG_INFO("BoidDemoScene: OnEnter");

    ctx.eventBus->Register<Events::CollisionEvent>();

    std::mt19937 gen(12345);
    std::uniform_real_distribution<float> posDist(-20.0f, 20.0f);
    std::uniform_real_distribution<float> velDist(-0.5f, 0.5f);

    for (size_t i = 0; i < NUM_ENTITIES; ++i) {
      ECS::Entity e = ctx.registry->CreateEntity();
      ctx.registry->AddComponent<Components::Position>(e, posDist(gen), posDist(gen), 0.0f, 0.0f);
      ctx.registry->AddComponent<Components::Velocity>(e, velDist(gen), velDist(gen), 0.0f, 0.0f);
      ctx.registry->AddComponent<Components::Collider>(e, 0.3f);
      ctx.registry->AddComponent<Components::BoidAgent>(e);
    }
  }

  void BoidDemoScene::OnUpdate(GameContext& ctx) {
    Systems::GridBuildSystem::Update(ctx);
    Systems::InteractionSystem::Update(*ctx.registry, *ctx.jobSystem, *ctx.input, *ctx.window);
    Systems::BoidSystem::Update(ctx);
    Systems::MovementSystem::Update(ctx);
    ApplyWorldBounds(ctx);
    Systems::CollisionSystem::Update(ctx);
  }

  void BoidDemoScene::OnRender(GameContext& ctx) {
    ctx.renderer->SetTexture(ctx.texture);
    Systems::RenderSystem::Update(*ctx.registry, *ctx.renderer, ctx.totalTime);
  }

  void BoidDemoScene::OnExit(GameContext& ctx) {
    LOG_INFO("BoidDemoScene: OnExit");
  }

  void BoidDemoScene::ApplyWorldBounds(GameContext& ctx) {
    auto& positions = ctx.registry->View<Components::Position>();
    auto& velocities = ctx.registry->View<Components::Velocity>();

    size_t count = positions.GetSize();
    auto* pData = positions.GetData();
    auto* vData = velocities.GetData();

    const float bx = GameConfig::WorldBoundsX;
    const float by = GameConfig::WorldBoundsY;

    for (size_t i = 0; i < count; ++i) {
      if (pData[i].x < -bx) { pData[i].x = -bx; vData[i].vx = std::abs(vData[i].vx); }
      if (pData[i].x > bx) { pData[i].x = bx; vData[i].vx = -std::abs(vData[i].vx); }
      if (pData[i].y < -by) { pData[i].y = -by; vData[i].vy = std::abs(vData[i].vy); }
      if (pData[i].y > by) { pData[i].y = by; vData[i].vy = -std::abs(vData[i].vy); }
    }
  }

}