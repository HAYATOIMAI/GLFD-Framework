#pragma once

#include "../Core/StackResource.h"
#include "../Threading/JobSystem.h"
#include "../ECS/Registry.h"
#include "../Events/EventBus.h"
#include "../Graphics/SimpleWindow.h"
#include "../Graphics/DX11Renderer.h"
#include "../Graphics/Texture.h"
#include "../Physics/SpatialHashGrid.h"
#include "InputSystem.h"
#include "DoubleStackAllocator.h"
#include "FileManager.h"

namespace GLFD::Scene { class SceneManager; }

namespace GLFD {
  // �S�V�X�e�������L���ׂ��u�Q�[���̐��E�v�f�[�^
  struct GameContext {
    // �i���I�ȃ��\�[�X
    Memory::StackResource*    globalResource;
    // �t���[�����Ƃ̃��\�[�X
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
    Graphics::Texture*        texture;
    float                     totalTime;
    Scene::SceneManager*      sceneManager;
  };
}