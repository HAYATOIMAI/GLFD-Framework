/**
 * @file  probe_scene_manager.cpp
 * @brief `SceneManager` transitions - and a demonstration of the audit's blind spot.
 *
 * @warning **This row is CLEAN and that means almost nothing.**
 *          `PushScene` / `PopScene` / `ChangeScene` / `ProcessPendingTransitions`
 *          are declared here but **defined in `SceneManager.cpp`**, so calling
 *          them from a probe emits a call to an external symbol and generates no
 *          code to audit. The throwing `EmplaceBack` / `PushBack` inside them is
 *          invisible from this TU.
 *
 *          The `translation_units.txt` list exists for exactly this: it audits
 *          `Source/Scene/SceneManager.cpp` itself, where those definitions live,
 *          and **that row is dirty**. Keep both - this one catches a throw moving
 *          into the header, that one catches the ones already in the .cpp.
 */
// EXPECT: CLEAN
// WHY: only declarations live in the header. the real check is the .cpp row.

#include "Core/GameContext.h"
#include "Scene/SceneManager.h"

void Drive(GLFD::Scene::SceneManager& sm, GLFD::GameContext& ctx,
           std::unique_ptr<GLFD::Scene::IScene> scene) {
  sm.PushScene(std::move(scene));
  sm.PopScene();
  sm.ProcessPendingTransitions(ctx);
  sm.Update(ctx);
  sm.Render(ctx);
  (void)sm.HasActiveScene();
}
