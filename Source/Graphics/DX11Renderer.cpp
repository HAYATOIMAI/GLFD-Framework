#include "DX11Renderer.h"
#include <iostream>

namespace GLFD::Graphics {
  DX11Renderer::DX11Renderer() = default;
  DX11Renderer::~DX11Renderer() = default;

  bool DX11Renderer::Initialize(void* hwnd, int width, int height) {
    m_width = width;
    m_height = height;

    // 1. スワップチェーンの設定
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;                                    // バックバッファの数
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;     // 色の形式 (32bit RGBA)
    scd.BufferDesc.RefreshRate.Numerator = 60;              // 60FPS
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;      // 描画対象として使う
    scd.OutputWindow = static_cast<HWND>(hwnd);             // 描画先のウィンドウ
    scd.SampleDesc.Count = 1;                               // アンチエイリアスなし
    scd.Windowed = TRUE;                                    // ウィンドウモード
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    // 2. デバイスとスワップチェーンの作成
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr,                    // デフォルトのアダプタ(GPU)
      D3D_DRIVER_TYPE_HARDWARE,   // ハードウェアアクセラレーション
      nullptr,
      0,                          // フラグ (デバッグ時は D3D11_CREATE_DEVICE_DEBUG を入れると良い)
      nullptr, 0,                 // 機能レベル (デフォルト)
      D3D11_SDK_VERSION,
      &scd,
      &m_swapChain,
      &m_device,
      &featureLevel,
      &m_deviceContext
    );

    if (FAILED(hr)) {
      std::cerr << "Failed to create D3D11 Device." << std::endl;
      return false;
    }

    // バックバッファからレンダーターゲットビューを作成
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);

    if (FAILED(hr)) {
      std::cerr << "Failed to create RenderTargetView." << std::endl;
      return false;
    }

    // ビューポートの設定
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    m_deviceContext->RSSetViewports(1, &viewport);

    // ステートをセット
    m_deviceContext->RSSetState(m_rasterizerState.Get());

    if (!m_shader.Initialize(m_device.Get())) return false;

    // 定数バッファ初期化
    if (!m_cbGlobal.Initialize(m_device.Get())) return false;

    if (!CreateVertexBuffer()) return false;

    if (!CreateBlendState()) {
      return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;

    hr = m_device->CreateRasterizerState(&rd, &m_rasterizerState);
    if (FAILED(hr)) return false;

    m_deviceContext->RSSetState(m_rasterizerState.Get());

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; // 滑らかに補間
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;   // 端っこを引き伸ばさない
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_device->CreateSamplerState(&sd, &m_samplerState);
    if (FAILED(hr)) return false;

    return true;
  }

  void DX11Renderer::BeginFrame() {
    // 画面クリア色 (RGBA: Cornflower Blue っぽい色)
    float clearColor[] = { 0.05f, 0.05f, 0.1f, 1.0f };

    // バックバッファを指定色で塗りつぶす
    m_deviceContext->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);

    // 描画先をバックバッファに設定
    m_deviceContext->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
  }

  void DX11Renderer::EndFrame() {
    // 垂直同期 (VSync) ありで画面転送
    m_swapChain->Present(1, 0);
  }

  void DX11Renderer::DrawPoints(const std::vector<SimpleVertex>& points) {
    if (points.empty()) return;

    // 頂点バッファをCPUメモリで更新 (Map / Unmap)
    D3D11_MAPPED_SUBRESOURCE mapped = {};

    // バッファサイズを超えないように安全策
    size_t count = std::min((size_t)points.size(), (size_t)MAX_PARTICLES);

    auto hr = m_deviceContext->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

    if (SUCCEEDED(hr)) {
      memcpy(mapped.pData, points.data(), sizeof(SimpleVertex) * count);
      m_deviceContext->Unmap(m_vertexBuffer.Get(), 0);
    }

    UINT stride = sizeof(SimpleVertex);
    UINT offset = 0;

    // シェーダーとInputLayoutをセット
    m_shader.Bind(m_deviceContext.Get());

    m_deviceContext->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);

    if (m_currentTexture) {
      m_deviceContext->PSSetShaderResources(0, 1, m_currentTexture->GetAddress());
    }

    // テクスチャとサンプラーをPSステージにセット
    m_deviceContext->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());

    // ジオメトリシェーダーに定数バッファを渡す
    m_deviceContext->GSSetConstantBuffers(0, 1, m_cbGlobal.GetAddress());

    float blendFactor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_deviceContext->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffff);

    // 描画命令
    m_deviceContext->Draw(static_cast<UINT>(count), 0);

    m_deviceContext->GSSetShader(nullptr, nullptr, 0);
  }

  void DX11Renderer::UpdateGlobalConstants(float aspectRatio, float time) {
    ConstantBufferData data = { aspectRatio, time, {0, 0} };
    m_cbGlobal.Update(m_deviceContext.Get(), data);
  }

  void DX11Renderer::SetTexture(Texture* texture) {
    if (texture == nullptr) {
      assert("Texture is nullptr");
      return;
    }

    m_currentTexture = texture;
  }

  bool DX11Renderer::CreateVertexBuffer() {
    // 動的頂点バッファ作成 (CPU書き込み可)
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;             // CPUから頻繁に書き換える
    bd.ByteWidth = sizeof(SimpleVertex) * MAX_PARTICLES;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; // 書き込み許可
    m_device->CreateBuffer(&bd, nullptr, &m_vertexBuffer);

    return true;
  }

  bool DX11Renderer::CreateBlendState() {
    // ブレンドステート作成 (加算合成)
    D3D11_BLEND_DESC bd = {};
    bd.AlphaToCoverageEnable = FALSE;
    bd.IndependentBlendEnable = FALSE; // 全てのレンダーターゲットで同じ設定

    // 0番目のレンダーターゲットの設定
    bd.RenderTarget[0].BlendEnable = TRUE;

    // 色の合成式: (Source * SrcAlpha) + (Dest * 1.0)
    // Source: これから描く色, Dest: 既に描かれている色
    // これにより、透明度を考慮しつつ、色がどんどん足し算されていく（発光する）
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;

    // アルファの合成式（今回はあまり重要ではないが設定しておく）
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    auto hr = m_device->CreateBlendState(&bd, &m_blendState);

    if (FAILED(hr)) {
      std::cerr << "Failed to create Blend State." << std::endl;
      return false;
    }

    return true;
  }
}