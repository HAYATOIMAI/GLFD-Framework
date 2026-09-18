#include "Engine.h"

#include "Core/MemoryUtils.h"
#include "Core/StackAllocator.h"
#include "Core/MemoryResource.h"
#include "Core/GameContext.h"
#include "Core/GameConfig.h"
#include "Core/GameConfigLoad.h"
#include "Core/GameConfigLog.h"
#include "Core/Json/Json.h"
#include "Core/InputSystem.h"
#include "Core/Logger.h"
#include "Core/FileManager.h"

#include "Threading/JobSystem.h"

#include "ECS/Registry.h"
#include "ECS/CommandBuffer.h"

#include "Events/EventBus.h"

#include "Graphics/SimpleWindow.h"
#include "Graphics/DX11Renderer.h"

#include "Resource/ResourceManager.h"
#include "Resource/TextureLoader.h"

#include "Scene/SceneManager.h"
#include "Game/BoidDemoScene.h"
#include "Game/SceneCatalog.h"
#include "Game/SurvivorScene.h"

#include <chrono>
#include <string>

namespace {
  constexpr auto USE_MEMORY_SIZE = 512 * 1024 * 1024; // 512MB
  constexpr auto FRAME_MEMORY_SIZE = 64 * 1024 * 1024; // 64MB

  /// UTF-8 の StringView を Win32 へ渡すためのワイド文字列にする
  std::wstring ToWide(GLFD::StringView utf8) {
    if (utf8.Empty()) { return std::wstring(); }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.Data(),
                                             static_cast<int>(utf8.Size()), nullptr, 0);
    if (needed <= 0) { return std::wstring(); }
    std::wstring wide(static_cast<size_t>(needed), wchar_t{});
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.Data(), static_cast<int>(utf8.Size()),
                          wide.data(), needed);
    return wide;
  }
}

namespace GLFD {
  GameEngine::GameEngine() = default;
  GameEngine::~GameEngine() {
    if (m_resourceManager) {
      m_resourceManager->ReleaseAll();
    }
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
    // **メインスレッドで作る。** 構築したスレッドが所有者になり、他スレッドからの
    // 積み込み・適用を Debug の assert が捕まえる (R-18)
    m_commands = std::make_unique<ECS::CommandBuffer>(m_stackResource.get());
    m_eventBus = std::make_unique<Events::EventBus>(m_stackResource.get());
    // --- 設定の読み込み ------------------------------------------------------
    // Document は GameConfig と同じ寿命で持つ。StringView (title など) が
    // Document のアリーナを指しているため (R0-5)
    {
      // 2 段レイヤ (2-3)。.local.json は無いのが普通で、あれば上書きされる
      ConfigDiagnosticsLogger diagnostics;
      const bool loaded = ReloadGameConfig(m_configDoc, m_configLocalDoc, m_config,
                                           kGameConfigPath, kGameConfigLocalPath,
                                           m_stackResource.get(), diagnostics);
      if (loaded) {
        LOG_INFO("config loaded: %s (version %u)%s", kGameConfigPath, diagnostics.Version(),
                 diagnostics.SawLocal() ? " + GameConfig.local.json" : "");
      }
      else {
        // **起動不能にしない。** 既定値のまま続行する。
        // 失敗の位置は診断が file(line,col) 形式で出している
        LOG_WARN("config could not be loaded (%s). falling back to built-in defaults",
                 kGameConfigPath);
        m_configDoc      = std::make_unique<Json::Document>(m_stackResource.get());
        m_configLocalDoc = std::make_unique<Json::Document>(m_stackResource.get());
        m_config         = std::make_unique<GameConfig>(m_stackResource.get());
      }
    }

    // 適用値をログに残す。設定が効いていることを起動ログだけで確認できる
    LOG_INFO("window: %dx%d entities=%d maxSpeed=%.2f halfExtent=(%.1f, %.1f)",
             m_config->window.width, m_config->window.height,
             m_config->simulation.entityCount, m_config->simulation.maxSpeed,
             m_config->world.halfExtent[0], m_config->world.halfExtent[1]);

    const std::wstring windowTitle = ToWide(m_config->window.title);
    m_window = std::make_unique<Graphics::SimpleWindow>(
        windowTitle, m_config->window.width, m_config->window.height);
    m_inputSystem = std::make_unique<Core::InputSystem>();

    m_renderer = std::make_unique<Graphics::DX11Renderer>();
    if (!m_renderer->Initialize(m_window->GetHWND(),
                                m_config->window.width, m_config->window.height)) {
#ifdef _DEBUG
      std::cerr << "DX11 Init Failed!" << std::endl;
#endif // DEBUG
      LOG_ERROR("DX11 Init Failed!");
      m_isRunning = false;
      return;
    }
    LOG_INFO("=== DX11 Initialize Success ===");

    m_resourceManager = std::make_unique<Resource::ResourceManager>();
    m_resourceManager->RegisterLoader<Graphics::Texture>(
        std::make_unique<Resource::TextureLoader>());

    m_sceneManager = std::make_unique<Scene::SceneManager>(m_stackResource.get());

    GameContext initCtx{
        m_stackResource.get(),
        nullptr,
        m_jobSystem.get(),
        m_registry.get(),
        m_commands.get(),
        m_eventBus.get(),
        nullptr,
        m_window.get(),
        m_inputSystem.get(),
        m_fileManager.get(),
        0.0f,
        m_renderer.get(),
        0.0f,
        m_sceneManager.get(),
        m_resourceManager.get()
    };

    auto texHandle = m_resourceManager->Load<Graphics::Texture>("Resource/particle.png", initCtx);
    if (!texHandle.IsValid()) {
      LOG_ERROR("Failed to load particle texture via ResourceManager");
      m_isRunning = false;
      return;
    }

    // The start scene comes from the config (ECS 1-8); 2-1 added the runtime
    // switch. Both read the same table in SceneCatalog.h, so a scene name
    // never has to be written down twice.
    const StringView startScene = m_config->startScene;
    auto scene = Game::MakeScene(startScene);
    if (scene == nullptr) {
      LOG_WARN("start scene '%.*s' is not known. starting '%s' instead",
               static_cast<int>(startScene.Size()), startScene.Data(),
               Game::DefaultSceneName());
      scene = Game::MakeDefaultScene();
    }
    else {
      LOG_INFO("start scene: %.*s", static_cast<int>(startScene.Size()), startScene.Data());
    }
    if (!m_sceneManager->PushScene(std::move(scene))) {
      LOG_ERROR("could not queue the start scene. the engine has nothing to run");
      m_isRunning = false;
      return;
    }
    m_sceneManager->ProcessPendingTransitions(initCtx);
    Game::ReportTransitions(m_sceneManager->Report(), m_sceneManager->Depth());
    m_sceneManager->ResetReport();

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
        m_commands.get(),
        m_eventBus.get(),
        nullptr,
        m_window.get(),
        m_inputSystem.get(),
        m_fileManager.get(),
        dt,
        m_renderer.get(),
        m_totalTime,
        m_sceneManager.get(),
        m_resourceManager.get()
    };

