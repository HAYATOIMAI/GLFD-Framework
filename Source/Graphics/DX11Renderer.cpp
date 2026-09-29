#include "DX11Renderer.h"
#include "RenderFaultProbe.h"
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
    // 戻り値を見る (ECS 2-9)。以前は見ておらず、失敗は次の CreateRenderTargetView が空のバッファで
    // 失敗する形でしか分からなかった
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) {
      std::cerr << "Failed to get the swap chain's back buffer." << std::endl;
      return false;
    }

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
    // 描画を続けられなくなった後は呼ばない (ECS 2-9)。2-9 の手順1 で、消失したまま呼び続けると
    // 待たずに戻り、ループが 1 秒に約 11 万回まわった
    if (m_present.failure.failed) { return; }
    // 垂直同期 (VSync) ありで画面転送。窓が見えないと待たずに DXGI_STATUS_OCCLUDED が返る (ECS 2-8)
    const HRESULT hr = RenderFaultProbe::Call(RenderFaultProbe::Point::Present,
                                              [this] { return m_swapChain->Present(1, 0); });
    AfterPresent(m_present, hr, [this] { return RemovedReason(); });
  }

  bool DX11Renderer::IsOccluded() {
    if (!m_present.occluded) { return false; }
    if (m_present.failure.failed) { return true; }
    // DXGI_PRESENT_TEST: 何も転送せず、今の状態だけを返す。
    // **この値も同じ判定に通す** (ECS 2-9)。TDR では Present が消失より先に OCCLUDED を返した
    const HRESULT hr = RenderFaultProbe::Call(RenderFaultProbe::Point::PresentTest,
                                              [this] { return m_swapChain->Present(0, DXGI_PRESENT_TEST); });
    return AfterPresentTest(m_present, hr, [this] { return RemovedReason(); });
  }

  HRESULT DX11Renderer::RemovedReason() {
    return RenderFaultProbe::Call(RenderFaultProbe::Point::RemovedReason,
                                  [this] { return m_device->GetDeviceRemovedReason(); });
  }

  bool DX11Renderer::TakeSkippedDraw(HRESULT& mapReturned) {
    const bool skipped = m_skippedDraw;
    mapReturned = m_skippedMapHr;
    m_skippedDraw = false;
    m_skippedMapHr = S_OK;
    return skipped;
  }

  void DX11Renderer::DrawPoints(const SimpleVertex* points, size_t pointCount) {
    if (points == nullptr || pointCount == 0) return;
    if (m_present.failure.failed) return;   // ECS 2-9: 描画を続けられなくなった後は何もしない

    // 頂点バッファをCPUメモリで更新 (Map / Unmap)
    D3D11_MAPPED_SUBRESOURCE mapped = {};

    // バッファサイズを超えないように安全策
    size_t count = std::min(pointCount, (size_t)MAX_PARTICLES);

    const HRESULT hr = RenderFaultProbe::Call(RenderFaultProbe::Point::Map, [this, &mapped] {
      return m_deviceContext->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    });

    // **書けなかったら、このフレームは描かない** (ECS 2-9)。2-8 までは写しを飛ばして Draw だけ
    // 呼んでいたので、前のフレームの頂点が黙って描かれ続けた。デバイスが失われていたら
    // Present の失敗と同じく終わる。失われていなければ、状態の変わり目だけ Run がログに出す
    const MapOutcome mapResult = AfterMap(m_present.failure, hr, [this] { return RemovedReason(); });
    if (mapResult != MapOutcome::Written) {
      if (mapResult == MapOutcome::SkipFrame) {
        m_skippedDraw = true;
        m_skippedMapHr = hr;
      }
      return;
    }
    memcpy(mapped.pData, points, sizeof(SimpleVertex) * count);
    m_deviceContext->Unmap(m_vertexBuffer.Get(), 0);

    UINT stride = sizeof(SimpleVertex);
    UINT offset = 0;

    // シェーダーとInputLayoutをセット
    m_shader.Bind(m_deviceContext.Get());

    m_deviceContext->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);

    // **点の並びとして描く。描くたびに自分で設定する** (ECS 2-3)。
    // 2-3 まで一度も設定しておらず、頂点が 3 つに 1 つしか描かれていなかった
    // (点ではなく三角形の並びとして読まれ、三角形ごとに先頭の 1 点だけが GS に渡った)。
    // 起動時に 1 度だけ設定する形にしないのは、ほかの描画がトポロジーを変えたとき、
    // 点の描画がまた黙って壊れるため。描く直前に設定すれば他の描画の状態に左右されない。
    // 画面での歯: Tests\render_point_check.ps1(決まった位置に置いた点がすべて描かれるか)
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

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
    // 戻り値を見る (ECS 2-9)。以前は見ずに true を返していたので、作れなくても空の頂点バッファのまま
    // 起動していた (毎フレームの Map が失敗し続ける形になる)
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_vertexBuffer))) {
      std::cerr << "Failed to create the vertex buffer." << std::endl;
      return false;
    }

    return true;
  }

  bool DX11Renderer::CreateBlendState() {
    // ブレンドステート作成 (通常のα合成。ECS 2-3 で加算合成から変えた)
    D3D11_BLEND_DESC bd = {};
    bd.AlphaToCoverageEnable = FALSE;
    bd.IndependentBlendEnable = FALSE; // 全てのレンダーターゲットで同じ設定

    // 0番目のレンダーターゲットの設定
    bd.RenderTarget[0].BlendEnable = TRUE;

    // 色の合成式: (Source * SrcAlpha) + (Dest * (1 - SrcAlpha))
    // Source: これから描く色, Dest: 既に描かれている色
    // **後から描いたものが上に重なる。** 以前の加算合成 (Dest * 1.0) では、重なると色が
    // 足されて混ざり、緑の経験値が赤い敵に重なると弾の黄に近づいた (ECS 2-3 で画面を撮って確認)。
    // 全体で 1 つの設定にし、シーンごとの切り替えは作らない
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
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