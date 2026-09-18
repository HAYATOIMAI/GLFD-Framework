/**
 * @file  probe_scene_manager.cpp
 * @brief `SceneManager` transitions - and a demonstration of the audit's blind spot.
 *
 * @warning **A CLEAN verdict here means almost nothing on its own.**
 *          `PushScene` / `PopScene` / `ChangeScene` / `ProcessPendingTransitions`
 *          are declared in the header but **defined in `SceneManager.cpp`**, so
 *          calling them from a probe emits a call to an external symbol and
 *          generates no code to audit.
 *
 *          `translation_units.txt` exists for exactly this: it compiles
 *          `Source/Scene/SceneManager.cpp` as itself, where those definitions
 *          live. Keep both rows - this one catches a throw moving into the
 *          header, that one covers the bodies.
 */
// EXPECT: CLEAN
// WHY: only declarations live in the header. the real check is the .cpp row.

#include "Core/GameContext.h"
#include "Scene/SceneManager.h"

void Drive(GLFD::Scene::SceneManager& sm, GLFD::GameContext& ctx,
           std::unique_ptr<GLFD::Scene::IScene> scene) {
  (void)sm.PushScene(std::move(scene));
  (void)sm.PopScene();
  sm.ProcessPendingTransitions(ctx);
  sm.Update(ctx);
  sm.Render(ctx);
  (void)sm.HasActiveScene();
  (void)sm.Depth();
  (void)sm.Report().HasProblem();
  sm.ResetReport();
}
