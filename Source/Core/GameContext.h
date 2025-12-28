#pragma once

#include "../Core/StackResource.h"
#include "../Threading/JobSystem.h"
#include "../ECS/Registry.h"
#include "../Events/EventBus.h"
#include "../Graphics/SimpleWindow.h"
#include "../Physics/SpatialHashGrid.h"
#include "InputSystem.h"
#include "DoubleStackAllocator.h"
#include "FileManager.h"

namespace GLFD {
  // 全システムが共有すべき「ゲームの世界」データ
  struct GameContext {
    // 永続的なリソース
    Memory::StackResource*    globalResource;
    // フレームごとのリソース
    Memory::StackResource*    frameResource;

    Thread::JobSystem*        jobSystem;
    ECS::Registry*            registry;
    Events::EventBus*         eventBus;

    Physics::SpatialHashGrid* grid;

    Graphics::SimpleWindow*   window;
    Core::InputSystem*        input;

    Core::FileManager* fileManager;

    float dt;
  };
}