/**
 * @file  control_throw.cpp
 * @brief T-ECS-27 control: the symbol check must fire on throw.
 *
 * @details
 *  **C4530 never fires here.** That is the finding that made this second check
 *  necessary: the warning reports the *handler*, not the *raise*. Measured on
 *  four TUs (try/catch, `throw 1`, `throw std::bad_alloc()`, and a real
 *  `DynamicArray::PushBack`) - only the first produced C4530.
 */
// EXPECT: THROW
// WHY: raises without handling. C4530 is blind to this; _CxxThrowException is not.

#include <new>

void Raise() { throw std::bad_alloc(); }
