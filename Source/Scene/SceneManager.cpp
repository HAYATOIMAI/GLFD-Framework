#include "SceneManager.h"
#include "../Core/GameContext.h"

#include <utility>

namespace GLFD::Scene {

  SceneManager::SceneManager(Memory::IMemoryResource* resource)
    : m_sceneStack(resource)
    , m_pendingTransitions(resource)
    , m_processing(resource)
    , m_resource(resource)
  {}

  SceneManager::~SceneManager() = default;

  bool SceneManager::PushScene(std::unique_ptr<IScene> scene) {
    return m_pendingTransitions.TryPushBack(
      TransitionRequest{ TransitionType::Push, std::move(scene) }
    );
  }

  bool SceneManager::PopScene() {
    return m_pendingTransitions.TryPushBack(
      TransitionRequest{ TransitionType::Pop, nullptr }
    );
  }

  bool SceneManager::ChangeScene(std::unique_ptr<IScene> scene) {
    return m_pendingTransitions.TryPushBack(
      TransitionRequest{ TransitionType::Change, std::move(scene) }
    );
  }

  /**
   * @brief `OnExit` を呼び、契約が守られたかを照合する (2-1 / 論点5)
   *
   * @details
   *  **購読は安全網まで持つ。** 外し忘れを報告だけにすると、次の配信が
   *  解放済みのシーンを呼ぶ(use-after-free)。`OnEnter` の直前に控えた
   *  通し番号以降に残っている購読が、そのシーンの足した分である。
   *  持ち主を記録しなくてよく、シーンを積んでも成り立つ。
   *
   *  **エンティティは一番下のシーンだけ掃く。** 上に積んだシーンでは
   *  どれが自分のものか区別できないので、報告に留める (`IScene.h` の @note)。
   */
  void SceneManager::LeaveTop(GameContext& ctx) {
    m_sceneStack.Back()->OnExit(ctx);

    if (ctx.eventBus != nullptr) {
      const std::size_t leaked = ctx.eventBus->SubscriberCountSince(m_serialAtEnter);
      if (leaked != 0) {
        m_report.leakedSubscriptions += static_cast<std::uint32_t>(leaked);
        (void)ctx.eventBus->UnsubscribeSince(m_serialAtEnter);
      }
    }

    if (ctx.registry != nullptr) {
      const std::uint32_t alive = ctx.registry->AliveCount();
      if (alive != m_aliveAtEnter) {
        m_report.leakedEntities += (alive > m_aliveAtEnter) ? (alive - m_aliveAtEnter)
                                                            : (m_aliveAtEnter - alive);
      }
      // 一番下のシーン(抜けると誰も残らない)だけは掃いてよい
      if (m_sceneStack.GetSize() == 1 && alive != 0) {
        m_report.sweptEntities += ctx.registry->DestroyAll();
      }
    }

    // **積み残したコマンドは次のシーンへ渡さない。** 渡すと、死んだ
    // エンティティ宛てとして取りこぼしに数えられ、**前のシーンの原因が
    // 次のシーンの ERROR として出る** (§6.7)
    if (ctx.commands != nullptr && !ctx.commands->IsEmpty()) {
      m_report.discardedCommands += static_cast<std::uint32_t>(ctx.commands->Size());
      ctx.commands->Clear();
    }

    m_sceneStack.PopBack();
  }

  void SceneManager::ApplyPush(GameContext& ctx, std::unique_ptr<IScene> scene,
                               bool replaceTop) {
    // **確保を先に済ませる。** `OnExit` を呼んだ後に失敗すると、シーンが
    // 1 つも無い状態が残る
    const std::size_t needed = replaceTop ? m_sceneStack.GetSize()
                                          : m_sceneStack.GetSize() + 1u;
    if (!m_sceneStack.TryReserve(needed == 0 ? 1u : needed)) {
      ++m_report.refused;
      return;                       // 元のシーンをそのまま動かし続ける
    }

    if (replaceTop && !m_sceneStack.IsEmpty()) {
      LeaveTop(ctx);
    }

    if (!m_sceneStack.TryPushBack(std::move(scene))) {
      ++m_report.refused;           // 上で確保したので、ここは通らないはず
      return;
    }

    if (ctx.eventBus != nullptr) { m_serialAtEnter = ctx.eventBus->NextSerial(); }
    if (ctx.registry != nullptr) { m_aliveAtEnter  = ctx.registry->AliveCount(); }

    ++m_report.transitions;
    m_sceneStack.Back()->OnEnter(ctx);
  }

  void SceneManager::ApplyPop(GameContext& ctx) {
    if (m_sceneStack.IsEmpty()) {
      ++m_report.refused;
      return;
    }
    LeaveTop(ctx);
    ++m_report.transitions;

    // 下のシーンへ戻る。控えは取り直せないので、**下のシーンの控えは
    // その時点の値にする**(そこから増えた分だけが下のシーンの責任になる)
    if (!m_sceneStack.IsEmpty()) {
      if (ctx.eventBus != nullptr) { m_serialAtEnter = ctx.eventBus->NextSerial(); }
      if (ctx.registry != nullptr) { m_aliveAtEnter  = ctx.registry->AliveCount(); }
    }
  }

  void SceneManager::ProcessPendingTransitions(GameContext& ctx) {
    if (m_pendingTransitions.IsEmpty()) { return; }

    // **入れ替えてから処理する。** なぞりながら `OnEnter` が要求を足すと
    // 再確保が起きて参照が宙に浮く (ヘッダの @note)
    m_processing.Swap(m_pendingTransitions);

    for (std::size_t i = 0; i < m_processing.GetSize(); ++i) {
      TransitionRequest& req = m_processing[i];

      switch (req.type) {
        case TransitionType::Push:
          ApplyPush(ctx, std::move(req.scene), false);
          break;

        case TransitionType::Pop:
          ApplyPop(ctx);
          break;

        case TransitionType::Change:
          ApplyPush(ctx, std::move(req.scene), true);
          break;
      }
    }
    m_processing.Clear();
  }

  void SceneManager::Update(GameContext& ctx) {
    if (!m_sceneStack.IsEmpty()) {
      m_sceneStack.Back()->OnUpdate(ctx);
    }
  }

  void SceneManager::Render(GameContext& ctx) {
    if (!m_sceneStack.IsEmpty()) {
      m_sceneStack.Back()->OnRender(ctx);
    }
  }

  bool SceneManager::HasActiveScene() const {
    return !m_sceneStack.IsEmpty();
  }

}
