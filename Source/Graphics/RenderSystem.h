#pragma once

#include "DX11Renderer.h"
#include "RenderStatus.h"   // RenderStatus は ECS も DX11 も引き込まないヘッダへ移した (ECS 2-3)
#include "../ECS/Registry.h"
#include "../ECS/Components.h"
#include "../ECS/View.h"
#include "../Core/GameConfig.h"
#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"

#include <cstddef>
#include <cstdint>

namespace GLFD::Systems {

  class RenderSystem {
  public:
    // メイン処理
    /// @param width / height 実際のウィンドウサイズ。**設定の型に依存させない**ため
    ///        GameConfig ではなく値で受け取る(レンダラを単体で試せる形を保つ)
    /// @param frameResource 頂点の一時バッファの確保元。**フレームごとに Reset
    ///        されるもの**を渡すこと。以前は関数ローカルの `static std::vector`
    ///        で、N-1(STL コンテナ)と N-3(グローバル可変状態)に抵触し、
    ///        **スレッド安全でもなかった** (ECS-0 の 2)
    [[nodiscard]] static RenderStatus Update(ECS::Registry& registry,
                       Graphics::DX11Renderer& renderer,
                       Memory::IMemoryResource* frameResource, float time,
                       int windowWidth, int windowHeight) {

      // 画面クリア (黒)
     renderer.BeginFrame();

     // 定数バッファ更新
     float width = static_cast<float>(windowWidth);
     float height = static_cast<float>(windowHeight);
     //float aspect = width / height;

     // 高さ0対策
     if (height < 1.0f) height = 1.0f;

     // 必ず float として計算する
     float aspect = width / height;

     // 値がおかしい場合の安全策 (1.0を入れておく)
     if (aspect < 0.0001f) aspect = 1.0f;

     renderer.UpdateGlobalConstants(aspect, time);

      // 全ての位置を取得。**単一型なので飛ばされる要素が無く、
      // dense 配列をそのまま舐められる** (1-5)
      auto view = registry.View<Components::Position>();
      const size_t count = view.BaseSize();
      const Components::Position* const pData = view.BaseComponents();

      // DX11 用の一時バッファ。**フレームメモリから取る**ので、
      // グローバル状態にもならずスレッド安全にもなる
      DynamicArray<Graphics::SimpleVertex> vertices(frameResource);
      if (!vertices.TryResize(count)) {
        // フレームメモリを使い切った。**この 1 フレームは点を描かない**。
        // 1-6 で観測できる形にした。**黙って諦めない** (R-28)
        renderer.EndFrame();
        return RenderStatus{ RenderStatus::Outcome::VertexBufferUnavailable, count };
      }

      // 画面サイズ定数 (GameConfigから取ると良い)
      // ワールド座標(-50~50) を NDC座標(-1.0~1.0) に変換する係数
      // アスペクト比修正は一旦無視して正方形として扱う
      const float scaleX = 1.0f / 85.0f; // 画面端が85くらい
      const float scaleY = 1.0f / 64.0f;

      // データ変換
      if (pData == nullptr) {
        // 成分プールを確保できていない。**1-5 で足した経路で、当時は
        // コメントも無く黙って戻っていた。** 1-6 で同じ扱いにする
        renderer.EndFrame();
        return RenderStatus{ RenderStatus::Outcome::ComponentsUnavailable, count };
      }
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
      renderer.DrawPoints(vertices.GetData(), vertices.GetSize());

      renderer.EndFrame();
      return RenderStatus{ RenderStatus::Outcome::Drawn, count };
    }
  };
}
