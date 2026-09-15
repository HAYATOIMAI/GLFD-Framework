#include "SurvivorScene.h"

#include "../Core/DynamicArray.h"
#include "../Core/GameContext.h"
#include "../Core/Logger.h"
#include "../ECS/Components.h"
#include "../ECS/View.h"
#include "../Graphics/DX11Renderer.h"
#include "../Graphics/RenderSystem.h"
#include "../Resource/ResourceManager.h"

#include "EcsDiagnosticsLog.h"
#include "SurvivorComponents.h"
#include "SurvivorLoop.h"

#include <cstddef>
#include <cstdint>

namespace {

  // ワールド座標から NDC へ。`RenderSystem` の 1/85 : 1/64 と同じ比で、
  // 半径 30 の輪が画面に収まる大きさにした
  constexpr float kScaleX = 1.0f / 45.0f;
  constexpr float kScaleY = 1.0f / 34.0f;

  /// 経過の要約を出す間隔。**毎フレームは出さない**(10 秒に 1 行)
  constexpr std::uint64_t kSummaryEveryFrames = 600;

  /**
   * @brief 敵 / 弾 / 経験値を**色で**分けて描く (1-8 論点5)
   *
   * @details
   *  - `RenderSystem` は変更しない。**`Graphics` が `Game` の成分を知る形にしない**
   *  - シェーダーには触らない。ピクセルシェーダーが頂点色を掛けているので色だけで分かる
   *  - 種類ごとの `View` から 1 本の頂点配列に集め、`DrawPoints` は 1 回
   *
   *  @note 3 本の `View` の `BaseSize()` の合計を容量にする。**実際に出る数はこれ以下**
   *        (基準プールの範囲であって返る件数ではない。§18.7)
   */
  GLFD::Systems::RenderStatus DrawSurvivor(GLFD::ECS::Registry& registry,
                                           GLFD::Graphics::DX11Renderer& renderer,
                                           GLFD::Memory::IMemoryResource* frameResource,
                                           float time, int windowWidth, int windowHeight) {
    using namespace GLFD;

    renderer.BeginFrame();

    const float width  = static_cast<float>(windowWidth);
    float       height = static_cast<float>(windowHeight);
    if (height < 1.0f) { height = 1.0f; }
    float aspect = width / height;
    if (aspect < 0.0001f) { aspect = 1.0f; }
    renderer.UpdateGlobalConstants(aspect, time);

    auto enemies = registry.View<Components::Position, Components::Health>();
    auto bullets = registry.View<Components::Position, Components::Damage>();
    auto pickups = registry.View<Components::Position, Components::Pickup>();

    // 原点のプレイヤー(回収点)に 1 点足す
    const std::size_t capacity = enemies.BaseSize() + bullets.BaseSize() + pickups.BaseSize() + 1u;

    DynamicArray<Graphics::SimpleVertex> vertices(frameResource);
    if (!vertices.TryResize(capacity)) {
      // **黙って諦めない** (R-28)。報告はシーンが門を通して出す
      renderer.EndFrame();
      return Systems::RenderStatus{ Systems::RenderStatus::Outcome::VertexBufferUnavailable, capacity };
    }

    std::size_t count = 0;
    const auto emit = [&vertices, &count](float x, float y, float r, float g, float b) {
      vertices[count].Pos   = DirectX::XMFLOAT4(x * kScaleX, y * kScaleY, 0.0f, 1.0f);
      vertices[count].Color = DirectX::XMFLOAT4(r, g, b, 1.0f);
      ++count;
    };

    emit(0.0f, 0.0f, 1.0f, 1.0f, 1.0f);                                   // プレイヤー: 白
    for (auto [e, pos, hp] : enemies) {
      (void)e; (void)hp;
      emit(pos.x, pos.y, 1.0f, 0.25f, 0.25f);                             // 敵: 赤
    }
    for (auto [e, pos, dmg] : bullets) {
      (void)e; (void)dmg;
      emit(pos.x, pos.y, 1.0f, 0.9f, 0.2f);                               // 弾: 黄
    }
    for (auto [e, pos, pick] : pickups) {
      (void)e; (void)pick;
      emit(pos.x, pos.y, 0.3f, 1.0f, 0.4f);                               // 経験値: 緑
    }

    renderer.DrawPoints(vertices.GetData(), count);
    renderer.EndFrame();
    return Systems::RenderStatus{ Systems::RenderStatus::Outcome::Drawn, count };
  }

}

namespace GLFD {

  void SurvivorScene::OnEnter(GameContext& ctx) {
    LOG_INFO("SurvivorScene: OnEnter");

    m_textureHandle = ctx.resourceManager->Load<Graphics::Texture>("Resource/particle.png", ctx);

    m_state.params = Game::SmallSurvivorParams();
    Game::AttachSurvivor(m_state, *ctx.registry, *ctx.commands, *ctx.eventBus);

    const Game::SurvivorParams& p = m_state.params;
    LOG_INFO("SurvivorScene: small preset. %u enemies every %u frames at radius %.0f, "
             "%u bullet(s) every %u frames, caps %u / %u / %u",
             p.enemiesPerSpawn, p.spawnEveryFrames, p.spawnRadius,
             p.bulletsPerVolley, p.fireEveryFrames,
             p.maxEnemies, p.maxBullets, p.maxPickups);
  }

  void SurvivorScene::OnUpdate(GameContext& ctx) {
    // **ループの本体はここに無い。** テストとベンチが回しているのと同じ関数を呼ぶ。
    // 配信 (`DispatchEvents`) と適用 (`ApplyCommands`) もこの中で終わる
    Game::RunSurvivorFrame(m_state, ctx, m_frameReport);

    // --- ここから下は観測だけ。状態の変わり目と、段ごとの初回だけ出す (R-46) ---
    Game::ReportFrameSteps(m_frameReport, m_frameGate);
    Game::ReportAppliedCommands(ctx.commands->Report(), m_loggedFirstApply, m_commandDropGate);
    Game::ReportSurvivorCreation(m_state.thisFrame,
                                 Game::ObserveCreationFailures(m_state.thisFrame, m_creationGate),
                                 m_creationGate);
    {
      // **命中を黙って捨てない** (R-28)。キューが溢れると命中そのものが消える
      const Events::BusCounters counters = ctx.eventBus->Counters();
      Game::ReportEventQueue(counters, m_eventOverflowGate);
      ctx.eventBus->ResetCounters();
    }
    Game::ReportSurvivorFirstLap(m_state, m_lap);

    if (m_state.frame % kSummaryEveryFrames == 0u) {
      Game::ReportSurvivorSummary(m_state,
                                  ctx.registry->View<Components::Health>().BaseSize(),
                                  ctx.registry->View<Components::Damage>().BaseSize(),
                                  ctx.registry->View<Components::Pickup>().BaseSize(),
                                  ctx.registry->AliveCount());
    }
  }

  void SurvivorScene::OnRender(GameContext& ctx) {
    if (auto* tex = ctx.resourceManager->Get(m_textureHandle)) {
      ctx.renderer->SetTexture(tex);
    }
    const Systems::RenderStatus status =
        DrawSurvivor(*ctx.registry, *ctx.renderer, ctx.frameResource, ctx.totalTime,
                     ctx.window->GetWidth(), ctx.window->GetHeight());
    Game::ReportRenderStep(status, m_renderGate);
  }

  void SurvivorScene::OnExit(GameContext&) {
    // 購読者は外せない (ファイル冒頭の @warning)。今はシーンが最後まで残るので問題ない
    LOG_INFO("SurvivorScene: OnExit");
  }

}
