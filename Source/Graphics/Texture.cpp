#include "Texture.h"
#include "../Core/GameContext.h"
#include <iostream>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace GLFD::Graphics {
  bool Texture::Load(ID3D11Device* device, const std::string& filename) {
    int width = 0, height = 0, channels = 0;

    // RGBA (4チャンネル) で強制的に読み込む
   // std::vector<uint8_t> data;

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, 4);

    if (!data) {
      std::cerr << "Failed to load texture: " << filename << std::endl;
      return false;
    }

    // テクスチャの設定
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // RGBA 8bit
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE; // 作成後に変更しない
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // 初期データ
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = data;
    initData.SysMemPitch = width * 4; // 1行のバイト数 (幅 * 4byte)

    HRESULT hr = device->CreateTexture2D(&desc, &initData, &m_texture);

    // メモリ解放 (GPUに転送したので不要)
    stbi_image_free(data);

    if (FAILED(hr)) return false;

    // 3. シェーダーリソースビュー (SRV) 作成
    hr = device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv);
    return SUCCEEDED(hr);
  }
}
