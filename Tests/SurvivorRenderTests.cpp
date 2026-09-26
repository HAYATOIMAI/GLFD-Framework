/**
 * @file  SurvivorRenderTests.cpp
 * @brief T-ECS-38: 描画に渡すデータ (ECS 2-3)
 *
 * @details
 *  **描画そのものはテストできないが、描画に渡すデータは検証できる。**
 *  シーンが呼ぶのと同じ `Game::BuildSurvivorVertices`(`Game/SurvivorRender.h`)を呼ぶ
 *  (手順を写さない。開発手法 §4.7)。
 *
 *  ## 確かめること
 *   - 38a: **種類ごとの色**が頂点に入る。位置は `ワールド × 縮尺`。プレイヤーは原点に 1 点。
 *          種類の成分を持たない `Position` だけのエンティティと、`Position` を持たない
 *          `Health` だけのエンティティは描かない。**描く順**(後が上)
 *   - 38b: **頂点の数**が描く対象の数と一致する。破棄・生成(添字の再利用)・成分の取り外しで
 *          増減しても、消えたものは描かず、残ったものは正しい色で描く
 *   - 38c: **確保に失敗したとき**、黙らず `VertexBufferUnavailable` と要求した数を返し、
 *          配列は空で、何も残さない。確保できるようになれば元に戻る
 *
 *  1-6 の診断の**行**(状態の変わり目だけ出ること)は、`Logger` を引き込むので
 *  別のスイート(`RenderDiagnosticsTests`)で見る。
 *
 *  ## 入力を原点・先頭一致に寄せない (§4.2)
 *   - 位置は原点から離し、正負の両側に散らす(プレイヤーだけが原点)
 *   - 種類を**交互に**作る。どのプールの dense の並びも、生成の順とも他のプールとも一致しない
 *   - 破棄は**途中の**エンティティから行い、空いた添字を**別の種類**で再利用する
 *
 *  ## 押さえないもの
 *   - GPU に渡った後(シェーダー・合成・画面の見え方)。目視 (T-ECS-40) が受け持つ
 *   - `DrawPoints` の呼び出し。シーンは組んだ配列をそのまま渡すだけ
 *
 *  @note このスイートは `d3d11.h` を引き込まない(T-ECS-39 で `/showIncludes` を確かめる)。
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Core/DynamicArray.h"
#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "ECS/Registry.h"
#include "Game/SurvivorComponents.h"
#include "Game/SurvivorRender.h"
#include "Graphics/RenderStatus.h"
#include "Graphics/SimpleVertex.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::Components::Damage;
using GLFD::Components::Health;
using GLFD::Components::Pickup;
using GLFD::Components::Position;
using GLFD::ECS::Entity;
using GLFD::ECS::Registry;
using GLFD::Game::SurvivorColor;
using GLFD::Graphics::SimpleVertex;
using GLFD::Systems::RenderStatus;
using GLFD::Test::MockMemoryResource;

namespace {

  enum class Kind : std::uint8_t { Player, Enemy, Bullet, Pickup, Unknown };

  /// 描く順(後が上)。**この表が仕様**。種類の並びを変えるときはここを変える
  constexpr Kind kDrawOrder[] = { Kind::Player, Kind::Enemy, Kind::Bullet, Kind::Pickup };

  int OrderOf(Kind k) {
    for (int i = 0; i < 4; ++i) {
      if (kDrawOrder[i] == k) { return i; }
    }
    return -1;
  }

  bool SameColor(const DirectX::XMFLOAT4& c, const SurvivorColor& expected) {
    return c.x == expected.r && c.y == expected.g && c.z == expected.b;
  }

  Kind KindOfColor(const DirectX::XMFLOAT4& c) {
    if (SameColor(c, GLFD::Game::kSurvivorPlayerColor)) { return Kind::Player; }
    if (SameColor(c, GLFD::Game::kSurvivorEnemyColor))  { return Kind::Enemy; }
    if (SameColor(c, GLFD::Game::kSurvivorBulletColor)) { return Kind::Bullet; }
    if (SameColor(c, GLFD::Game::kSurvivorPickupColor)) { return Kind::Pickup; }
    return Kind::Unknown;
  }

  /// 置いたもの。`alive` が false のものは描かれてはならない
  struct Placed {
    Entity entity;
    Kind   kind;
    float  x, y;
    bool   alive;
  };

  constexpr std::size_t kMaxPlaced = 32;

  struct World {
    MockMemoryResource mock;
    Registry           registry{ &mock };
    Placed             placed[kMaxPlaced]{};
    std::size_t        count = 0;

    /// 種類の成分と位置を付けて置く。作れなければ false
    bool Place(Kind kind, float x, float y) {
      if (count >= kMaxPlaced) { return false; }
      const Entity e = registry.CreateEntity();
      if (registry.AddComponent<Position>(e, x, y, 0.0f, 0.0f) == nullptr) { return false; }
      bool ok = false;
      switch (kind) {
        case Kind::Enemy:  ok = registry.AddComponent<Health>(e, 2.0f) != nullptr; break;
        case Kind::Bullet: ok = registry.AddComponent<Damage>(e, 1.0f) != nullptr; break;
        case Kind::Pickup: ok = registry.AddComponent<Pickup>(e, 1.0f) != nullptr; break;
        default:           ok = true; break;   // Unknown: 位置だけ
      }
      placed[count++] = Placed{ e, kind, x, y, kind != Kind::Unknown };
      return ok;
    }

    std::size_t AliveDrawable() const {
      std::size_t n = 0;
      for (std::size_t i = 0; i < count; ++i) { if (placed[i].alive) { ++n; } }
      return n;
    }
  };

  /// 原点から離れた、正負に散った位置。i ごとに違い、どれも 0 にならない
  float XAt(int i) { return -20.5f + 3.25f * static_cast<float>(i); }
  float YAt(int i) { return 13.75f - 2.5f * static_cast<float>(i); }

  bool Near(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

  /// ワールド位置 (x, y) を描いた頂点の数
  std::size_t VerticesAt(const GLFD::DynamicArray<SimpleVertex>& v, float x, float y) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < v.GetSize(); ++i) {
      if (Near(v[i].Pos.x, x * GLFD::Game::kSurvivorScaleX) && Near(v[i].Pos.y, y * GLFD::Game::kSurvivorScaleY)) { ++n; }
    }
    return n;
  }

  /// ワールド位置 (x, y) を描いた最初の頂点の添字。無ければ -1
  long long IndexAt(const GLFD::DynamicArray<SimpleVertex>& v, float x, float y) {
    for (std::size_t i = 0; i < v.GetSize(); ++i) {
      if (Near(v[i].Pos.x, x * GLFD::Game::kSurvivorScaleX) && Near(v[i].Pos.y, y * GLFD::Game::kSurvivorScaleY)) {
        return static_cast<long long>(i);
      }
    }
    return -1;
  }

  /**
   * @brief 組んだ頂点が、置いたものとちょうど対応しているかを全部見る
   * @details 生きているものは**ちょうど 1 つ**の頂点で、種類の色で描かれている。
   *          死んだもの・種類の無いものは描かれていない。プレイヤーは原点にちょうど 1 つ。
   *          種類の並びが `kDrawOrder` のとおり。頂点の数 = 生きているもの + 1
   */
  void CheckMatchesWorld(const World& w, const GLFD::DynamicArray<SimpleVertex>& v, const RenderStatus& status) {
    CHECK(status.outcome == RenderStatus::Outcome::Drawn);
    CHECK(v.GetSize() == w.AliveDrawable() + 1u);
    CHECK(status.requestedVertices == v.GetSize());

    bool eachAliveOnceInItsColor = true;
    bool noDeadOrUntyped         = true;
    for (std::size_t i = 0; i < w.count; ++i) {
      const Placed& p = w.placed[i];
      const std::size_t n = VerticesAt(v, p.x, p.y);
      if (p.alive) {
        const long long at = IndexAt(v, p.x, p.y);
        eachAliveOnceInItsColor = eachAliveOnceInItsColor && n == 1u && at >= 0
                               && KindOfColor(v[static_cast<std::size_t>(at)].Color) == p.kind;
      } else {
        noDeadOrUntyped = noDeadOrUntyped && n == 0u;
      }
    }
    CHECK(eachAliveOnceInItsColor);
    CHECK(noDeadOrUntyped);

    // プレイヤー: 原点にちょうど 1 つ、白
    std::size_t players = 0;
    for (std::size_t i = 0; i < v.GetSize(); ++i) {
      if (KindOfColor(v[i].Color) == Kind::Player) {
        ++players;
        CHECK_QUIET(v[i].Pos.x == 0.0f && v[i].Pos.y == 0.0f);
      }
    }
    CHECK(players == 1u);

    // 描く順: 種類の順位が下がらない。知らない色も無い
    bool ordered = true;
    bool known   = true;
    int  last    = -1;
    for (std::size_t i = 0; i < v.GetSize(); ++i) {
      const Kind k = KindOfColor(v[i].Color);
      if (k == Kind::Unknown) { known = false; continue; }
      const int o = OrderOf(k);
      if (o < last) { ordered = false; }
      last = o;
    }
    CHECK(known);
    CHECK(ordered);

    // 使っていない成分: z は 0、w と α は 1
    bool unusedFields = true;
    for (std::size_t i = 0; i < v.GetSize(); ++i) {
      unusedFields = unusedFields && v[i].Pos.z == 0.0f && v[i].Pos.w == 1.0f && v[i].Color.w == 1.0f;
    }
    CHECK(unusedFields);
  }

  /// 12 体を種類交互に置き、成分の組み合わせで種類が決まることを試す囮を 2 つ足す
  bool Populate(World& w) {
    const Kind pattern[] = { Kind::Enemy, Kind::Pickup, Kind::Bullet, Kind::Bullet, Kind::Enemy, Kind::Pickup,
                             Kind::Enemy, Kind::Bullet, Kind::Pickup, Kind::Enemy, Kind::Enemy, Kind::Bullet };
    bool ok = true;
    int  i  = 0;
    for (Kind k : pattern) { ok = w.Place(k, XAt(i), YAt(i)) && ok; ++i; }
    ok = w.Place(Kind::Unknown, XAt(i), YAt(i)) && ok;   // 位置だけ: 描かない
    ++i;
    // Health だけで位置が無い: 描かない(描く対象は Position との組)
    const Entity noPos = w.registry.CreateEntity();
    ok = (w.registry.AddComponent<Health>(noPos, 2.0f) != nullptr) && ok;
    return ok;
  }

  // ===========================================================================
  // T-ECS-38a 種類ごとの色・位置・描く順
  // ===========================================================================
  void TestColorsPositionsAndOrder() {
    GLFD::Test::BeginCase("T-ECS-38a: each kind is drawn once, in its colour, at its position, in the draw order");

    World w;
    CHECK(Populate(w));

    MockMemoryResource frame;
    GLFD::DynamicArray<SimpleVertex> v(&frame);
    const RenderStatus status = GLFD::Game::BuildSurvivorVertices(w.registry, v);
    CheckMatchesWorld(w, v, status);
    CHECK(v.GetSize() == 13u);   // 12 体 + プレイヤー(囮 2 つは描かない)

    // 4 色が互いに違う(同じ色が 2 種類に付いていれば、上の対応づけが意味を失う)
    CHECK(!SameColor(DirectX::XMFLOAT4(GLFD::Game::kSurvivorEnemyColor.r, GLFD::Game::kSurvivorEnemyColor.g,
                                       GLFD::Game::kSurvivorEnemyColor.b, 1.0f), GLFD::Game::kSurvivorBulletColor));
    CHECK(!SameColor(DirectX::XMFLOAT4(GLFD::Game::kSurvivorBulletColor.r, GLFD::Game::kSurvivorBulletColor.g,
                                       GLFD::Game::kSurvivorBulletColor.b, 1.0f), GLFD::Game::kSurvivorPickupColor));
    CHECK(!SameColor(DirectX::XMFLOAT4(GLFD::Game::kSurvivorPickupColor.r, GLFD::Game::kSurvivorPickupColor.g,
                                       GLFD::Game::kSurvivorPickupColor.b, 1.0f), GLFD::Game::kSurvivorEnemyColor));
  }

  // ===========================================================================
  // T-ECS-38b 生成・破棄・成分の取り外しで数が追従する
  // ===========================================================================
  void TestCountFollowsTheWorld() {
    GLFD::Test::BeginCase("T-ECS-38b: the vertex count follows creation, destruction, index reuse and component removal");

    World w;
    CHECK(Populate(w));
    MockMemoryResource frame;

    {
      GLFD::DynamicArray<SimpleVertex> v(&frame);
      CheckMatchesWorld(w, v, GLFD::Game::BuildSurvivorVertices(w.registry, v));
    }

    // 途中のものを破棄する(敵 2・弾 1・経験値 1)。先頭と末尾は残す
    const std::size_t victims[] = { 4, 2, 8, 6 };   // Enemy, Bullet, Pickup, Enemy
    for (std::size_t idx : victims) {
      w.registry.DestroyEntity(w.placed[idx].entity);
      w.placed[idx].alive = false;
    }
    {
      GLFD::DynamicArray<SimpleVertex> v(&frame);
      const RenderStatus s = GLFD::Game::BuildSurvivorVertices(w.registry, v);
      CheckMatchesWorld(w, v, s);
      CHECK(v.GetSize() == 9u);
    }

    // 空いた添字を**別の種類で**再利用する。新しい位置に置く
    CHECK(w.Place(Kind::Pickup, XAt(20), YAt(20)));
    CHECK(w.Place(Kind::Bullet, XAt(21), YAt(21)));
    CHECK(w.Place(Kind::Enemy,  XAt(22), YAt(22)));
    {
      GLFD::DynamicArray<SimpleVertex> v(&frame);
      const RenderStatus s = GLFD::Game::BuildSurvivorVertices(w.registry, v);
      CheckMatchesWorld(w, v, s);
      CHECK(v.GetSize() == 12u);
    }

    // 敵から Health を外す: 位置はあるが種類が無いので、描かない
    w.registry.RemoveComponent<Health>(w.placed[0].entity);
    w.placed[0].alive = false;
    {
      GLFD::DynamicArray<SimpleVertex> v(&frame);
      const RenderStatus s = GLFD::Game::BuildSurvivorVertices(w.registry, v);
      CheckMatchesWorld(w, v, s);
      CHECK(v.GetSize() == 11u);
    }
    CHECK(frame.LiveBytes() == 0u);   // 配列は抜けるたびに返している
  }

  // ===========================================================================
  // T-ECS-38c 確保失敗
  // ===========================================================================
  void TestAllocationFailure() {
    GLFD::Test::BeginCase("T-ECS-38c: a vertex array that cannot be sized is reported, left empty, and recovers");

    World w;
    CHECK(Populate(w));

    MockMemoryResource frame;
    frame.SetFailAfter(0);
    {
      GLFD::DynamicArray<SimpleVertex> v(&frame);
      const RenderStatus s = GLFD::Game::BuildSurvivorVertices(w.registry, v);
      CHECK(s.outcome == RenderStatus::Outcome::VertexBufferUnavailable);
      // 要求した数は、描くはずだった数(13)以上。基準プールの範囲なので囮の分だけ多いことがある
      CHECK(s.requestedVertices >= 13u);
      CHECK(v.GetSize() == 0u);
      CHECK(frame.InjectedFailures() >= 1);
    }
    CHECK(frame.LiveBytes() == 0u);

    // 確保できるようになれば、同じ世界がそのまま描ける
    frame.ClearFailure();
    {
      GLFD::DynamicArray<SimpleVertex> v(&frame);
      CheckMatchesWorld(w, v, GLFD::Game::BuildSurvivorVertices(w.registry, v));
    }
    CHECK(frame.LiveBytes() == 0u);
  }

}

int main() {
  GLFD::Test::BeginSuite("SurvivorRender (ECS 2-3)");
  TestColorsPositionsAndOrder();
  TestCountFollowsTheWorld();
  TestAllocationFailure();
  return GLFD::Test::Summarize();
}
