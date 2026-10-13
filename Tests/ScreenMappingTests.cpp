/**
 * @file  ScreenMappingTests.cpp
 * @brief T-ECS-52: 世界と画面の対応 (ECS 2-13)
 *
 * @details
 *  シーンが描画とクリックに使うのと同じ `Graphics::ScreenMappingOf` / `ScreenMapping` を呼ぶ
 *  (手順を写さない。開発手法 §4.7)。窓の大きさを差し込んで決定的に確かめる。
 *
 *  ## 確かめること
 *   - 52a: **往復で戻る**。画素 → 世界 → NDC → 画素が、押した画素の中心に戻る。世界 → 画素 → 世界は
 *          半画素の内側に戻る(クリックの変換が描画の逆であること)
 *   - 52b: **1 単位の画素が縦横で同じ**(16:9 / 4:3 / 正方形 / 縦長 / 1920x1080 / 1024x768)
 *   - 52c: **縦の見える半寸法が画面の上下の端に来る**。横は窓の縦横比で広がる(Boid の壁 ±85 は内側)
 *   - 52d: **向き**。画面の上の画素は世界の上(+y)、左は世界の左(-x)。2-12 までは縦が逆だった。
 *          1280x720 で (840, 260) を押した点の値も、式から出した値と比べる
 *   - 52e: **壊れない**。幅・高さ 0、縦の半寸法が 0 / 負 / NaN / 無限でも、すべて有限
 *
 *  ## 入力を対称に寄せない (§4.2)
 *   - 窓の縦横は違う値。縦の半寸法(64)も、縦横比から出る横の見える半寸法(113.8)と違う
 *   - 世界の点・画素は中心から外し、縦と横で違う距離にする(縦横の取り違えが見える)
 *
 *  ## 押さえないもの
 *   - DX11 のビューポートの変換そのもの(D3D の決まり: x_px = (ndcX + 1) w / 2、y_px = (1 - ndcY) h / 2)。
 *     このテストはその決まりを式で置く。画面で合うかは T-ECS-53 / 54(撮って測る)が受け持つ
 *   - `RenderSystem` / `InteractionSystem` がこの型を使っていること(DX11 と Win32 の入力が要る)。
 *     コードで確かめ、画面で T-ECS-53 が確かめる
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cmath>
#include <limits>

#include "Graphics/ScreenMapping.h"

#include "TestHarness.h"

using GLFD::Graphics::ScreenMapping;
using GLFD::Graphics::ScreenMappingOf;
using GLFD::Graphics::WorldPoint;

namespace {

  /// Boid の壁の縦(出荷の `world.halfExtent.y`)。横(85)とは違う値
  constexpr float kHalfY = 64.0f;

  struct Size { int w, h; };
  /// 16:9 / 4:3 / 正方形 / 縦長 / 1920x1080 / 1024x768。正方形以外は縦横が違う
  constexpr Size kSizes[] = { { 1280, 720 }, { 960, 720 }, { 800, 800 }, { 720, 1280 }, { 1920, 1080 }, { 1024, 768 } };

  /// D3D のビューポート: NDC → 画素(連続値。画素 px は [px, px+1) を占める)
  float PixelX(const ScreenMapping& m, float ndcX) { return (ndcX + 1.0f) * m.width * 0.5f; }
  float PixelY(const ScreenMapping& m, float ndcY) { return (1.0f - ndcY) * m.height * 0.5f; }

  /// 1 単位が横に何画素か / 縦に何画素か
  float PxPerUnitX(const ScreenMapping& m) { return m.scaleX * m.width * 0.5f; }
  float PxPerUnitY(const ScreenMapping& m) { return m.scaleY * m.height * 0.5f; }

  bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

  // ===========================================================================
  // T-ECS-52a 往復
  // ===========================================================================
  void TestRoundTrip() {
    GLFD::Test::BeginCase("T-ECS-52a: pixel -> world -> pixel lands on the clicked pixel's centre; world -> pixel -> world within half a pixel");

    // 中心から外した画素。縦と横で違う距離
    const int pixels[][2] = { { 840, 260 }, { 101, 633 }, { 3, 2 }, { 517, 41 } };
    bool pixelBack = true;
    for (const Size& s : kSizes) {
      const ScreenMapping m = ScreenMappingOf(kHalfY, s.w, s.h);
      for (const auto& p : pixels) {
        const int px = p[0] % s.w;
        const int py = p[1] % s.h;
        const WorldPoint wp = m.PixelToWorld(px, py);
        pixelBack = pixelBack && Near(PixelX(m, m.NdcX(wp.x)), static_cast<float>(px) + 0.5f, 1e-3f)
                              && Near(PixelY(m, m.NdcY(wp.y)), static_cast<float>(py) + 0.5f, 1e-3f);
      }
    }
    CHECK(pixelBack);

    // 世界の点 → 描画の画素 → その画素を押す → 世界。戻りは半画素(を世界の単位にしたもの)の内側
    const float points[][2] = { { 37.25f, -21.5f }, { -80.0f, 50.0f }, { 3.5f, 61.0f }, { -110.0f, -7.75f } };
    bool worldBack = true;
    int  onScreen  = 0;
    for (const Size& s : kSizes) {
      const ScreenMapping m = ScreenMappingOf(kHalfY, s.w, s.h);
      const float halfPixelX = 0.5f / PxPerUnitX(m) + 1e-4f;
      const float halfPixelY = 0.5f / PxPerUnitY(m) + 1e-4f;
      for (const auto& q : points) {
        const float fx = PixelX(m, m.NdcX(q[0]));
        const float fy = PixelY(m, m.NdcY(q[1]));
        if (fx < 0.0f || fy < 0.0f || fx >= m.width || fy >= m.height) { continue; }   // 画面の外は押せない
        const WorldPoint back = m.PixelToWorld(static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy)));
        worldBack = worldBack && Near(back.x, q[0], halfPixelX) && Near(back.y, q[1], halfPixelY);
        ++onScreen;
      }
    }
    CHECK(worldBack);
    // 画面の外を飛ばすので、確かめた数も見る(0 件で通らないように)。横に見える半寸法は 64 x 幅 / 高さで、
    // 1280x720 と 1920x1080 は 4 点、960x720 と 1024x768 は 3 点、800x800 は 2 点、720x1280 は 1 点が画面の中
    CHECK(onScreen == 17);
  }

  // ===========================================================================
  // T-ECS-52b 縦横の画素
  // ===========================================================================
  void TestSamePixelsAcrossAndDown() {
    GLFD::Test::BeginCase("T-ECS-52b: one world unit is the same number of pixels across and down, for any window shape");

    bool same = true;
    for (const Size& s : kSizes) {
      const ScreenMapping m = ScreenMappingOf(kHalfY, s.w, s.h);
      same = same && Near(PxPerUnitX(m), PxPerUnitY(m), 1e-4f);
    }
    CHECK(same);

    // 1280 x 720: 1 単位 = 360 / 64 = 5.625 画素(縦横とも)
    const ScreenMapping hd = ScreenMappingOf(kHalfY, 1280, 720);
    CHECK(Near(PxPerUnitX(hd), 5.625f, 1e-4f));
    CHECK(Near(PxPerUnitY(hd), 5.625f, 1e-4f));
    // 2-12 までの値ではない: 横 1/85(横 7.53 画素、縦 5.63 画素で縦に潰れていた)
    CHECK(!Near(hd.scaleX, 1.0f / 85.0f, 1e-4f));
    // 縦横比はシェーダーに渡す値
    CHECK(Near(hd.aspect, 1280.0f / 720.0f, 1e-6f));
    const ScreenMapping tall = ScreenMappingOf(kHalfY, 720, 1280);
    CHECK(Near(tall.aspect, 720.0f / 1280.0f, 1e-6f));
  }

  // ===========================================================================
  // T-ECS-52c 端
  // ===========================================================================
  void TestEdges() {
    GLFD::Test::BeginCase("T-ECS-52c: the vertical half-extent sits on the top and bottom edges; across widens with the window");

    const ScreenMapping hd = ScreenMappingOf(kHalfY, 1280, 720);
    CHECK(Near(hd.NdcY(kHalfY), 1.0f, 1e-6f));
    CHECK(Near(hd.NdcY(-kHalfY), -1.0f, 1e-6f));
    // 横の端は 64 x 1280 / 720 = 113.8 単位。壁の横(85)はその内側(NDC 0.747)
    CHECK(Near(hd.NdcX(kHalfY * 1280.0f / 720.0f), 1.0f, 1e-5f));
    CHECK(Near(hd.NdcX(85.0f), 85.0f / (kHalfY * 1280.0f / 720.0f), 1e-5f));

    // 縦の半寸法を変えれば、縦の端もそれに付いて動く(F5 で halfExtent を変えた場合)
    const ScreenMapping other = ScreenMappingOf(40.0f, 1280, 720);
    CHECK(Near(other.NdcY(40.0f), 1.0f, 1e-6f));
    CHECK(Near(PxPerUnitX(other), PxPerUnitY(other), 1e-4f));

    // 縦長の窓でも縦が端(横は狭く見える)
    const ScreenMapping tall = ScreenMappingOf(kHalfY, 720, 1280);
    CHECK(Near(tall.NdcY(kHalfY), 1.0f, 1e-6f));
    CHECK(Near(tall.NdcX(kHalfY * 720.0f / 1280.0f), 1.0f, 1e-5f));
  }

  // ===========================================================================
  // T-ECS-52d 向き
  // ===========================================================================
  void TestDirections() {
    GLFD::Test::BeginCase("T-ECS-52d: the top of the screen is +y in the world and the left is -x (the click was mirrored up to 2-12)");

    const ScreenMapping hd = ScreenMappingOf(kHalfY, 1280, 720);
    const WorldPoint top    = hd.PixelToWorld(700, 0);
    const WorldPoint bottom = hd.PixelToWorld(700, 719);
    const WorldPoint left   = hd.PixelToWorld(0, 300);
    const WorldPoint right  = hd.PixelToWorld(1279, 300);
    CHECK(top.y > 63.0f);       // 上の端の画素は、ほぼ +64
    CHECK(bottom.y < -63.0f);   // 下の端の画素は、ほぼ -64
    CHECK(left.x < -113.0f);
    CHECK(right.x > 113.0f);

    // 1280 x 720 で (840, 260) を押す: 中心から右に 200.5 画素、上に 99.5 画素(画素の中心)。1 単位 = 5.625 画素
    const WorldPoint click = hd.PixelToWorld(840, 260);
    CHECK(Near(click.x, 200.5f / 5.625f, 1e-3f));    // 35.64
    CHECK(Near(click.y, 99.5f / 5.625f, 1e-3f));     // 17.69。2-12 までは -16.67(下に鏡)
  }

  // ===========================================================================
  // T-ECS-52e 壊れない
  // ===========================================================================
  bool AllFinite(const ScreenMapping& m) {
    const WorldPoint p = m.PixelToWorld(17, 9);
    return std::isfinite(m.scaleX) && std::isfinite(m.scaleY) && std::isfinite(m.aspect)
        && m.scaleX > 0.0f && m.scaleY > 0.0f && m.aspect > 0.0f
        && std::isfinite(p.x) && std::isfinite(p.y);
  }

  void TestDegenerate() {
    GLFD::Test::BeginCase("T-ECS-52e: zero-sized windows and a bad vertical half-extent stay finite");

    CHECK(AllFinite(ScreenMappingOf(kHalfY, 0, 0)));
    CHECK(AllFinite(ScreenMappingOf(kHalfY, 1280, 0)));
    CHECK(AllFinite(ScreenMappingOf(kHalfY, 0, 720)));
    CHECK(AllFinite(ScreenMappingOf(kHalfY, -5, -7)));
    CHECK(AllFinite(ScreenMappingOf(0.0f, 1280, 720)));
    CHECK(AllFinite(ScreenMappingOf(-64.0f, 1280, 720)));
    CHECK(AllFinite(ScreenMappingOf(std::numeric_limits<float>::quiet_NaN(), 1280, 720)));
    CHECK(AllFinite(ScreenMappingOf(std::numeric_limits<float>::infinity(), 1280, 720)));

    // 0 x 0 は 1 x 1 と同じ扱い
    const ScreenMapping zero = ScreenMappingOf(kHalfY, 0, 0);
    const ScreenMapping one  = ScreenMappingOf(kHalfY, 1, 1);
    CHECK(zero.scaleX == one.scaleX && zero.scaleY == one.scaleY && zero.aspect == one.aspect);
  }

}

int main() {
  GLFD::Test::BeginSuite("ScreenMapping (ECS 2-13)");
  TestRoundTrip();
  TestSamePixelsAcrossAndDown();
  TestEdges();
  TestDirections();
  TestDegenerate();
  return GLFD::Test::Summarize();
}
