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
#include "ECS/Components.h"

#include "Events/EventBus.h"

#include "Graphics/SimpleWindow.h"
#include "Graphics/RenderSystem.h"
#include "Graphics/DX11Renderer.h"
#include "Graphics/Texture.h"

#include "Physics/SpatialHashGrid.h"
#include "Physics/CollisionSystem.h"

#include "Game/InteractionSystem.h"
#include "Game/SimdSystem.h"
#include "Game/BoidSystems.h"
#include "Game/BoidAgent.h"
#include "Game/GridBulidSystem.h"


#include <chrono>
#include <string>
#include <random>

namespace {
  constexpr auto USE_MEMORY_SIZE = 512 * 1024 * 1024; // 512MB
  constexpr auto FRAME_MEMORY_SIZE = 64 * 1024 * 1024; // 64MB
  constexpr auto NUM_ENTITIES = 20000;
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

    // メモリ確保
    m_mainStack = std::make_unique<Memory::StackAllocator>(USE_MEMORY_SIZE);
    m_stackResource = std::make_unique<Memory::StackResource>(*m_mainStack);
    m_frameAllocator = std::make_unique<Memory::DoubleStackAllocator>(FRAME_MEMORY_SIZE);

    // システム初期化
    m_fileManager = std::make_unique<Core::FileManager>(".");
    m_jobSystem = std::make_unique<Thread::JobSystem>();
    m_registry = std::make_unique<ECS::Registry>(m_stackResource.get());
    m_eventBus = std::make_unique<Events::EventBus>(m_stackResource.get());
    m_window = std::make_unique<Graphics::SimpleWindow>(GameConfig::WindowTitle, GameConfig::WindowWidth, GameConfig::WindowHeight);
    m_inputSystem = std::make_unique<Core::InputSystem>();

    // イベント登録
    m_eventBus->Register<Events::CollisionEvent>();

    GameContext ctx{
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr ,
        nullptr,
        nullptr,
        m_fileManager.get(),
        0.0f
    };

    // DX11初期化
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

    // デモシーン構築
    InitDemoScene();

    m_isRunning = true;
    LOG_INFO("=== Engine Systems Initialized === ");
  }
  void GameEngine::Run() {
    // 計測開始 (results.json に出力)
    //Core::Instrumentor::Get().BeginSession("Game Engine Profile");

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

      // FPS計測
      fpsTimer += dt;
      frames++;
      if (fpsTimer >= 1.0) {
        frames = 0;
        fpsTimer -= 1.0;
      }

      Update(0.016f);
      Render();
    }

    // 計測終了
    //Core::Instrumentor::Get().EndSession();
  }
  void GameEngine::Update(float dt) {

    m_frameAllocator->SwapAndReset();

    Memory::StackResource frameResource(m_frameAllocator->GetCurrent());

    // コンテキストの構築 (ポインタを渡すだけなので軽量)
    GameContext ctx{
        m_stackResource.get(),
        &frameResource,
        m_jobSystem.get(),
        m_registry.get(),
        m_eventBus.get(),
        nullptr ,
        m_window.get(),
        m_inputSystem.get(),
        m_fileManager.get(),
        dt
    };

    m_totalTime += dt;

    // 入力状態更新
    m_inputSystem->Update(m_window->GetHWND());

    Systems::GridBuildSystem::Update(ctx);

    Systems::InteractionSystem::Update(*m_registry, *m_jobSystem, *m_inputSystem, *m_window);

    Systems::BoidSystem::Update(ctx);

    // 移動
    Systems::MovementSystem::Update(ctx);

    ApplyWorldBounds(ctx);

    // 衝突
    Systems::CollisionSystem::Update(ctx);

    // イベント
    m_eventBus->DispatchAll();
  }
  void GameEngine::Render() {
    //PROFILE_FUNCTION();

    //m_renderer->BeginFrame();
    m_renderer->SetTexture(m_texture.get());

    Systems::RenderSystem::Update(*m_registry, *m_renderer, m_totalTime);
    //m_renderer->EndFrame();
  }

  void GameEngine::InitDemoScene() {

    std::mt19937 gen(12345);
    std::uniform_real_distribution<float> posDist(-20.0f, 20.0f);
    std::uniform_real_distribution<float> velDist(-0.5f, 0.5f);

    for (size_t i = 0; i < NUM_ENTITIES; ++i) {
      ECS::Entity e = m_registry->CreateEntity();
      m_registry->AddComponent<Components::Position>(e, posDist(gen), posDist(gen), 0.0f, 0.0f);
      m_registry->AddComponent<Components::Velocity>(e, velDist(gen), velDist(gen), 0.0f, 0.0f);
      m_registry->AddComponent<Components::Collider>(e, 0.3f);

      m_registry->AddComponent<Components::BoidAgent>(e);
    }
  }

  void GameEngine::ApplyWorldBounds(GameContext& context) {
    auto& positions = context.registry->View<Components::Position>();
    auto& velocities = context.registry->View<Components::Velocity>();

    size_t count = positions.GetSize();
    auto* pData = positions.GetData();
    auto* vData = velocities.GetData();

    const float bx = GameConfig::WorldBoundsX;
    const float by = GameConfig::WorldBoundsY;

    for (size_t i = 0; i < count; ++i) {
      if (pData[i].x < -bx) { pData[i].x = -bx; vData[i].vx = std::abs(vData[i].vx); }
      if (pData[i].x > bx) { pData[i].x = bx; vData[i].vx = -std::abs(vData[i].vx); }
      if (pData[i].y < -by) { pData[i].y = -by; vData[i].vy = std::abs(vData[i].vy); }
      if (pData[i].y > by) { pData[i].y = by; vData[i].vy = -std::abs(vData[i].vy); }
    }
  }
}
