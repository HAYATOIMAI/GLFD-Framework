#pragma once

#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"
#include "IScene.h"
#include <memory>

namespace GLFD {
  struct GameContext;
}

namespace GLFD::Scene {

  class SceneManager {
  public:
    explicit SceneManager(Memory::IMemoryResource* resource);
    ~SceneManager();

    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;
    SceneManager(SceneManager&&) = delete;
    SceneManager& operator=(SceneManager&&) = delete;

    void PushScene(std::unique_ptr<IScene> scene);
    void PopScene();
    void ChangeScene(std::unique_ptr<IScene> scene);

    void ProcessPendingTransitions(GameContext& ctx);

    void Update(GameContext& ctx);
    void Render(GameContext& ctx);

    [[nodiscard]] bool HasActiveScene() const;

  private:
    enum class TransitionType { Push, Pop, Change };

    struct TransitionRequest {
      TransitionType type;
      std::unique_ptr<IScene> scene;
    };

    DynamicArray<std::unique_ptr<IScene>> m_sceneStack;
    DynamicArray<TransitionRequest>       m_pendingTransitions;
    Memory::IMemoryResource*              m_resource;
  };

}