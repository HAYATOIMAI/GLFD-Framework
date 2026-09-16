/**
 * @file  probe_root_logger.cpp
 * @brief Root cause 2 of the accepted debt: `Logger::LogFmt` builds a `std::string`.
 *
 * @details
 *  `LogFmt` formats into a 1024-byte stack buffer - and then wraps it in
 *  `std::string(buffer)` to call `Log`. That construction allocates and throws.
 *  Measured on its own: `std::string s(p);` alone produces _CxxThrowException
 *  and no C4530.
 *
 *  6.3 says a diagnostic must not allocate, because the moment you need it most
 *  is while recovering from a failed allocation. **The throw is the same defect
 *  seen from the other side**, and it is what makes `probe_diagnostics` dirty.
 *  Out of scope for 2-2: the fix is a `Log(const char*)` overload, which touches
 *  every call site.
 */
// EXPECT: THROW
// WHY: root cause 2 - Logger::LogFmt constructs a std::string. Pre-existing, not ECS 2-2.

#include "Core/Logger.h"

void Emit(int value) { LOG_INFO("value=%d", value); }
