/**
 * @file  probe_root_job_dispatch.cpp
 * @brief Root cause 1 of the accepted debt: `std::function` allocates.
 *
 * @details
 *  `JobSystem::KickJob(JobFunction job, ...)` takes `std::function<void()>` **by
 *  value**, so the lambda is converted at every call site. MSVC keeps a small
 *  callable inline and **heap-allocates the rest**, which means a kick whose
 *  capture outgrows that buffer calls `operator new` and can throw
 *  `std::bad_alloc` **on the per-frame path**. N-2 forbids exactly that.
 *
 *  Measured, not reasoned - two TUs differing only in capture size:
 *    - `KickJob([&a]{ ... })`        (one reference)   -> no throw
 *    - `KickJob([b, &out]{ ... })`   (128-byte copy)   -> _CxxThrowException
 *  Neither produced C4530, which is why this survived eight phases of a check
 *  that was believed to cover it.
 *
 *  Three rows are dirty because of this one and no other reason:
 *    - `probe_hit_and_grid`        `HitSystem::Update` captures `[=, &grid, &bus]`
 *    - `probe_grid_build_filtered` `UpdateFiltered` captures the whole `View`
 *    - `probe_survivor_loop`       reaches both of the above
 *  **Fix this row and those three go CLEAN together.** Out of scope for 2-2:
 *  the repair is a non-allocating job payload, which is a phase of its own.
 *
 *  **ECS 2-6 3a: those three went CLEAN while this row stays THROW.** The call
 *  sites now go through `Thread::ParallelForChunks`, whose job captures only
 *  `&body` and the range (24 bytes, inside the inline buffer). `KickJob` itself
 *  is unchanged: a large capture still allocates, which is what this row checks.
 *  It stays THROW on purpose - it is the tooth for the other three.
 */
// EXPECT: THROW
// WHY: root cause 1 - std::function heap-allocates a large lambda. Pre-existing, not ECS 2-2.

#include "Threading/JobSystem.h"

// **Namespace scope, not an anonymous namespace.** With an internal-linkage
// parameter type MSVC generated no code at all and this row read CLEAN - the
// mistake `control_no_code.cpp` now guards against.
struct AuditPayload { double v[16]; };

void Kick(GLFD::Thread::JobSystem& js, AuditPayload payload, int& out) {
  js.KickJob([payload, &out]() { out = static_cast<int>(payload.v[0]); });
}
