#include "Shader.h"
#include <d3dcompiler.h>
#include <iostream>
#include <fstream>
#include "../Core/GameContext.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace GLFD::Graphics {

  static bool LoadFileToString(const std::string& filepath, std::string& outString)
  {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary); // 末尾にシークして開く
    if (!file.is_open()) {
      return false;
    }

    size_t fileSize = (size_t)file.tellg();
    outString.resize(fileSize);

    file.seekg(0, std::ios::beg); // 先頭に戻る
    file.read(&outString[0], fileSize);

    ///

    return true;
  }

  bool ParticleShader::Initialize(ID3D11Device* device) {
    // 1. HLSLファイルを読み込む
    std::string shaderCode;
    if (!LoadFileToString("Source/Shaders/particle.hlsl", shaderCode)) {
      std::cerr << "Error: Failed to load Particle.hlsl" << std::endl;
      return false;
    }

    //if (!ctx.fileManager->ReadTextFile("Source/Shaders/Particle.hlsl", shaderCode)) {
    //  std::cerr << "Error: Failed to load Particle.hlsl" << std::endl;
    //  return false;
    //}

    const char* codePtr = shaderCode.c_str();

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, gsBlob, psBlob;

    // 1. コンパイル
    if (!CompileShader(codePtr, "VS", "vs_4_0", &vsBlob)) return false;
    if (!CompileShader(codePtr, "GS", "gs_4_0", &gsBlob)) return false;
    if (!CompileShader(codePtr, "PS", "ps_4_0", &psBlob)) return false;

    // 2. 作成
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    device->CreateGeometryShader(gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(), nullptr, &m_geometryShader);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);

    // 3. InputLayout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    auto result = device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);

    if (FAILED(result)) {
      return false;
    }

    return true;
  }

  void ParticleShader::Bind(ID3D11DeviceContext* context) {
    context->IASetInputLayout(m_inputLayout.Get());
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->GSSetShader(m_geometryShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
  }

  bool ParticleShader::CompileShader(const char* code, const char* entry, const char* target, ID3DBlob** outBlob) {
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(code, strlen(code), nullptr, nullptr, nullptr, entry, target, 0, 0, outBlob, &errorBlob);
    
    if (FAILED(hr)) {
      if (errorBlob) std::cerr << "Shader Error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
      return false;
    }

    return true;
  }
}
