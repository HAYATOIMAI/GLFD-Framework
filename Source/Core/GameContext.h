#pragma once

#include "../Core/StackResource.h"
#include "../Threading/JobSystem.h"
#include "../ECS/Registry.h"
#include "../ECS/CommandBuffer.h"
#include "../Events/EventBus.h"
#include "../Graphics/SimpleWindow.h"
#include "../Graphics/DX11Renderer.h"
#include "../Physics/SpatialHashGrid.h"
#include "InputSystem.h"
#include "../Core/DoubleStackAllocator.h"

namespace GLFD::Scene { class SceneManager; }
namespace GLFD::Core { struct GamePaths; }

namespace GLFD {
  struct GameContext {
    Memory::StackResource*    globalResource;
    Memory::StackResource*    frameResource;

    Thread::JobSystem*        jobSystem;
    ECS::Registry*            registry;
    /// 構造変更の積み place (1-4)。**反復中の生成・破棄はここへ積む**
    ECS::CommandBuffer*       commands;
    Events::EventBus*         eventBus;

    Physics::SpatialHashGrid* grid;

    Graphics::SimpleWindow*   window;
    Core::InputSystem*        input;

    float dt;

    Graphics::DX11Renderer*   renderer;
    Scene::SceneManager*      sceneManager;

    /// 根と、設定・シェーダー・ログのパス (ECS 2-16)。`GameEngine::Initialize` の頭で 1 回だけ決める。
    /// F5 の読み直しもここを読むので、起動時と同じファイルを読む。描画の ctx には入れない (nullptr)
    const Core::GamePaths*    paths = nullptr;
  };
}