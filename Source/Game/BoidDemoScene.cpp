#include "BoidDemoScene.h"

#include "../Core/GameContext.h"
#include "../Core/GameConfig.h"
#include "../Core/SystemSchedule.h"
#include "../Core/GameConfigLoad.h"
#include "../Core/GameConfigLog.h"
#include "../Core/Json/Json.h"
#include "../Core/Logger.h"

#include "../ECS/Registry.h"
#include "../ECS/View.h"
#include "../ECS/CommandBuffer.h"
#include "../ECS/Components.h"

#include "../Events/EventBus.h"
#include "../Events/Events.h"

#include "../Graphics/RenderSystem.h"
#include "../Graphics/DX11Renderer.h"

#include "../Resource/ResourceManager.h"

#include "../Physics/CollisionSystem.h"

#include "EcsDiagnosticsLog.h"

#include "InteractionSystem.h"
#include "BoidSystems.h"
#include "BoidAgent.h"
#include "GridBulidSystem.h"
#include "SimdSystem.h"

#include <random>
#include <cmath>

// config の上限と ECS の構造的な限界の関係を、両方を include しているここで守る。
// Core を ECS へ依存させないため、GameConfig.h 側には置けない (ECS-0 1-3)
static_assert(GLFD::kMaxConfigurableEntityCount
                  <= static_cast<std::int32_t>(GLFD::ECS::MaxEntities),
              "the configurable entity cap must stay well below ECS::MaxEntities. "
              "MaxEntities is the structural limit of the sparse array; if the "
              "config cap is allowed to reach it, safety starts depending on the "
              "boundary being off-by-one correct. The cap is half of MaxEntities "
              "(1-2), which keeps a value read from the config away from the "
              "boundary without wasting the range. "
              "If you lower MaxEntities, lower kMaxConfigurableEntityCount with it.");

namespace {
  // NUM_ENTITIES は撤去した。以前は GameConfig::ParticleCount と同じ 20000 が
  // ここにも書かれており、片方だけ変えると壊れる二重定義になっていた。
  // 現在は Resource/GameConfig.jsonc の simulation.entityCount 1 箇所である

  // 名前付けと整形は 1-6 で Game/EcsDiagnosticsLog.h へ移した。
  // 診断を足したら整形が 5 箇所に散り、OnUpdate と OnRender に割れたため
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

    // **観測点** (1-7 / R-27)。ECS-0 で衝突が 1 件も動いていないことに誰も
    // 気づかなかったのは、**購読者が 0 件だった**ためである。
    // ここは数えるだけで、破棄は積まない(1-8 の仕事)
    ctx.eventBus->Subscribe<Events::CollisionEvent>(
        [this](const Events::CollisionEvent&) { ++m_collisionsDelivered; });

    std::mt19937 gen(12345);
    std::uniform_real_distribution<float> posDist(config.world.spawnRange[0],
                                                  config.world.spawnRange[1]);
    std::uniform_real_distribution<float> velDist(config.world.velocityRange[0],
                                                  config.world.velocityRange[1]);

    const size_t entityCount = (config.simulation.entityCount > 0)
                             ? static_cast<size_t>(config.simulation.entityCount) : 0u;
    const size_t profileCount = config.boidProfiles.GetSize();

