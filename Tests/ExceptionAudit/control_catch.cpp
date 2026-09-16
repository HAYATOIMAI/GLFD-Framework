/**
 * @file  control_catch.cpp
 * @brief T-ECS-27 control: C4530 must fire on try / catch.
 *
 * @details
 *  This is the tooth of the **first** check. It is also the row that proves the
 *  C4530 slot is still free: MSVC emits C4530 **once per TU**, so if an include
 *  chain had already spent it, this file would come back CLEAN and the whole
 *  acceptance condition would be silently dead (ECS 2-2 / 21.5).
 */
// EXPECT: C4530
// WHY: try / catch with no throw. C4530 sees the handler, dumpbin sees no throw.

void Sink();

void Guarded() {
  try {
    Sink();
  }
  catch (...) {
  }
}
