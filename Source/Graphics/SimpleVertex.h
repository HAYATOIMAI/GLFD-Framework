#pragma once

/**
 * @file  SimpleVertex.h
 * @brief GPU に送る頂点の形(ECS 2-3 で `DX11Renderer.h` から移した)
 *
 * @details
 *  **`d3d11.h` を引き込まない。** 頂点を組むのは Game 側(`Game/SurvivorRender.h`)で、
 *  そこをテストとベンチから呼ぶため、頂点の型だけを軽いヘッダに置く。
 *  `DirectXMath.h` はヘッダだけの数学ライブラリで、D3D11 には依存しない。
 *
 *  ## フィールドの使い方
 *  | フィールド | 中身 |
 *  |---|---|
 *  | `Pos.x` / `Pos.y` | NDC の位置 |
 *  | `Pos.z` | 使っていない(常に 0) |
 *  | `Pos.w` | 1 |
 *  | `Color` | 頂点の色 (r, g, b, a) |
 *
 *  @note 入力レイアウト(`Shader.cpp`)は `POSITION` をオフセット 0、`COLOR` を
 *        オフセット 16 に置いている。**このヘッダの並びと大きさを変えるときは、
 *        入力レイアウトとシェーダーも同時に変えること。** 下の `static_assert` が
 *        片方だけ変えた状態をビルドで止める。
 */

#include <DirectXMath.h>

#include <cstddef>

namespace GLFD::Graphics {

  struct SimpleVertex {
    DirectX::XMFLOAT4 Pos;    ///< 位置 (x, y, z, w)
    DirectX::XMFLOAT4 Color;  ///< 色 (r, g, b, a)
  };

  static_assert(sizeof(SimpleVertex) == 32u,
                "SimpleVertex must stay 32 bytes: the vertex buffer stride and the input layout "
                "in Shader.cpp (POSITION at 0, COLOR at 16) are written for this size");
  static_assert(offsetof(SimpleVertex, Color) == 16u,
                "SimpleVertex::Color must start at byte 16: see the input layout in Shader.cpp");

}
