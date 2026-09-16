/**
 * @file  probe_game_context.cpp
 * @brief The 2-2 deliverable: `GameContext.h` must not spend the C4530 slot.
 *
 * @details
 *  Before 2-2 this row was C4530, from two entry points and nothing else:
 *   - `Threading/JobSystem.h` -> `<future>` -> `ppltasks.h(1580)`  (0 users in Source/)
 *   - `Core/FileManager.h`    -> `<filesystem>` -> `<chrono>`      (pointer only here)
 *
 *  **Every other dirty header in `Source/` was a transcription of these two**
 *  (measured: 111 single-header TUs, 74 engine headers + 36 standard ones).
 *  Of the standard library only `<future>` / `<filesystem>` / `<chrono>` fire;
 *  `<string>` `<vector>` `<memory>` `<iostream>` `<fstream>` `<stdexcept>`
 *  `<functional>` `<random>` `<mutex>` do not.
 *
 *  If this row goes back to C4530, someone re-included one of the two. The
 *  `control_catch.cpp` row is what tells you the check itself still works.
 */
// EXPECT: CLEAN
// WHY: the slot must be free here, because every Game/ probe below includes this file.

#include "Core/GameContext.h"

void TouchContext(GLFD::GameContext& ctx) {
  (void)ctx.registry;
  (void)ctx.commands;
  (void)ctx.eventBus;
  (void)ctx.grid;
  (void)ctx.fileManager;   // pointer only. the forward declaration is enough
}
