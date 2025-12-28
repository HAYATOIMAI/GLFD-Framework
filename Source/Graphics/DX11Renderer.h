#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX // std::min/max との衝突回避

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <DirectXMath.h>
#include <vector>
#include "Shader.h"
#include "ConstantBuffer.h"
#include "Texture.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")


namespace GLFD::Graphics {
  // 定数バッファのデータ構造
  struct ConstantBufferData {
    float AspectRatio;  // 画面のアスペクト比 (Width / Height)
    float Time;         // ゲーム経過時間 (秒)
    float Padding[2];   // 16バイトに揃えるための詰め物
  };

  // GPUに送る頂点の形式
  struct SimpleVertex {
    DirectX::XMFLOAT4 Pos;   // 位置 (x, y, z)
    DirectX::XMFLOAT4 Color; // 色   (r, g, b, a)
  };

  class DX11Renderer {
  public:
    DX11Renderer();
    ~DX11Renderer();

    // コピー禁止
    DX11Renderer(const DX11Renderer&) = delete;
    DX11Renderer& operator=(const DX11Renderer&) = delete;
    // 初期化
    bool Initialize(void* hwnd, int width, int height);
    // 描画開始
    void BeginFrame();
    // 描画終了
    void EndFrame();

    // パーティクル描画用メソッドを追加
    // CPU側で計算した頂点リストを受け取ってGPUに送る
    void DrawPoints(const std::vector<SimpleVertex>& points);

    void UpdateGlobalConstants(float aspectRatio, float time);

    void SetTexture(Texture* texture);

    // デバイスコンテキスト取得
    ID3D11DeviceContext* GetDeviceContext() const { return m_deviceContext.Get(); }
    // デバイス取得
    ID3D11Device* GetDevice() const { return m_device.Get(); }
  private:
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_deviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState>       m_blendState;

    ParticleShader m_shader;
    ConstantBuffer<ConstantBufferData>   m_cbGlobal;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerState;
    Texture* m_currentTexture = nullptr; // 描画するテクスチャ

    // バッファの最大容量 (これを超えたら分割して描画)
    static const int MAX_PARTICLES = 100000;

    // シェーダー初期化用のヘルパー
    bool CreateVertexBuffer();

    bool CreateBlendState();

    int m_width = 0;
    int m_height = 0;
  };
}