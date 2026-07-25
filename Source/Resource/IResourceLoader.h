#pragma once
#include <memory>
#include <string>

namespace GLFD { struct GameContext; }

namespace GLFD::Resource {

  // Per-resource-type loader interface
  template <typename T>
  class IResourceLoader {
  public:
    virtual ~IResourceLoader() = default;
    virtual std::unique_ptr<T> Load(const std::string& path, GameContext& ctx) = 0;
  };

} // namespace GLFD::Resource