    m_inputSystem->Update(m_window->GetHWND());

    // The scene switch is read before the scene runs (2-1). The table is the one
    // in SceneCatalog.h that the start-scene selection above also uses. Pressing
    // the key of the scene you are already in re-enters it, which is how the
    // round trip gets exercised by hand.
    for (std::size_t i = 0; i < Game::kSceneCount; ++i) {
      if (!m_inputSystem->IsTriggered(Game::kSceneCatalog[i].key)) { continue; }
      LOG_INFO("scene: '%c' pressed. changing to %s",
               static_cast<char>(Game::kSceneCatalog[i].key), Game::kSceneCatalog[i].name);
      if (!m_sceneManager->ChangeScene(Game::kSceneCatalog[i].make())) {
        LOG_ERROR("scene: could not queue the change to %s", Game::kSceneCatalog[i].name);
      }
      break;
    }

    m_sceneManager->Update(ctx);

    m_eventBus->DispatchAll();

    m_sceneManager->ProcessPendingTransitions(ctx);
    Game::ReportTransitions(m_sceneManager->Report(), m_sceneManager->Depth());
    m_sceneManager->ResetReport();
  }

  void GameEngine::Render() {
    // **描画にもフレームメモリを渡す** (ECS 1-1)。`RenderSystem` の頂点バッファが
    // 関数ローカルの `static std::vector`(N-1 / N-3 抵触)から
    // `DynamicArray` + `IMemoryResource` へ移ったため、確保元が要る。
    //
    // `SwapAndReset()` は `Update()` の先頭で呼ばれ、`Run()` は
    // `Update(); Render();` の順なので、**このフレームの領域は Render が
    // 終わるまで生きている**。取った分は次フレームの先頭でまとめて戻る。
    Memory::StackResource frameResource(m_frameAllocator->GetCurrent());

    GameContext ctx{
        m_stackResource.get(),
        &frameResource,
        m_jobSystem.get(),
        m_registry.get(),
        // **描画にも同じバッファを渡す。** nullptr を入れると、将来ここで
        // 積んだ人が落ちる地雷になる。描画中に積まれたものは次フレームの
        // 適用で効く(描画は構造を変えない前提なので、通常は空のまま)
        m_commands.get(),
        m_eventBus.get(),
        nullptr,
        m_window.get(),
        m_inputSystem.get(),
        m_fileManager.get(),
        0.0f,
        m_renderer.get(),
        m_totalTime,
        m_sceneManager.get(),
        m_resourceManager.get()
    };

    m_sceneManager->Render(ctx);
  }
}