#include "TextureLoader.h"
#include "../Core/GameContext.h"
#include "../Core/Logger.h"
#include "../Graphics/DX11Renderer.h"

namespace GLFD::Resource {

  std::unique_ptr<Graphics::Texture> TextureLoader::Load(
    const std::string& path, GameContext& ctx) {

    auto texture = std::make_unique<Graphics::Texture>();

    if (!texture->Load(ctx.renderer->GetDevice(), path)) {
      LOG_ERROR("TextureLoader: Failed to load '%s'", path.c_str());
      return nullptr;
    }

    LOG_INFO("TextureLoader: Loaded '%s'", path.c_str());
    return texture;
  }

} // namespace GLFD::Resource
