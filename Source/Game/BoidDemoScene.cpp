#include "BoidDemoScene.h"

#include "../Core/GameContext.h"
#include "../Core/GameConfig.h"
#include "../Core/GameConfigLoad.h"
#include "../Core/GameConfigLog.h"
#include "../Core/Json/Json.h"
#include "../Core/Logger.h"

#include "../ECS/Registry.h"
#include "../ECS/Components.h"

#include "../Events/EventBus.h"
#include "../Events/Events.h"

#include "../Graphics/RenderSystem.h"
#include "../Graphics/DX11Renderer.h"

#include "../Resource/ResourceManager.h"

#include "../Physics/CollisionSystem.h"

#include "InteractionSystem.h"
#include "BoidSystems.h"
#include "BoidAgent.h"
#include "GridBulidSystem.h"
#include "SimdSystem.h"

#include <random>
#include <cmath>

namespace {
  // NUM_ENTITIES は撤去した。以前は GameConfig::ParticleCount と同じ 20000 が
  // ここにも書かれており、片方だけ変えると壊れる二重定義になっていた。
  // 現在は Resource/GameConfig.jsonc の simulation.entityCount 1 箇所である

}

namespace GLFD {

  void BoidDemoScene::ReloadConfig(GameContext& ctx) {
    // **成功したときだけ差し替える。** 手順そのものは ReloadGameConfig にあり、
    // テスト (T-41) が同じ関数を叩けるようにここには置いていない
    // 差分 (2-4) のために旧 Document を延命する。ReloadGameConfig は成功時に
    // 差し替えるだけなので、退避しておかないと旧 DOM はその場で消える。
    // 2-3 でレイヤが 2 枚になったので両方を退避する
    std::unique_ptr<Json::Document> previousBase  = std::move(m_configDoc);
    std::unique_ptr<Json::Document> previousLocal = std::move(m_configLocalDoc);
    const Json::Value* const beforeBase  = previousBase  ? &previousBase->Root()  : nullptr;
    const Json::Value* const beforeLocal = previousLocal ? &previousLocal->Root() : nullptr;

    ConfigDiagnosticsLogger diagnostics;
    const bool ok = ReloadGameConfig(m_configDoc, m_configLocalDoc, m_config,
                                     kGameConfigPath, kGameConfigLocalPath,
                                     ctx.globalResource, diagnostics);
    const std::uint32_t version = diagnostics.Version();

    if (!ok) {
      // 失敗時は元へ戻す。m_config は無傷のまま
      m_configDoc      = std::move(previousBase);
      m_configLocalDoc = std::move(previousLocal);
    }
    else if (beforeBase != nullptr && beforeLocal != nullptr) {
      // previous* はまだ生きているので、新旧の DOM を並べて比較できる
      LogConfigDiff(*beforeBase, m_configDoc->Root(), kGameConfigPath,
                    *beforeLocal, m_configLocalDoc->Root(), kGameConfigLocalPath);
    }

    if (ok) {
      // F5 で押したときに「何が入ったか」が見えるよう、その場で効く値も出す
      LOG_INFO("BoidDemoScene: config loaded (version %u, %u profiles, "
               "maxSpeed=%.2f, halfExtent=(%.1f, %.1f))",
               version, static_cast<unsigned>(m_config->boidProfiles.GetSize()),
               m_config->simulation.maxSpeed,
               m_config->world.halfExtent[0], m_config->world.halfExtent[1]);
      return;
    }

    if (m_config) {
      // **既定値へ戻さない。** 直前に正しく読めた設定をそのまま使い続ける
      LOG_WARN("BoidDemoScene: config reload failed. keeping the previous settings");
      return;
    }

    // 初回で失敗した場合だけ既定値を採用する。起動不能にはしない。
    // Document は空のまま持つ(GameConfig の StringView が既定のリテラルを
    // 指しているので中身は要らないが、以降の寿命の扱いを1本にしておく)
    m_configDoc      = std::make_unique<Json::Document>(ctx.globalResource);
    m_configLocalDoc = std::make_unique<Json::Document>(ctx.globalResource);
    m_config    = std::make_unique<GameConfig>(ctx.globalResource);
    LOG_WARN("BoidDemoScene: config could not be loaded. using built-in defaults");
  }

