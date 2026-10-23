/**
 * @file  probe_root_path.cpp
 * @brief ECS 2-16: deciding the game's root (`Core/RootPath.h`) must not throw.
 *
 * @details
 *  The root is decided before the Logger is opened. A throw there would end the
 *  process before any log line exists, so the search uses fixed arrays and Win32
 *  calls only (no std::wstring, no std::filesystem).
 */
// EXPECT: CLEAN
// WHY: 2-16 - FindRoot / FindRootFrom / JoinPath run before the Logger exists; fixed arrays and Win32 only.

#include "Core/RootPath.h"

namespace {
  bool NoFile(const wchar_t*, void*) { return false; }
}

int FindBoth(const wchar_t* exe) {
  static GLFD::Core::RootSearch real;
  static GLFD::Core::RootSearch fake;
  GLFD::Core::FindRoot(real);
  GLFD::Core::FindRootFrom(exe, &NoFile, nullptr, fake);
  wchar_t joined[64];
  const bool fits = GLFD::Core::JoinPath(joined, 64, L"C:\\a", L"b");
  return static_cast<int>(real.status) + static_cast<int>(fake.status) + (fits ? 1 : 0);
}