    size_t created = 0;
    for (size_t i = 0; i < entityCount; ++i) {
      // 性格を順番に配る。プロファイルが無ければ defaultBoid を全体に使う
      const BoidProfile& profile = (profileCount != 0)
                                 ? config.boidProfiles[i % profileCount]
                                 : config.defaultBoid;

      const ECS::Entity e = ctx.registry->CreateEntity();
      if (!e.IsValid()) {
        // **黙って少ない数で続けない** (2-4 の方針)。config に書いた数と実際に
        // 作れた数が食い違ったまま進むと、後から原因を追えなくなる。
        // OnEnter は入室のたびに走り、破棄の経路が無いので ID は積み上がる
        LOG_ERROR("BoidDemoScene: entity limit reached. created %zu of %zu requested "
                  "(ECS::MaxEntities = %zu)", created, entityCount, ECS::MaxEntities);
        break;
      }

      // **AddComponent は T* を返す** (R-25)。確保に失敗すると nullptr になる。
      // 途中で失敗したら**中途半端なエンティティを残さず**破棄して打ち切る
      const bool built =
             ctx.registry->AddComponent<Components::Position>(
                 e, posDist(gen), posDist(gen), 0.0f, 0.0f) != nullptr
          && ctx.registry->AddComponent<Components::Velocity>(
                 e, velDist(gen), velDist(gen), 0.0f, 0.0f) != nullptr
          && ctx.registry->AddComponent<Components::Collider>(
                 e, config.world.colliderRadius) != nullptr
          && ctx.registry->AddComponent<Components::BoidAgent>(
                 e, profile.viewRadius, profile.separationWeight,
                 profile.alignmentWeight, profile.cohesionWeight) != nullptr;

      if (!built) {
        ctx.registry->DestroyEntity(e);
        LOG_ERROR("BoidDemoScene: could not build entity %zu of %zu "
                  "(component allocation failed). created %zu",
                  i, entityCount, created);
        break;
      }
      ++created;
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

    // **前フレームの衝突を報告する** (1-7)。発行は `OnUpdate` の中、配信は
    // `DispatchAll`(このシーンの外)なので、揃って読めるのは次のフレームの頭になる
    {
      const Events::BusCounters counters = ctx.eventBus->Counters();
      Game::ReportCollisionObservation(counters.published, counters.dropped,
                                       m_collisionsDelivered, m_loggedFirstCollision,
                                       m_eventOverflowGate);
      ctx.eventBus->ResetCounters();
      m_collisionsDelivered = 0;
    }

    // =======================================================================
    // **実行順序はここ 1 箇所にある** (1-6 / R-26)
    // =======================================================================
    //
    //  以前はこの下に 6 行が並んでいるだけで、依存は `GameContext` のメンバ
    //  経由の暗黙のものだった(宣言も検査も無い)。表にすると:
    //
    //   - 順序が 1 箇所に見える
    //   - **前提条件が同じ場所に見える**(「Boid はグリッドが要る」)
    //   - シグネチャが揃っていなくても並べられる。GLFD のシステムは
    //     `Update(ctx)` / `Update(ctx, maxSpeed)` / 引数 7 個 /
    //     シーンのメンバ関数、と 4 種類に割れている
    //
    //  @note **描画 (`RenderSystem`) はこの表に入らない。** `OnRender` にいて
    //        位相が違う。更新の順序表に描画を混ぜると嘘になる。
    //
    //  @note **依存を宣言して順序を自動で決めるところまでは行かない**(過剰)。
    //        順序は人が書き、前提条件が同じ場所に見えていればよい。
    //
    //  @note config は**シーンの `m_config` から取る**。`GameContext` へ入れない。
    //        `GameConfig` の実体は 2 つあり (`GameEngine::m_config` と
    //        `BoidDemoScene::m_config`)、`ctx` に入るのはエンジン側なので、
    //        **F5 で読み直しても効かない値**が生まれる。
    static constexpr Core::SystemStep<BoidDemoScene, GameContext> kUpdateOrder[] = {
      { "GridBuild", [](BoidDemoScene&, GameContext& c) {
          Systems::GridBuildSystem::Update(c);
          // 作れたかどうかは `ctx.grid` に出る (1-6 の確保失敗の扱い)
          return (c.grid != nullptr) ? Core::StepResult::Ran : Core::StepResult::Failed;
        } },

      { "Interaction", [](BoidDemoScene& self, GameContext& c) {
          const InteractionConfig& in = self.m_config->interaction;
          Systems::InteractionSystem::Update(*c.registry, *c.jobSystem, *c.input, *c.window,
                                             in.explosionRadius, in.explosionForce,
                                             in.screenScale);
          return Core::StepResult::Ran;
        } },

      { "Boid", [](BoidDemoScene& self, GameContext& c) {
          if (c.grid == nullptr) { return Core::StepResult::Skipped; }   // 近傍探索に要る
          Systems::BoidSystem::Update(c, self.m_config->simulation.maxSpeed);
          return Core::StepResult::Ran;
        } },

      { "Movement", [](BoidDemoScene&, GameContext& c) {
          Systems::MovementSystem::Update(c);
          return Core::StepResult::Ran;
        } },

      { "WorldBounds", [](BoidDemoScene& self, GameContext& c) {
          self.ApplyWorldBounds(c);
          return Core::StepResult::Ran;
        } },

      { "Collision", [](BoidDemoScene&, GameContext& c) {
          if (c.grid == nullptr) { return Core::StepResult::Skipped; }   // 近傍探索に要る
          Systems::CollisionSystem::Update(c);
          return Core::StepResult::Ran;
        } },
    };

    Core::FrameReport frameReport;
    Core::RunSteps(kUpdateOrder, *this, ctx, frameReport);

    // **構造変更をここで適用する** (1-4)。反復中に積まれたものがフレーム境界で
    // まとめて効く。CollisionSystem の後に置いたのは、破棄を積む最有力の候補が
    // 衝突だからで、同じフレームのうちに適用しないと 2 フレーム遅れる。
    //
    // **DispatchAll() との前後は決めきれていない。** 購読者が「衝突したら敵を
    // 破棄する」を積むなら、この位置(DispatchAll の前)では 1 フレーム遅れる。
    // ただし CollisionEvent の購読者は現在 0 件で、**どちらに置いても観測できる
    // 差が無い**。1-7 で購読者が現れた時点で、DispatchAll の後へ移すか
    // 2 回目の適用を足すかを決めること。観測できない仮定で選んだふりをしない
    ctx.registry->ApplyCommands(*ctx.commands);

    // **出力は診断層が行う** (1-6)。ここは呼ぶだけ
    Game::ReportAppliedCommands(ctx.commands->Report(), m_loggedFirstApply);
    Game::ReportFrameSteps(frameReport, m_frameGate);
  }

