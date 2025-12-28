#pragma once

#include "DX11Renderer.h"
#include "../ECS/Registry.h"
#include "../ECS/Components.h"
#include "../ECS/View.h"
#include "../Core/Profiler.h"
#include "../Core/GameConfig.h"
#include <vector>

namespace GLFD::Systems {
  class RenderSystem {
  public:
    // メイン処理
    static void Update(ECS::Registry& registry, Graphics::DX11Renderer& renderer, float time) {

      // 画面クリア (黒)
     renderer.BeginFrame();

     // 定数バッファ更新
     float width = static_cast<float>(GameConfig::WindowWidth);
     float height = static_cast<float>(GameConfig::WindowHeight);
     //float aspect = width / height;

     // 高さ0対策
     if (height < 1.0f) height = 1.0f;

     // 必ず float として計算する
     float aspect = width / height;

     // 値がおかしい場合の安全策 (1.0を入れておく)
     if (aspect < 0.0001f) aspect = 1.0f;

     renderer.UpdateGlobalConstants(aspect, time);

      auto& view = registry.View<Components::Position>(); // 全ての位置を取得
      size_t count = view.GetSize();
      auto* pData = view.GetData();

      // DX11用の一時バッファ
      static std::vector<Graphics::SimpleVertex> vertices;
      vertices.resize(count);

      // 画面サイズ定数 (GameConfigから取ると良い)
      // ワールド座標(-50~50) を NDC座標(-1.0~1.0) に変換する係数
      // アスペクト比修正は一旦無視して正方形として扱う
      const float scaleX = 1.0f / 85.0f; // 画面端が85くらい
      const float scaleY = 1.0f / 64.0f;

      // データ変換
      for (size_t i = 0; i < count; ++i) {
        vertices[i].Pos = DirectX::XMFLOAT4(
          pData[i].x * scaleX,
          pData[i].y * scaleY,
          0.0f,
          1.0f
        );
        // 白い点
        vertices[i].Color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
      }

      // GPUへ転送して描画
      renderer.DrawPoints(vertices);

      renderer.EndFrame();
    }
  };
}
