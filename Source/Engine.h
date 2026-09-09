#pragma once

namespace GLFD::Memory { class StackAllocator; class StackResource; class DoubleStackAllocator; }
namespace GLFD::Thread { class JobSystem; }
namespace GLFD::ECS { class Registry; class CommandBuffer; }
namespace GLFD::Events { class EventBus; }
namespace GLFD::Graphics { class SimpleWindow; class DX11Renderer; }
namespace GLFD::Resource { class ResourceManager; }
namespace GLFD::Physics  { class SpatialHashGrid; }
namespace GLFD::Core { class InputSystem; class FileManager; }
namespace GLFD::Scene { class SceneManager; }
// 設定は JSON から読む。ヘッダに Document を持ち込まない
namespace GLFD::Json { class Document; }

#include <memory>

namespace GLFD {
  struct GameContext;
  struct GameConfig;

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
    // 1-4: 構造変更のバッファ。**フレームアロケータからは取らない** ―
    // StackResource::Deallocate は no-op なので、DynamicArray が 1.5 倍で
    // 伸びるたびに旧領域がフレーム内で死蔵される。寿命の長い側から取り、
    // Clear() で容量を保って使い回す
    std::unique_ptr<ECS::CommandBuffer> m_commands = nullptr;
    std::unique_ptr<Events::EventBus>  m_eventBus = nullptr;
    std::unique_ptr<Graphics::SimpleWindow> m_window = nullptr;
    std::unique_ptr<Graphics::DX11Renderer> m_renderer = nullptr;

    std::unique_ptr<Core::InputSystem> m_inputSystem = nullptr;
    std::unique_ptr<Core::FileManager> m_fileManager = nullptr;

    std::unique_ptr<Resource::ResourceManager> m_resourceManager = nullptr;

    std::unique_ptr<Scene::SceneManager> m_sceneManager = nullptr;

    // 起動時の設定。**Document と GameConfig は同じ寿命で持つ**
    // GameConfig の StringView は Document のアリーナ上にあるため、
    // Document を先に捨てるとタイトルなどがダングリングする (R0-5)
    std::unique_ptr<Json::Document> m_configDoc = nullptr;
    // 2-3: 個人の上書き (.local.json)。これも同じ寿命で持つ ―
    // 上書きが文字列に及ぶと StringView はこちらを指す (R0-5)
    std::unique_ptr<Json::Document> m_configLocalDoc = nullptr;
    std::unique_ptr<GameConfig>     m_config    = nullptr;

    bool m_isRunning = false;
    float m_totalTime = 0.0f;

    void Update(float dt);
    void Render();
  };
}