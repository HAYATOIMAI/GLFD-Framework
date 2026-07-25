#pragma once
#include <cstddef>
#include <string>

namespace GLFD::Resource {

  // Type-safe resource handle (lightweight, copyable)
  template <typename T>
  struct ResourceHandle {
    size_t id = 0;

    [[nodiscard]] bool IsValid() const { return id != 0; }
    bool operator==(const ResourceHandle& other) const { return id == other.id; }
    bool operator!=(const ResourceHandle& other) const { return id != other.id; }
  };

  // Runtime FNV-1a hash (same algorithm as TypeInfo::Hash)
  inline size_t HashString(const std::string& str) {
    size_t hash = 14695981039346656037ull;
    for (char c : str) {
      hash ^= static_cast<size_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

} // namespace GLFD::Resource
