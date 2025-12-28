#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

namespace GLFD {
  struct GameContext;
}

namespace GLFD::Graphics {
  class ParticleShader {
  public:
    bool Initialize(ID3D11Device* device);

    // 描画設定（Contextにシェーダーをセット）
    void Bind(ID3D11DeviceContext* context);

  private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> m_geometryShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;

    // コンパイルヘルパー
    bool CompileShader(const char* code, const char* entry, const char* target, ID3DBlob** outBlob);
  };
}
