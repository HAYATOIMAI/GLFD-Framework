#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

namespace GLFD::Core {
  struct GameContext;
}

namespace GLFD::Graphics {
  class Texture {
  public:
    // 画像ファイルからテクスチャを作成
    bool Load(ID3D11Device* device, const std::string& filename);

    // シェーダーに渡すためのビューを取得
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
    ID3D11ShaderResourceView* const* GetAddress() const { return m_srv.GetAddressOf(); }

  private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
  };
}
