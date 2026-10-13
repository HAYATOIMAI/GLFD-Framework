#pragma once

/**
 * @file  ScreenMapping.h
 * @brief 世界と画面の対応 (ECS 2-13)。**描画(世界 → NDC)とクリック(画素 → 世界)は、この 1 つから導く**
 *
 * @details
 *  ## なぜ作ったか
 *  2-12 までは、同じ対応を 3 か所が別々の値で持っていた。
 *   - Boid の描画: `RenderSystem` に `1/85, 1/64` の直書き(1024×768 の窓を前提にした 4:3 の値)
 *   - Boid のクリック: `InteractionSystem` が `GameConfig` の `interaction.screenScale = 6.0`
 *     (1024×768 の窓での 1 単位あたりの画素)。**縦の向きが描画と逆**で、上をクリックすると下で爆発した
 *     (最初のコミットから。2-13 手順1 で実測)
 *   - Boid の壁: `GameConfig` の `world.halfExtent`。F5 で変えても描画は追随しなかった
 *  描画とクリックを別の値で持つと、片方だけ直したときに、ずれが別の形で残る (要件書 §28.7)。
 *
 *  ## 対応の決め方(縦を合わせ、横は窓に合わせる)
 *  **縦の見える半寸法 `visibleHalfY` を画面の上下の端に合わせる。** 横は 1 単位の画素が縦と同じに
 *  なるように窓の縦横比から決める。Survivor が 2-3 で採った形と同じ。
 *   - Boid は `world.halfExtent.y`(壁の縦)を渡す。壁の上下が画面の上下の端に来る。
 *     横は 16:9 の窓なら壁(±halfExtent.x)より広く見え、壁の外が余白になる (2-13 の判断 A)
 *   - Survivor(上下 34 単位)は `SurvivorScreenOf` が同じ形で作っている
 *
 *  ## 座標の向き
 *   - NDC は右が +x、**上が +y**(D3D のビューポート)
 *   - 画素(クライアント座標)は右が +x、**下が +y**。画素 `px` は `[px, px+1)` を占め、中心は `px + 0.5`
 *   - 世界は NDC と同じ向き(上が +y)
 *
 *  ## 窓の大きさ
 *  幅・高さが 1 未満(最小化中など)なら 1 として扱い、割り算で壊れない。`visibleHalfY` が正の有限の
 *  値でなければ 1 として扱う(設定ファイルから 0 や負が来ても有限のまま)。
 *
 *  @note **d3d11.h を引き込まない。** Game 側(`InteractionSystem` / `SurvivorRender.h`)とテストから使う
 */

#include <cmath>

namespace GLFD::Graphics {

  /// 世界の 1 点
  struct WorldPoint {
    float x;
    float y;
  };

  /// 世界と画面の対応。`ScreenMappingOf` で作る
  struct ScreenMapping {
    float scaleX;   ///< 世界 → NDC の横。1 単位の画素が縦と同じになる値
    float scaleY;   ///< 世界 → NDC の縦。`1 / visibleHalfY`
    float aspect;   ///< 窓の幅 / 高さ。シェーダー(GS)が点の四角の横の大きさに使う
    float width;    ///< 窓の幅(画素。1 以上)
    float height;   ///< 窓の高さ(画素。1 以上)

    /// 世界 → NDC の横
    [[nodiscard]] constexpr float NdcX(float worldX) const noexcept { return worldX * scaleX; }
    /// 世界 → NDC の縦
    [[nodiscard]] constexpr float NdcY(float worldY) const noexcept { return worldY * scaleY; }

    /**
     * @brief 画素(クライアント座標。下が +y)→ 世界。**描画(`NdcX` / `NdcY` とビューポート)の逆**
     * @details 画素の中心(`px + 0.5`)を NDC に直し、縮尺で割る。縦は画素が下向き、NDC が上向きなので
     *          符号が反転する
     */
    [[nodiscard]] WorldPoint PixelToWorld(int px, int py) const noexcept {
      const float ndcX = (static_cast<float>(px) + 0.5f) * 2.0f / width - 1.0f;
      const float ndcY = 1.0f - (static_cast<float>(py) + 0.5f) * 2.0f / height;
      return WorldPoint{ ndcX / scaleX, ndcY / scaleY };
    }
  };

  /**
   * @brief 縦の見える半寸法と窓の大きさ(画素)から対応を作る
   * @details NDC の横 2 が幅 w 画素、縦 2 が高さ h 画素。1 単位の画素を縦横で揃えるには
   *          `scaleX * w = scaleY * h`
   */
  [[nodiscard]] inline ScreenMapping ScreenMappingOf(float visibleHalfY, int windowWidth, int windowHeight) noexcept {
    const float w    = (windowWidth  < 1) ? 1.0f : static_cast<float>(windowWidth);
    const float h    = (windowHeight < 1) ? 1.0f : static_cast<float>(windowHeight);
    const float half = (std::isfinite(visibleHalfY) && visibleHalfY > 0.0f) ? visibleHalfY : 1.0f;
    const float scaleY = 1.0f / half;
    return ScreenMapping{ scaleY * h / w, scaleY, w / h, w, h };
  }

}
