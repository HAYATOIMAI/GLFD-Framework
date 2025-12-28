#pragma once

namespace GLFD::Memory { class StackAllocator; class StackResource; class DoubleStackAllocator; }
namespace GLFD::Thread { class JobSystem; }
namespace GLFD::ECS { class Registry; }
namespace GLFD::Events { class EventBus; }
namespace GLFD::Graphics { class SimpleWindow; class DX11Renderer;  class Texture; }
namespace GLFD::Physics  { class SpatialHashGrid; }
namespace GLFD::Core { class InputSystem; class FileManager; }

#include <memory>
#include <vector>

namespace GLFD {
  struct GameContext;

  class GameEngine {
  public:
    GameEngine();
    ~GameEngine();

    // コピー禁止
    GameEngine(const GameEngine&) = delete;
    GameEngine& operator=(const GameEngine&) = delete;

    // 初期化 (ウィンドウ作成、メモリ確保など)
    void Initialize();

    // ゲームループの開始
    void Run();

  private:
    // メモリ管理
    std::unique_ptr<Memory::StackAllocator> m_mainStack = nullptr;
    std::unique_ptr<Memory::StackResource>  m_stackResource = nullptr;
    std::unique_ptr<Memory::DoubleStackAllocator> m_frameAllocator = nullptr;

    // コアシステム
    std::unique_ptr<Thread::JobSystem> m_jobSystem = nullptr;
    std::unique_ptr<ECS::Registry>     m_registry = nullptr;
    std::unique_ptr<Events::EventBus>  m_eventBus = nullptr;
    std::unique_ptr<Graphics::SimpleWindow> m_window = nullptr;
    std::unique_ptr<Graphics::DX11Renderer> m_renderer = nullptr;

    std::unique_ptr<Core::InputSystem> m_inputSystem = nullptr;
    std::unique_ptr<Graphics::Texture> m_texture = nullptr;
    std::unique_ptr<Core::FileManager> m_fileManager = nullptr;

    bool m_isRunning = false;
    float m_totalTime = 0.0f;

    // 内部更新処理
    void Update(float dt);
    void Render();

    // デモ用の初期化 (エンティティ生成など)
    void InitDemoScene();

    void ApplyWorldBounds(GameContext& context);
  };
}