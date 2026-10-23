// GLFD (since ECS 2-14): offsets and sizes measured by the compiler, for the specs of Tests/disasm_compare.py.
//
// Only the CURRENT types are measured here, so this file always builds: Tests\build_and_run.bat compiles it on
// every run (since ECS 2-15). Before 2-15 it carried #ifdef branches for members that 2-14 removed, nothing built
// it, and those branches no longer compiled.
//
// When a phase removes a member: add the member to a temporary #ifdef branch, measure the tree that still has it
// (measure_layout.py <tree> <tag> measure_layout.cpp <DEFINE>) and the tree without it, write the spec, and take the
// branch out again before the phase is committed.
//
// private / protected are made public in this TU only, so private members can be measured.
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
  P(GameEngine, m_inputSystem); P(GameEngine, m_sceneManager); P(GameEngine, m_isRunning); P(GameEngine, m_jobDropsSeen);
  P(GameEngine, m_droppingJobs); P(GameEngine, m_exitReason); P(GameEngine, m_closeRequest); P(GameEngine, m_exitMessage);
  P(GameEngine, m_paths);
  P(Graphics::DX11Renderer, m_vertexBuffer); P(Graphics::DX11Renderer, m_width); P(Graphics::DX11Renderer, m_height);
  P(GameContext, input); P(GameContext, dt); P(GameContext, renderer); P(GameContext, sceneManager); P(GameContext, paths);
  P(Core::Logger, m_fileStream); P(Core::Logger, m_mutex);
  P(Graphics::SimpleWindow, m_hwnd); P(Graphics::SimpleWindow, m_height);
  return 0;
}
