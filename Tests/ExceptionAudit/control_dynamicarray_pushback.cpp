/**
 * @file  control_dynamicarray_pushback.cpp
 * @brief T-ECS-27 control: the check has teeth against the **real** N-2 violation.
 *
 * @details
 *  `control_throw.cpp` shows the symbol check works on a hand-written `throw`.
 *  This one shows it works on the shape we actually write by accident:
 *  `PushBack` -> `Grow()` -> `throw std::bad_alloc()` (`DynamicArray.h`).
 *  **Every `Try*`-less call in the engine looks like this and produced no C4530.**
 *
 *  16.4 says a probe must have the same shape as production. A hand-written
 *  `throw` is not the same shape - it is visible when you read the line.
 */
// EXPECT: THROW
// WHY: DynamicArray::PushBack is the throwing overload (N-2 violation if reached).

#include "Core/DynamicArray.h"
#include "Core/MemoryResource.h"

void Append(GLFD::DynamicArray<int>& a) { a.PushBack(3); }
