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
#include "SurvivorRender.h"

#include <cstddef>
#include <cstdint>

namespace {

  /// 経過の要約を出す間隔。**毎フレームは出さない**(10 秒に 1 行)
  constexpr std::uint64_t kSummaryEveryFrames = 600;

  /**
   * @brief 敵 / 弾 / 経験値を**色で**分けて描く (1-8 論点5)
   *
   * @details
   *  - `RenderSystem` は変更しない。**`Graphics` が `Game` の成分を知る形にしない**
   *  - シェーダーには触らない。ピクセルシェーダーが頂点色を掛けているので色だけで分かる
   *  - 種類ごとの `View` から 1 本の頂点配列に集め、`DrawPoints` は 1 回
   *  - **頂点を組む部分は `Game::BuildSurvivorVertices` (`SurvivorRender.h`) に切り出した** (2-3)。
   *    ここに残るのは DX11 の呼び出しだけ
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

    // 頂点を組むのは `Game/SurvivorRender.h`。**テストとベンチも同じ関数を呼ぶ** (2-3)
    DynamicArray<Graphics::SimpleVertex> vertices(frameResource);
    const Systems::RenderStatus status = Game::BuildSurvivorVertices(registry, vertices);
    if (status.outcome != Systems::RenderStatus::Outcome::Drawn) {
      // **黙って諦めない** (R-28)。報告はシーンが門を通して出す
      renderer.EndFrame();
      return status;
    }

    renderer.DrawPoints(vertices.GetData(), vertices.GetSize());
    renderer.EndFrame();
    return status;
  }

}

namespace GLFD {

  void SurvivorScene::OnEnter(GameContext& ctx) {
    LOG_INFO("SurvivorScene: OnEnter");

    m_textureHandle = ctx.resourceManager->Load<Graphics::Texture>("Resource/particle.png", ctx);

    m_state = Game::SurvivorState{};          // 入り直しても前回の数を持ち越さない
    m_state.params = Game::SmallSurvivorParams();
    if (!Game::AttachSurvivor(m_state, *ctx.registry, *ctx.commands, *ctx.eventBus)) {
      // **黙って進まない。** 購読できていなければ命中が誰にも届かず、
      // 「動いているのに何も起きない」状態になる (1-7 / R-27)
      LOG_ERROR("SurvivorScene: could not subscribe to HitEvent. hits will not resolve");
    }

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

  void SurvivorScene::OnExit(GameContext& ctx) {
    // `IScene.h` の契約。**購読を外してからエンティティを消す**
    const bool removed   = Game::DetachSurvivor(m_state, *ctx.eventBus);
    const std::uint32_t destroyed = ctx.registry->DestroyAll();
    LOG_INFO("SurvivorScene: OnExit. subscription %s, %u entit%s destroyed",
             removed ? "removed" : "was already gone",
             destroyed, (destroyed == 1u) ? "y" : "ies");
  }

}
