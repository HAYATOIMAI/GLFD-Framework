#pragma once

namespace GLFD::Memory { class StackAllocator; class StackResource; class DoubleStackAllocator; }
namespace GLFD::Thread { class JobSystem; }
namespace GLFD::ECS { class Registry; }
namespace GLFD::Events { class EventBus; }
namespace GLFD::Graphics { class SimpleWindow; class DX11Renderer;  class Texture; }
namespace GLFD::Physics  { class SpatialHashGrid; }
namespace GLFD::Core { class InputSystem; class FileManager; }
namespace GLFD::Scene { class SceneManager; }

#include <memory>

namespace GLFD {
  struct GameContext;

  class GameEngine {
  public:
    GameEngine();
    ~GameEngine();

    GameEngine(const GameEngine&) = delete;
    GameEngine& operator=(const GameEngine&) = delete;

    void Initialize();
    void Run();

  private:
    std::unique_ptr<Memory::StackAllocator> m_mainStack = nullptr;
    std::unique_ptr<Memory::StackResource>  m_stackResource = nullptr;
    std::unique_ptr<Memory::DoubleStackAllocator> m_frameAllocator = nullptr;

    std::unique_ptr<Thread::JobSystem> m_jobSystem = nullptr;
    std::unique_ptr<ECS::Registry>     m_registry = nullptr;
    std::unique_ptr<Events::EventBus>  m_eventBus = nullptr;
    std::unique_ptr<Graphics::SimpleWindow> m_window = nullptr;
    std::unique_ptr<Graphics::DX11Renderer> m_renderer = nullptr;

    std::unique_ptr<Core::InputSystem> m_inputSystem = nullptr;
    std::unique_ptr<Graphics::Texture> m_texture = nullptr;
    std::unique_ptr<Core::FileManager> m_fileManager = nullptr;

    std::unique_ptr<Scene::SceneManager> m_sceneManager = nullptr;

    bool m_isRunning = false;
    float m_totalTime = 0.0f;

    void Update(float dt);
    void Render();
  };
}