  void BoidDemoScene::OnRender(GameContext& ctx) {
    auto* tex = ctx.resourceManager->Get(m_textureHandle);
    if (tex) {
      ctx.renderer->SetTexture(tex);
    }
    // 設定の型に依存させないため、実際のウィンドウサイズを値で渡す。
    // **戻り値で受ける** (1-6)。1-1 からの借りの返済で、それまでは確保に
    // 失敗しても黙って 1 フレーム描かずに戻っていた
    const Systems::RenderStatus status =
        Systems::RenderSystem::Update(*ctx.registry, *ctx.renderer, ctx.frameResource,
                                      ctx.totalTime,
                                      ctx.window->GetWidth(), ctx.window->GetHeight());
    Game::ReportRenderStep(status, m_renderGate);
  }

  void BoidDemoScene::OnExit(GameContext& ctx) {
    LOG_INFO("BoidDemoScene: OnExit");
  }

  void BoidDemoScene::ApplyWorldBounds(GameContext& ctx) {
    const float bx = m_config->world.halfExtent[0];
    const float by = m_config->world.halfExtent[1];

    // 位置と速度の**組**を回す (1-5 / R-32)
    for (auto [entity, pos, vel] : ctx.registry->View<Components::Position,
                                                      Components::Velocity>()) {
      (void)entity;
      if (pos.x < -bx) { pos.x = -bx; vel.vx = std::abs(vel.vx); }
      if (pos.x > bx) { pos.x = bx; vel.vx = -std::abs(vel.vx); }
      if (pos.y < -by) { pos.y = -by; vel.vy = std::abs(vel.vy); }
      if (pos.y > by) { pos.y = by; vel.vy = -std::abs(vel.vy); }
    }
  }

}