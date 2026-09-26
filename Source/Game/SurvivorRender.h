#pragma once

/**
 * @file  SurvivorRender.h
 * @brief Survivor の頂点列を組む (ECS 2-3)。**描画はしない**
 *
 * @details
 *  ## なぜ切り出したか
 *  1-8 から 2-3 までは `SurvivorScene.cpp` の無名名前空間の `DrawSurvivor` が、
 *  頂点を組む処理と DX11 の呼び出しを 1 つの関数で行っていた。そのため
 *  **種類ごとの色も、頂点の数も、確保失敗の扱いも、テストから確かめられなかった。**
 *  頂点を組む部分だけをここに移し、シーン・テスト・ベンチが**同じ関数を呼ぶ**
 *  (開発手法 §4.7)。
 *
 *  ## 層
 *  **`DX11Renderer.h`(`d3d11.h`)を引き込まない。** Graphics からは頂点の型
 *  (`SimpleVertex.h`)と結果の型(`RenderStatus.h`)だけを使う。
 *  **種類と色の対応は Game 側に置く。** 描画は「敵なら赤」を知らない。
 *
 *  ## 種類の決め方(成分の組み合わせ)
 *  | 種類 | 成分 | 色 |
 *  |---|---|---|
 *  | プレイヤー | (エンティティではない。原点の回収点に 1 点) | 白 |
 *  | 敵 | `Position` + `Health` | 赤 |
 *  | 弾 | `Position` + `Damage` | 黄 |
 *  | 経験値 | `Position` + `Pickup` | 緑 |
 *
 *  **色はハードコードする。** 色は実装の一部であって調整する設定ではない。
 *  調整したくなったら `GameConfig` の `$version` を上げて移す(A-2 の手順)。
 */

#include "../Core/DynamicArray.h"
#include "../ECS/Components.h"
#include "../ECS/Registry.h"
#include "../ECS/View.h"
#include "../Graphics/RenderStatus.h"
#include "../Graphics/SimpleVertex.h"
#include "SurvivorComponents.h"

#include <cstddef>

namespace GLFD::Game {

  /// ワールド座標から NDC へ。**4:3 の窓の比**(1-8 で決めた値)
  inline constexpr float kSurvivorScaleX = 1.0f / 45.0f;
  inline constexpr float kSurvivorScaleY = 1.0f / 34.0f;

  struct SurvivorColor {
    float r, g, b;
  };

  inline constexpr SurvivorColor kSurvivorPlayerColor = { 1.0f, 1.0f,  1.0f  };  ///< 白
  inline constexpr SurvivorColor kSurvivorEnemyColor  = { 1.0f, 0.25f, 0.25f };  ///< 赤
  inline constexpr SurvivorColor kSurvivorBulletColor = { 1.0f, 0.9f,  0.2f  };  ///< 黄
  inline constexpr SurvivorColor kSurvivorPickupColor = { 0.3f, 1.0f,  0.4f  };  ///< 緑

  /**
   * @brief 敵 / 弾 / 経験値 / プレイヤーの頂点を `vertices` に組む
   *
   * @param registry 読むだけ(構造は変えない)
   * @param vertices 組んだ頂点の置き場所。**呼ぶ側がフレームメモリで作って渡す。**
   *                 成功したとき、要素数は描く頂点の数ちょうどになる
   * @return 成功: `Drawn` と描く数。確保失敗: `VertexBufferUnavailable` と
   *         確保しようとした数(配列は空のまま)。**黙って諦めない** (R-28)
   *
   * @details
   *  3 本の `View` の `BaseSize()` の合計 + 1 を容量にする。**実際に出る数はこれ以下**
   *  (基準プールの範囲であって返る件数ではない。§18.7)。出し終えたら要素数を
   *  出した数まで縮める(縮小は確保を伴わないので失敗しない)。
   */
  [[nodiscard]] inline Systems::RenderStatus BuildSurvivorVertices(
      ECS::Registry& registry, DynamicArray<Graphics::SimpleVertex>& vertices) noexcept {
    auto enemies = registry.View<Components::Position, Components::Health>();
    auto bullets = registry.View<Components::Position, Components::Damage>();
    auto pickups = registry.View<Components::Position, Components::Pickup>();

    // 原点のプレイヤー(回収点)に 1 点足す
    const std::size_t capacity = enemies.BaseSize() + bullets.BaseSize() + pickups.BaseSize() + 1u;
    if (!vertices.TryResize(capacity)) {
      return Systems::RenderStatus{ Systems::RenderStatus::Outcome::VertexBufferUnavailable, capacity };
    }

    std::size_t count = 0;
    const auto emit = [&vertices, &count](float x, float y, const SurvivorColor& c) {
      vertices[count].Pos   = DirectX::XMFLOAT4(x * kSurvivorScaleX, y * kSurvivorScaleY, 0.0f, 1.0f);
      vertices[count].Color = DirectX::XMFLOAT4(c.r, c.g, c.b, 1.0f);
      ++count;
    };

    emit(0.0f, 0.0f, kSurvivorPlayerColor);
    for (auto [e, pos, hp] : enemies) {
      (void)e; (void)hp;
      emit(pos.x, pos.y, kSurvivorEnemyColor);
    }
    for (auto [e, pos, dmg] : bullets) {
      (void)e; (void)dmg;
      emit(pos.x, pos.y, kSurvivorBulletColor);
    }
    for (auto [e, pos, pick] : pickups) {
      (void)e; (void)pick;
      emit(pos.x, pos.y, kSurvivorPickupColor);
    }

    (void)vertices.TryResize(count);   // 縮小は確保を伴わないので必ず成功する
    return Systems::RenderStatus{ Systems::RenderStatus::Outcome::Drawn, count };
  }

}
