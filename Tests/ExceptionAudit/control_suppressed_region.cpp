/**
 * @file  control_suppressed_region.cpp
 * @brief T-ECS-27 control: a suppressed region does not spend the C4530 slot.
 *
 * @details
 *  ECS 1-8 measured this and it is what made the unmask technique usable at all.
 *  It is kept as a row because the audit now relies on the opposite property:
 *  **no production header may need a suppression.** If MSVC ever changed this,
 *  the first `try`/`catch` inside any suppressed include would start eating the
 *  slot again and the audit would go quiet without failing.
 *
 *  Two things in one file, on purpose:
 *   1. a `try`/`catch` inside `push` / `pop` - must NOT be reported
 *   2. a `try`/`catch` after the `pop`        - must be reported
 *  A CLEAN result here means the slot was eaten by (1).
 */
// EXPECT: C4530
// WHY: the suppressed handler must stay silent and leave the slot for the second one.

#pragma warning(push)
#pragma warning(disable: 4530)
inline void Suppressed() {
  try {
  }
  catch (...) {
  }
}
#pragma warning(pop)

void Sink2();

void Reported() {
  Suppressed();
  try {
    Sink2();
  }
  catch (...) {
  }
}
