/**
 * @file  probe_survivor_render.cpp
 * @brief The Survivor vertex building split out of SurvivorScene.cpp in ECS 2-3.
 *
 * @details Per-frame path: three View walks, one TryResize to grow and one to shrink.
 *          N-2 says it must not throw. SurvivorScene.cpp as a whole is THROW through
 *          LOG_* (translation_units.txt), so this probe is the only row that sees the
 *          vertex building by itself.
 */
// EXPECT: CLEAN
// WHY: 2-3: the per-frame vertex building. Try* only, no Logger, no DX11.

#include "Core/DynamicArray.h"
#include "ECS/Registry.h"
#include "Game/SurvivorRender.h"
#include "Graphics/RenderStatus.h"
#include "Graphics/SimpleVertex.h"

GLFD::Systems::RenderStatus Build(GLFD::ECS::Registry& registry,
                                  GLFD::DynamicArray<GLFD::Graphics::SimpleVertex>& vertices) {
  return GLFD::Game::BuildSurvivorVertices(registry, vertices);
}
