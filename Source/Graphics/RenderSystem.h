#pragma once

#include "DX11Renderer.h"
#include "RenderStatus.h"
#include "ScreenMapping.h"   // 世界 → NDC の対応。クリックの逆変換と同じもの (ECS 2-13)   // RenderStatus は ECS も DX11 も引き込まないヘッダへ移した (ECS 2-3)
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
    /// @param screen 世界 → NDC の対応 (ECS 2-13)。**クリックの逆変換(`InteractionSystem`)と同じものを渡す。**
    ///        縦の見える半寸法(Boid は `world.halfExtent.y`)と実際のウィンドウサイズから
    ///        `ScreenMappingOf` で作る。**設定の型に依存させない**ため値で受け取る
    ///        (レンダラを単体で試せる形を保つ)
    /// @param frameResource 頂点の一時バッファの確保元。**フレームごとに Reset
    ///        されるもの**を渡すこと。以前は関数ローカルの `static std::vector`
    ///        で、N-1(STL コンテナ)と N-3(グローバル可変状態)に抵触し、
    ///        **スレッド安全でもなかった** (ECS-0 の 2)
    [[nodiscard]] static RenderStatus Update(ECS::Registry& registry,
                       Graphics::DX11Renderer& renderer,
                       Memory::IMemoryResource* frameResource, float time,
                       const Graphics::ScreenMapping& screen) {

      // 画面クリア (黒)
     renderer.BeginFrame();

     // 定数バッファ更新。縦横比は対応が持つ(幅・高さ 0 の守りもそちら。ECS 2-13)
     renderer.UpdateGlobalConstants(screen.aspect, time);

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

      // 世界 → NDC は `screen` が持つ (ECS 2-13)。2-12 まではここに `1/85, 1/64` の直書きがあり、
      // 1024x768 の窓を前提にした値で、16:9 の窓では縦に潰れていた。壁(`halfExtent`)とも別の値だった

      // データ変換
      if (pData == nullptr) {
        // 成分プールを確保できていない。**1-5 で足した経路で、当時は
        // コメントも無く黙って戻っていた。** 1-6 で同じ扱いにする
        renderer.EndFrame();
        return RenderStatus{ RenderStatus::Outcome::ComponentsUnavailable, count };
      }
      // z は丸の半径 (NDC の縦)。GS が読む。**シェーダーに暗黙の既定値を置かない**ので、
      // ここで明示的に書く。値は 2-3 までシェーダーに固定で書かれていた大きさ (ECS 2-3 (3))
      constexpr float kBoidRadiusNdc = 0.05f;
      for (size_t i = 0; i < count; ++i) {
        vertices[i].Pos = DirectX::XMFLOAT4(
          screen.NdcX(pData[i].x),
          screen.NdcY(pData[i].y),
          kBoidRadiusNdc,
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
