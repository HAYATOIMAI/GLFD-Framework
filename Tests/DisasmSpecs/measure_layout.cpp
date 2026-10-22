// 2-14: offsets of the members a removal touches, measured by the compiler (private made public in this TU only).
// Build with /DFIELDS_BASE for the tree that still has the removed members.
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include <optional>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#define private public
#define protected public
#include "Engine.h"
#include "Graphics/DX11Renderer.h"
#include "Core/GameContext.h"
#include "Core/Logger.h"
#include "Graphics/SimpleWindow.h"
#define P(T, m) std::printf("%-40s %zu\n", #T "::" #m, offsetof(T, m))
#define S(T) std::printf("%-40s %zu\n", "sizeof " #T, sizeof(T))
int main() {
  using namespace GLFD;
  S(GameEngine); S(Graphics::DX11Renderer); S(GameContext); S(Core::Logger); S(Graphics::SimpleWindow);
  P(GameEngine, m_fileManager); P(GameEngine, m_sceneManager); P(GameEngine, m_isRunning); P(GameEngine, m_jobDropsSeen); P(GameEngine, m_droppingJobs); P(GameEngine, m_exitReason); P(GameEngine, m_closeRequest); P(GameEngine, m_exitMessage);
  P(Graphics::DX11Renderer, m_vertexBuffer); P(Graphics::DX11Renderer, m_width); P(Graphics::DX11Renderer, m_height);
  P(GameContext, renderer); P(GameContext, sceneManager);
#ifdef FIELDS_C3
  P(GameEngine, m_totalTime); P(GameContext, totalTime); P(GameContext, dt);
  S(Graphics::ConstantBufferData);
#endif
#ifdef FIELDS_C4
  S(Graphics::ConstantBufferData);
#endif
#ifdef FIELDS_C4L
  P(Core::Logger, m_fileStream); P(Core::Logger, m_mutex); P(Core::Logger, m_consoleCodePageChanged);
#endif
#ifdef FIELDS_C5L
  P(Core::Logger, m_fileStream); P(Core::Logger, m_mutex);
#endif
#ifdef FIELDS_C5W
  P(Graphics::SimpleWindow, m_height); P(Graphics::SimpleWindow, m_bufferMemory); P(Graphics::SimpleWindow, m_bmi);
#endif
#ifdef FIELDS_C0
  P(GameEngine, m_resourceManager);
  P(Graphics::DX11Renderer, m_samplerState); P(Graphics::DX11Renderer, m_currentTexture);
  P(GameContext, resourceManager);
#endif
  return 0;
}
