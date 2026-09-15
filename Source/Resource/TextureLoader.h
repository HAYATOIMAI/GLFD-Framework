#pragma once
#include "IResourceLoader.h"
#include "../Graphics/Texture.h"
#include "../Core/GameContext.h"
#include <memory>
#include <string>

namespace GLFD::Resource {

  class TextureLoader : public IResourceLoader<Graphics::Texture> {
  public:
    std::unique_ptr<Graphics::Texture> Load(
      const std::string& path, GameContext& ctx) override;
  };

} // namespace GLFD::Resource