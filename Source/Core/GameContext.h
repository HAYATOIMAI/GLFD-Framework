#pragma once

#include "../Core/StackResource.h"
#include "../Threading/JobSystem.h"
#include "../ECS/Registry.h"
#include "../Events/EventBus.h"
#include "../Graphics/SimpleWindow.h"
#include "../Graphics/DX11Renderer.h"
#include "../Physics/SpatialHashGrid.h"
#include "InputSystem.h"
#include "../Core/DoubleStackAllocator.h"
#include "FileManager.h"

namespace GLFD::Scene { class SceneManager; }
namespace GLFD::Resource { class ResourceManager; }

namespace GLFD {
  struct GameContext {
    Memory::StackResource*    globalResource;
    Memory::StackResource*    frameResource;

    Thread::JobSystem*        jobSystem;
    ECS::Registry*            registry;
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