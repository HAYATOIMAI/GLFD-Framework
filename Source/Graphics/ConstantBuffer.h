#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <vector>

namespace GLFD::Graphics {
  // 任意のデータ型 T を管理する定数バッファ
  template <typename T>
  class ConstantBuffer {
  public:
    // 作成 (初期化)
    bool Initialize(ID3D11Device* device) {
      D3D11_BUFFER_DESC bd = {};
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.ByteWidth = sizeof(T);

      // 定数バッファは16バイトの倍数サイズである必要がある
      if (bd.ByteWidth % 16 != 0) {
        // パディング不足の警告などを出しても良い
        // ここでは自動補正はせず、使う側の責任とする(構造体定義でPaddingを入れる)
      }

      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      bd.CPUAccessFlags = 0; // UpdateSubresourceを使うので0

      HRESULT hr = device->CreateBuffer(&bd, nullptr, &m_buffer);
      return SUCCEEDED(hr);
    }

    // データを更新してGPUへ転送
    void Update(ID3D11DeviceContext* context, const T& data) {
      context->UpdateSubresource(m_buffer.Get(), 0, nullptr, &data, 0, 0);
    }

    // 生のバッファを取得 (バインド用)
    ID3D11Buffer* Get() const { return m_buffer.Get(); }
    ID3D11Buffer* const* GetAddress() const { return m_buffer.GetAddressOf(); }

  private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_buffer;
  };
}