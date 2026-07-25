#include "SceneManager.h"
#include "../Core/GameContext.h"
#include "../Core/Logger.h"

namespace GLFD::Scene {

  SceneManager::SceneManager(Memory::IMemoryResource* resource)
    : m_sceneStack(resource)
    , m_pendingTransitions(resource)
    , m_resource(resource)
  {}

  SceneManager::~SceneManager() = default;

  void SceneManager::PushScene(std::unique_ptr<IScene> scene) {
    m_pendingTransitions.EmplaceBack(
      TransitionRequest{ TransitionType::Push, std::move(scene) }
    );
  }

  void SceneManager::PopScene() {
    m_pendingTransitions.EmplaceBack(
      TransitionRequest{ TransitionType::Pop, nullptr }
    );
  }

  void SceneManager::ChangeScene(std::unique_ptr<IScene> scene) {
    m_pendingTransitions.EmplaceBack(
      TransitionRequest{ TransitionType::Change, std::move(scene) }
    );
  }

  void SceneManager::ProcessPendingTransitions(GameContext& ctx) {
    for (size_t i = 0; i < m_pendingTransitions.GetSize(); ++i) {
      auto& req = m_pendingTransitions[i];

      switch (req.type) {
        case TransitionType::Push:
          LOG_INFO("SceneManager: Push scene");
          m_sceneStack.PushBack(std::move(req.scene));
          m_sceneStack.Back()->OnEnter(ctx);
          break;

        case TransitionType::Pop:
          if (!m_sceneStack.IsEmpty()) {
            LOG_INFO("SceneManager: Pop scene");
            m_sceneStack.Back()->OnExit(ctx);
            m_sceneStack.PopBack();
          } else {
            LOG_WARN("SceneManager: Pop called on empty stack");
          }
          break;

        case TransitionType::Change:
          if (!m_sceneStack.IsEmpty()) {
            LOG_INFO("SceneManager: Change scene");
            m_sceneStack.Back()->OnExit(ctx);
            m_sceneStack.PopBack();
          }
          m_sceneStack.PushBack(std::move(req.scene));
          m_sceneStack.Back()->OnEnter(ctx);
          break;
      }
    }
    m_pendingTransitions.Clear();
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