#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include "../Core/HashMap.h"
#include "../Core/MemoryResource.h"

namespace GLFD::Graphics {
  // シェーダーの種類
  enum class ShaderType {
    Vertex,
    Pixel,
    Geometry,
    Compute // 将来用
  };

  // 1つのシェーダープログラム（VS/PS/GS + InputLayout）を管理するリソース
  struct ShaderResource {
    Microsoft::WRL::ComPtr<ID3D11VertexShader>   vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>    pixelShader;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometryShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>    inputLayout;

    // コンテキストにバインドするヘルパー
    void Bind(ID3D11DeviceContext* context);
  };

  class ShaderManager {
  public:
    // メモリアロケータを受け取る
    explicit ShaderManager(ID3D11Device* device, Memory::IMemoryResource* resource);
    ~ShaderManager();

    // シェーダーをロードまたは取得する
    // 既にロード済みの場合はキャッシュを返す
    ShaderResource* LoadShader(const std::string& name,
      const std::string& vsPath,
      const std::string& psPath,
      const std::string& gsPath = "");

    // 毎フレーム呼び出し（ホットリロードの監視）
    void Update();

  private:
    ID3D11Device* m_device;
    Memory::IMemoryResource* m_resource;

    // ホットリロード管理用構造体
    struct ShaderSourceInfo {
      std::string name;
      std::string vsPath;
      std::string psPath;
      std::string gsPath;

      std::filesystem::file_time_type lastWriteTimeVS;
      std::filesystem::file_time_type lastWriteTimePS;
      std::filesystem::file_time_type lastWriteTimeGS;

      ShaderResource* resourcePtr = nullptr; // StackAllocator上のポインタ
    };

    std::unordered_map<std::string, ShaderSourceInfo> m_shaderMap;

    // 内部ヘルパー: コンパイルとリフレクション
    bool CompileAndCreate(ShaderSourceInfo& info, bool isReloading);

    // 単体シェーダーコンパイル
    Microsoft::WRL::ComPtr<ID3DBlob> CompileShaderBlob(const std::string& path, const char* entry, const char* target);

    // リフレクションによるInputLayout作成
    bool CreateInputLayoutFromVS(const void* vsBytecode, size_t vsLength, ID3D11InputLayout** outLayout);
  };
}
