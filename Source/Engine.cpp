#include "Engine.h"

#include "Core/MemoryUtils.h"
#include "Core/StackAllocator.h"
#include "Core/MemoryResource.h"
#include "Core/GameContext.h"
#include "Core/GameConfig.h"
#include "Core/InputSystem.h"
#include "Core/Profiler.h"
#include "Core/Logger.h"
#include "Core/FileManager.h"

#include "Threading/JobSystem.h"

#include "ECS/Registry.h"

#include "Events/EventBus.h"

#include "Graphics/SimpleWindow.h"
#include "Graphics/DX11Renderer.h"
#include "Graphics/Texture.h"

#include "Scene/SceneManager.h"
#include "Game/BoidDemoScene.h"

#include <chrono>
#include <string>

namespace {
  constexpr auto USE_MEMORY_SIZE = 512 * 1024 * 1024; // 512MB
  constexpr auto FRAME_MEMORY_SIZE = 64 * 1024 * 1024; // 64MB
}

namespace GLFD {
  GameEngine::GameEngine() = default;
  GameEngine::~GameEngine() {
    LOG_INFO("=== Engine Shutdown ===");
    Core::Logger::Get().Shutdown();
  }

  void GameEngine::Initialize() {

    Core::Logger::Get().Initialize("Game.log");
    LOG_INFO("=== Engine Startup ===");

    m_mainStack = std::make_unique<Memory::StackAllocator>(USE_MEMORY_SIZE);
    m_stackResource = std::make_unique<Memory::StackResource>(*m_mainStack);
    m_frameAllocator = std::make_unique<Memory::DoubleStackAllocator>(FRAME_MEMORY_SIZE);

    m_fileManager = std::make_unique<Core::FileManager>(".");
    m_jobSystem = std::make_unique<Thread::JobSystem>();
    m_registry = std::make_unique<ECS::Registry>(m_stackResource.get());
    m_eventBus = std::make_unique<Events::EventBus>(m_stackResource.get());
    m_window = std::make_unique<Graphics::SimpleWindow>(GameConfig::WindowTitle, GameConfig::WindowWidth, GameConfig::WindowHeight);
    m_inputSystem = std::make_unique<Core::InputSystem>();

    m_renderer = std::make_unique<Graphics::DX11Renderer>();
    if (!m_renderer->Initialize(m_window->GetHWND(), GameConfig::WindowWidth, GameConfig::WindowHeight)) {
#ifdef _DEBUG
      std::cerr << "DX11 Init Failed!" << std::endl;
#endif // DEBUG
      LOG_ERROR("DX11 Init Failed!");
      m_isRunning = false;
      return;
    }
    LOG_INFO("=== DX11 Initialize Success ===");

    m_texture = std::make_unique<Graphics::Texture>();

    if (!m_texture->Load(m_renderer->GetDevice(), "Resource/particle.png")) {
      std::cerr << "Texture Load Failed!" << std::endl;
      LOG_ERROR("Texture Load Failed!");
      m_isRunning = false;
      return;
    }

    m_sceneManager = std::make_unique<Scene::SceneManager>(m_stackResource.get());

    GameContext initCtx{
        m_stackResource.get(),
        nullptr,
        m_jobSystem.get(),
        m_registry.get(),
        m_eventBus.get(),
        nullptr,
        m_window.get(),
        m_inputSystem.get(),
        m_fileManager.get(),
        0.0f,
        m_renderer.get(),
        m_texture.get(),
        0.0f,
        m_sceneManager.get()
    };

    m_sceneManager->PushScene(std::make_unique<BoidDemoScene>());
    m_sceneManager->ProcessPendingTransitions(initCtx);

    m_isRunning = true;
    LOG_INFO("=== Engine Systems Initialized === ");
  }

  void GameEngine::Run() {
    auto lastTime = std::chrono::high_resolution_clock::now();
    int frames = 0;
    double fpsTimer = 0.0;

    while (m_isRunning) {
      if (!m_window->ProcessMessages()) {
        m_isRunning = false;
        break;
      }

      auto currentTime = std::chrono::high_resolution_clock::now();
      std::chrono::duration<float> dtSec = currentTime - lastTime;
      lastTime = currentTime;
      float dt = dtSec.count();

      fpsTimer += dt;
      frames++;
      if (fpsTimer >= 1.0) {
        frames = 0;
        fpsTimer -= 1.0;
      }

      Update(0.016f);
      Render();
    }
  }

  void GameEngine::Update(float dt) {

    m_frameAllocator->SwapAndReset();

    Memory::StackResource frameResource(m_frameAllocator->GetCurrent());

    m_totalTime += dt;

    GameContext ctx{
        m_stackResource.get(),
        &frameResource,
        m_jobSystem.get(),
        m_registry.get(),
        m_eventBus.get(),
        nullptr,
        m_window.get(),
        m_inputSystem.get(),
        m_fileManager.get(),
        dt,
        m_renderer.get(),
        m_texture.get(),
        m_totalTime,
        m_sceneManager.get()
    };

    m_inputSystem->Update(m_window->GetHWND());

    m_sceneManager->Update(ctx);

    m_eventBus->DispatchAll();

    m_sceneManager->ProcessPendingTransitions(ctx);
  }

  void GameEngine::Render() {
    GameContext ctx{
        m_stackResource.get(),
        nullptr,
        m_jobSystem.get(),
        m_registry.get(),
        m_eventBus.get(),
        nullptr,
        m_window.get(),
        m_inputSystem.get(),
        m_fileManager.get(),
        0.0f,
        m_renderer.get(),
        m_texture.get(),
        m_totalTime,
        m_sceneManager.get()
    };

    m_sceneManager->Render(ctx);
  }
}