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
// `FileManager` は **前方宣言だけ**にする (ECS 2-2)。`FileManager.h` を include すると
// `<filesystem>` → `<chrono>` が付いてきて、**TU に 1 回しか出ない C4530 の枠を
// 先に使い切る**。この構造体はポインタしか持たず、`ctx.fileManager` を
// 参照しているコードも 0 件なので、使う側が include すればよい
namespace GLFD::Core  { class FileManager; }
namespace GLFD::Resource { class ResourceManager; }

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

    Core::FileManager* fileManager;

    float dt;

    Graphics::DX11Renderer*   renderer;
    float                     totalTime;
    Scene::SceneManager*      sceneManager;
    Resource::ResourceManager* resourceManager;
  };
}