  void BoidDemoScene::OnEnter(GameContext& ctx) {
    LOG_INFO("BoidDemoScene: OnEnter");

    // 入室のたびに読み直す。JSON を編集してシーンを入り直せば、
    // アプリを再起動せずに反映される
    ReloadConfig(ctx);
    const GameConfig& config = *m_config;

    m_textureHandle = ctx.resourceManager->Load<Graphics::Texture>("Resource/particle.png", ctx);

    ctx.eventBus->Register<Events::CollisionEvent>();

    std::mt19937 gen(12345);
    std::uniform_real_distribution<float> posDist(config.world.spawnRange[0],
                                                  config.world.spawnRange[1]);
    std::uniform_real_distribution<float> velDist(config.world.velocityRange[0],
                                                  config.world.velocityRange[1]);

    const size_t entityCount = (config.simulation.entityCount > 0)
                             ? static_cast<size_t>(config.simulation.entityCount) : 0u;
    const size_t profileCount = config.boidProfiles.GetSize();

    for (size_t i = 0; i < entityCount; ++i) {
      // 性格を順番に配る。プロファイルが無ければ defaultBoid を全体に使う
      const BoidProfile& profile = (profileCount != 0)
                                 ? config.boidProfiles[i % profileCount]
                                 : config.defaultBoid;

      ECS::Entity e = ctx.registry->CreateEntity();
      ctx.registry->AddComponent<Components::Position>(e, posDist(gen), posDist(gen), 0.0f, 0.0f);
      ctx.registry->AddComponent<Components::Velocity>(e, velDist(gen), velDist(gen), 0.0f, 0.0f);
      ctx.registry->AddComponent<Components::Collider>(e, config.world.colliderRadius);
      ctx.registry->AddComponent<Components::BoidAgent>(
          e, profile.viewRadius, profile.separationWeight,
          profile.alignmentWeight, profile.cohesionWeight);
    }
  }

  void BoidDemoScene::OnUpdate(GameContext& ctx) {
    // F5 で設定を読み直す。チューニング中に再起動を挟まないための口。
    // **エンティティは作り直さない。** その場で変わるのは毎フレーム読む値
    // (maxSpeed / bounds / interaction)だけで、entityCount と boidProfiles、
    // spawnRange は次に OnEnter を通ったときに効く。窓の大きさは起動時のみ
    if (ctx.input->IsTriggered(Core::KeyCode::F5)) {
      LOG_INFO("BoidDemoScene: F5 pressed. reloading %s", kGameConfigPath);
      ReloadConfig(ctx);
    }

    // F6 で**実効値**をダンプする。ファイルに書いてある値ではなく、
    // 欠損を既定値で埋め、未知フィールドを捨て、範囲外を拒否した後の値が出る。
    // 数十行になるので常時は出さない(専用キーにしてある)
    if (ctx.input->IsTriggered(Core::KeyCode::F6)) {
      LogConfigDump(*m_config, ctx.globalResource);
    }

    // **ReloadConfig の後に束ねる。** 先に取ると差し替え前の設定を指したままになる
    const GameConfig& config = *m_config;

    Systems::GridBuildSystem::Update(ctx);
    Systems::InteractionSystem::Update(*ctx.registry, *ctx.jobSystem, *ctx.input, *ctx.window,
                                       config.interaction.explosionRadius,
                                       config.interaction.explosionForce,
                                       config.interaction.screenScale);
    Systems::BoidSystem::Update(ctx, config.simulation.maxSpeed);
    Systems::MovementSystem::Update(ctx);
    ApplyWorldBounds(ctx);
    Systems::CollisionSystem::Update(ctx);
  }

  void BoidDemoScene::OnRender(GameContext& ctx) {
    auto* tex = ctx.resourceManager->Get(m_textureHandle);
    if (tex) {
      ctx.renderer->SetTexture(tex);
    }
    // 設定の型に依存させないため、実際のウィンドウサイズを値で渡す
    Systems::RenderSystem::Update(*ctx.registry, *ctx.renderer, ctx.totalTime,
                                  ctx.window->GetWidth(), ctx.window->GetHeight());
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

    const float bx = m_config->world.halfExtent[0];
    const float by = m_config->world.halfExtent[1];

    for (size_t i = 0; i < count; ++i) {
      if (pData[i].x < -bx) { pData[i].x = -bx; vData[i].vx = std::abs(vData[i].vx); }
      if (pData[i].x > bx) { pData[i].x = bx; vData[i].vx = -std::abs(vData[i].vx); }
      if (pData[i].y < -by) { pData[i].y = -by; vData[i].vy = std::abs(vData[i].vy); }
      if (pData[i].y > by) { pData[i].y = by; vData[i].vy = -std::abs(vData[i].vy); }
    }
  }

}