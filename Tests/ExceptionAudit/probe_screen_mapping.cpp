/**
 * @file  probe_screen_mapping.cpp
 * @brief The world <-> screen mapping added in ECS 2-13 (Graphics/ScreenMapping.h).
 *
 * @details Per-frame path: BoidDemoScene builds the mapping every frame for the render and the
 *          click, and RenderSystem / InteractionSystem call NdcX / NdcY / PixelToWorld. N-2 says
 *          it must not throw. BoidDemoScene.cpp as a whole is THROW through LOG_*
 *          (translation_units.txt), so this probe is the only row that sees the mapping by itself.
 */
// EXPECT: CLEAN
// WHY: 2-13: the per-frame world <-> screen mapping. Arithmetic only, no Logger, no DX11.

#include "Graphics/ScreenMapping.h"

GLFD::Graphics::WorldPoint ClickToWorld(float visibleHalfY, int windowWidth, int windowHeight, int px, int py) {
  const GLFD::Graphics::ScreenMapping m = GLFD::Graphics::ScreenMappingOf(visibleHalfY, windowWidth, windowHeight);
  return m.PixelToWorld(px, py);
}

float WorldToNdc(float visibleHalfY, int windowWidth, int windowHeight, float x, float y) {
  const GLFD::Graphics::ScreenMapping m = GLFD::Graphics::ScreenMappingOf(visibleHalfY, windowWidth, windowHeight);
  return m.NdcX(x) + m.NdcY(y) + m.aspect;
}
