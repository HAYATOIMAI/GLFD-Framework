/**
 * @file  EcsCollisionTests.cpp
 * @brief ECS 1-7: 衝突検出とグリッド — T-ECS-19 / T-ECS-20 / T-ECS-21
 *
 * @details
 *  ## 本番のシステムをそのまま呼ぶ
 *  `GridBuildSystem::Update` と `CollisionSystem::Update` を、実物の
 *  `GameContext` を組み立てて呼んでいる。**ロジックを写したテストは本番を
 *  一度も実行しない**ので、ここは実物を通す。ウィンドウと DX11 は使わない
 *  ので `nullptr` で足りる。
 *
 *  ## C4324 の抑制について
 *  `GameContext.h` は `JobSystem` / `LockFreeQueue` を引き込み、そこに
 *  `alignas` による意図的な詰め物がある(偽共有を避けるため)。C4324 は
 *  それを知らせるだけの警告で、**ゲーム本体の `/W3` では出ない**。
 *  テストの `/W4 /WX` だけが見る。
 *
 *  **抑制は `Tests/build_and_run.bat` の `EXTRA_FLAGS` に 1 箇所だけ置く。**
 *  ここに `#pragma warning` を重ねない。リンクするスレッド系の `.cpp` も
 *  同じ警告を出すので、**どのみちコマンドライン側でしか抑えられない**。
 *  同じ抑制を 2 箇所に置くと、片方を消したときに気づけない。
 *
 *  ## プロセスの終了について
 *  末尾で `std::_Exit` を使う。`JobSystem` の停止経路には取りこぼしがあり
 *  (要件書 §18.7)、デストラクタで固まることがある。**そのバグを消したのでは
 *  なく避けているだけ**で、スイートの結果は上で出し切っている。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "Core/GameContext.h"
#include "Game/GridBulidSystem.h"
#include "Physics/CollisionSystem.h"

#include "Core/StackAllocator.h"
#include "Core/StackResource.h"
#include "ECS/CommandBuffer.h"
#include "ECS/Registry.h"
#include "ECS/View.h"
#include "Events/EventBus.h"
#include "Events/Events.h"
#include "Physics/CollisionComponents.h"
#include "Physics/SpatialHashGrid.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::Components::Collider;
using GLFD::Components::Position;
using GLFD::Components::Velocity;
using GLFD::ECS::Entity;
using GLFD::Events::CollisionEvent;
using GLFD::Physics::SpatialHashGrid;
using GLFD::Test::MockMemoryResource;

namespace {

  // ===========================================================================
  // 実物の GameContext を組み立てる足場
  // ===========================================================================

  /**
   * @brief スイート全体で 1 本だけ使うジョブシステム
   *
   * @details
   *  **ケースごとに作らない。** `JobSystem` の停止経路には取りこぼしがあり
   *  (要件書 §18.7)、**デストラクタで固まる**。ケースごとに作ると毎回
   *  その賽を振ることになり、実際にこのスイートは最初それで止まった。
   *  1 本だけ作り、**破棄しない**(`main` の末尾は `std::_Exit`)。
   */
  GLFD::Thread::JobSystem* g_jobs = nullptr;

  /// ウィンドウも DX11 も要らない。**衝突に要るものだけ**を実物で用意する
  class Harness {
  public:
    Harness()
      : m_mainStack(16u * 1024u * 1024u)
      , m_frameStack(8u * 1024u * 1024u)
      , m_global(m_mainStack)
      , m_frame(m_frameStack)
      , m_registry(&m_global)
      , m_commands(&m_global)
      , m_eventBus(&m_global) {
      m_eventBus.Register<CollisionEvent>();
    }

    [[nodiscard]] GLFD::GameContext MakeContext() {
      return GLFD::GameContext{
          &m_global, &m_frame, g_jobs, &m_registry, &m_commands, &m_eventBus,
          nullptr,                 // grid: GridBuildSystem が差し込む
          nullptr, nullptr, nullptr,
          0.016f,
          nullptr, 0.0f, nullptr, nullptr };
    }

    [[nodiscard]] Entity Spawn(float x, float y, float radius) {
      const Entity e = m_registry.CreateEntity();
      if (!e.IsValid()) { return e; }
      if (m_registry.AddComponent<Position>(e, x, y, 0.0f, 0.0f) == nullptr) { return Entity::Invalid(); }
      if (m_registry.AddComponent<Velocity>(e, 0.0f, 0.0f, 0.0f, 0.0f) == nullptr) { return Entity::Invalid(); }
      if (m_registry.AddComponent<Collider>(e, radius) == nullptr) { return Entity::Invalid(); }
      return e;
    }

    GLFD::ECS::Registry&    Registry()  { return m_registry; }
    GLFD::Events::EventBus& EventBus()  { return m_eventBus; }

  private:
    GLFD::Memory::StackAllocator m_mainStack;
    GLFD::Memory::StackAllocator m_frameStack;
    GLFD::Memory::StackResource  m_global;
    GLFD::Memory::StackResource  m_frame;
    GLFD::ECS::Registry          m_registry;
    GLFD::ECS::CommandBuffer     m_commands;
    GLFD::Events::EventBus       m_eventBus;
  };

  /// 届いた衝突を集める購読者(1-7 の観測点と同じ形)
  struct CollisionLog {
    static constexpr std::size_t kMax = 256;
    CollisionEvent items[kMax]{};
    std::size_t    count      = 0;
    std::size_t    overflowed = 0;

    void Add(const CollisionEvent& e) {
      if (count < kMax) { items[count++] = e; }
      else              { ++overflowed; }
    }

    [[nodiscard]] std::size_t CountPair(Entity a, Entity b) const {
      std::size_t n = 0;
      for (std::size_t i = 0; i < count; ++i) {
        if ((items[i].entityA == a && items[i].entityB == b)
            || (items[i].entityA == b && items[i].entityB == a)) {
          ++n;
        }
      }
      return n;
    }

    [[nodiscard]] bool MentionsAnyOf(Entity e) const {
      for (std::size_t i = 0; i < count; ++i) {
        if (items[i].entityA == e || items[i].entityB == e) { return true; }
      }
      return false;
    }
  };

  // ===========================================================================
  // T-ECS-20 グリッドの正しさ
  // ===========================================================================

  void TestFreshGridIsEmpty() {
    GLFD::Test::BeginCase("T-ECS-20a: a freshly built grid returns nothing (the sentinel is right)");

    MockMemoryResource mock;
    SpatialHashGrid    grid(&mock, 64);
    CHECK(grid.IsReady());

    // **ここが ECS-0 1-4 の本体。** 以前は 0 埋めで、番兵は 0xFFFFFFFF だった。
    // つまり全バケットの末尾にエンティティ 0 がぶら下がって見えていた
    std::size_t visited = 0;
    for (float x = -5.0f; x <= 5.0f; x += 1.7f) {
      for (float y = -5.0f; y <= 5.0f; y += 1.7f) {
        grid.Query(Position{ x, y, 0.0f, 0.0f },
                   [&visited](std::uint32_t) { ++visited; return true; });
      }
    }
    CHECK(visited == 0u);
  }

  void TestInsertThenQueryFindsIt() {
    GLFD::Test::BeginCase("T-ECS-20b: what was inserted comes back, and Clear empties it");

    MockMemoryResource mock;
    SpatialHashGrid    grid(&mock, 64);
    CHECK(grid.IsReady());

    const Position here{ 1.0f, 2.0f, 0.0f, 0.0f };
    grid.Insert(7u, here);
    grid.Insert(9u, here);

    bool saw7 = false, saw9 = false;
    std::size_t visited = 0;
    grid.Query(here, [&](std::uint32_t id) {
      ++visited;
      if (id == 7u) { saw7 = true; }
      if (id == 9u) { saw9 = true; }
      return true;
    });
    CHECK(saw7);
    CHECK(saw9);
    CHECK(visited == 2u);

    grid.Clear();
    visited = 0;
    grid.Query(here, [&visited](std::uint32_t) { ++visited; return true; });
    CHECK(visited == 0u);
  }

  void TestTableSizeTracksTheEntityLimit() {
    GLFD::Test::BeginCase("T-ECS-20c: the bucket count stays tied to MaxEntities");

    // **1-7 の不具合の本体は「エンティティ数が足元で変わったのに表が
    // 取り残された」ことである。** 同じ関係をテストからも見る
    CHECK(SpatialHashGrid::TABLE_SIZE >= GLFD::ECS::MaxEntities);
    CHECK((SpatialHashGrid::TABLE_SIZE & SpatialHashGrid::TABLE_MASK) == 0u);
    CHECK(SpatialHashGrid::NULL_INDEX == 0xFFFFFFFFu);
  }

  void TestTheGridIsBuiltOnceAndSurvivesTheReaders() {
    GLFD::Test::BeginCase("T-ECS-20d: the grid is built once and the readers do not clear it");

    Harness harness;
    for (int i = 0; i < 16; ++i) {
      CHECK(harness.Spawn(static_cast<float>(i) * 0.3f, 0.0f, 0.3f).IsValid());
    }

    GLFD::GameContext ctx = harness.MakeContext();
    GLFD::Systems::GridBuildSystem::Update(ctx);
    CHECK(ctx.grid != nullptr);
    CHECK(ctx.grid->IsReady());

    // 組んだ直後は中身がある
    std::size_t before = 0;
    ctx.grid->Query(Position{ 0.0f, 0.0f, 0.0f, 0.0f },
                    [&before](std::uint32_t) { ++before; return true; });
    CHECK(before > 0u);

    // **読む側が消していないこと。** 以前は CollisionSystem が Clear() してから
    // 読んでいたので、ここが 0 になっていた(ECS-0 1-1)
    GLFD::Systems::CollisionSystem::Update(ctx);

    std::size_t after = 0;
    ctx.grid->Query(Position{ 0.0f, 0.0f, 0.0f, 0.0f },
                    [&after](std::uint32_t) { ++after; return true; });
    CHECK(after == before);
  }

  // ===========================================================================
  // T-ECS-19 衝突が実際に検出される(**中核**)
  // ===========================================================================

  void TestCollisionsAreDetectedAndDelivered() {
    GLFD::Test::BeginCase("T-ECS-19: overlapping pairs are detected, distant ones are not");

    Harness     harness;
    CollisionLog log;
    harness.EventBus().Subscribe<CollisionEvent>(
        [&log](const CollisionEvent& e) { log.Add(e); });

    // **既知の配置。** 半径 0.5 同士なので、距離 0.6 なら重なり、
    // 距離 20 なら 3x3x3 の近傍にも入らない
    const Entity a = harness.Spawn(0.0f, 0.0f, 0.5f);
    const Entity b = harness.Spawn(0.6f, 0.0f, 0.5f);
    const Entity farA = harness.Spawn(40.0f, 40.0f, 0.5f);
    const Entity farB = harness.Spawn(60.0f, 60.0f, 0.5f);
    CHECK(a.IsValid() && b.IsValid() && farA.IsValid() && farB.IsValid());

    GLFD::GameContext ctx = harness.MakeContext();
    GLFD::Systems::GridBuildSystem::Update(ctx);
    CHECK(ctx.grid != nullptr);
    GLFD::Systems::CollisionSystem::Update(ctx);
    harness.EventBus().DispatchAll();

    // **「1 件以上」では不十分。期待値と一致すること。**
    // 重なった組は**双方向**に出る(A が B を見つけ、B が A を見つける)
    CHECK(log.count == 2u);
    CHECK(log.CountPair(a, b) == 2u);

    // 離れた組は出ない(偽陽性が無いこと)
    CHECK(!log.MentionsAnyOf(farA));
    CHECK(!log.MentionsAnyOf(farB));
    CHECK(log.overflowed == 0u);

    // **本物の Entity が入っていること** (1-5 で Invalid() を解消した経路)
    bool everyHandleIsReal = true;
    for (std::size_t i = 0; i < log.count; ++i) {
      if (!harness.Registry().IsAlive(log.items[i].entityA)) { everyHandleIsReal = false; }
      if (!harness.Registry().IsAlive(log.items[i].entityB)) { everyHandleIsReal = false; }
      if (log.items[i].entityA == Entity::Invalid())         { everyHandleIsReal = false; }
      if (log.items[i].entityB == Entity::Invalid())         { everyHandleIsReal = false; }
    }
    CHECK(everyHandleIsReal);
  }

  void TestTouchingPairsJustOutOfRangeAreNotReported() {
    GLFD::Test::BeginCase("T-ECS-19b: a pair just outside the sum of the radii is not a collision");

    Harness     harness;
    CollisionLog log;
    harness.EventBus().Subscribe<CollisionEvent>(
        [&log](const CollisionEvent& e) { log.Add(e); });

    // 半径 0.25 同士 = 接触距離 0.5。**0.8 離す**(同じセルには居る)
    const Entity a = harness.Spawn(0.0f, 0.0f, 0.25f);
    const Entity b = harness.Spawn(0.8f, 0.0f, 0.25f);
    CHECK(a.IsValid() && b.IsValid());

    GLFD::GameContext ctx = harness.MakeContext();
    GLFD::Systems::GridBuildSystem::Update(ctx);
    GLFD::Systems::CollisionSystem::Update(ctx);
    harness.EventBus().DispatchAll();

    // **近傍として見えてはいるが、衝突ではない。**
    // 「近傍が返る = 衝突」になっていたらここで落ちる
    CHECK(log.count == 0u);
  }

  /// 溢れを**数えていること**。1-7 までここは黙って捨てていた (R-28)
  void TestTheChannelCountsWhatItDrops() {
    GLFD::Test::BeginCase("T-ECS-19c: the event channel counts what it publishes and drops");

    MockMemoryResource      mock;
    GLFD::Events::EventBus  bus(&mock);
    bus.Register<CollisionEvent>();

    std::size_t delivered = 0;
    bus.Subscribe<CollisionEvent>([&delivered](const CollisionEvent&) { ++delivered; });

    constexpr std::uint32_t kCapacity =
        static_cast<std::uint32_t>(GLFD::Events::EventChannel<CollisionEvent>::QUEUE_CAPACITY);
    constexpr std::uint32_t kExtra = 37u;

    for (std::uint32_t i = 0; i < kCapacity + kExtra; ++i) {
      bus.Publish<CollisionEvent>({ Entity::Make(i, 1u), Entity::Make(i + 1u, 1u), 1.0f });
    }

    const GLFD::Events::BusCounters counters = bus.Counters();
    CHECK(counters.published == kCapacity + kExtra);
    CHECK(counters.dropped == kExtra);            // **捨てた数がそのまま出ること**

    bus.DispatchAll();
    CHECK(delivered == kCapacity);                // 入った分だけ届く

    // 数え直せること(フレームの区切りで戻す)
    bus.ResetCounters();
    const GLFD::Events::BusCounters cleared = bus.Counters();
    CHECK(cleared.published == 0u);
    CHECK(cleared.dropped == 0u);
  }

  // ===========================================================================
  // T-ECS-21 自己スキップ
  // ===========================================================================

  void TestAnEntityNeverCollidesWithItself() {
    GLFD::Test::BeginCase("T-ECS-21: an entity is never reported as colliding with itself");

    Harness     harness;
    CollisionLog log;
    harness.EventBus().Subscribe<CollisionEvent>(
        [&log](const CollisionEvent& e) { log.Add(e); });

    // 1 体だけ。**自分しか近傍に居ない**
    const Entity alone = harness.Spawn(0.0f, 0.0f, 1.0f);
    CHECK(alone.IsValid());

    GLFD::GameContext ctx = harness.MakeContext();
    GLFD::Systems::GridBuildSystem::Update(ctx);
    GLFD::Systems::CollisionSystem::Update(ctx);
    harness.EventBus().DispatchAll();

    CHECK(log.count == 0u);

    // 密集させても自分自身は出ないこと
    for (int i = 0; i < 24; ++i) {
      CHECK(harness.Spawn(0.05f * static_cast<float>(i), 0.0f, 0.5f).IsValid());
    }
    GLFD::GameContext ctx2 = harness.MakeContext();
    GLFD::Systems::GridBuildSystem::Update(ctx2);
    GLFD::Systems::CollisionSystem::Update(ctx2);
    harness.EventBus().DispatchAll();

    CHECK(log.count > 0u);                 // 検算: そもそも検出できていること
    bool anySelfPair = false;
    for (std::size_t i = 0; i < log.count; ++i) {
      if (log.items[i].entityA == log.items[i].entityB) { anySelfPair = true; }
    }
    CHECK(!anySelfPair);
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsCollision (ECS 1-7)");

  // **意図的に解放しない。** 上の g_jobs の @details を参照
  GLFD::Thread::JobSystem jobs;
  g_jobs = &jobs;

  TestFreshGridIsEmpty();
  TestInsertThenQueryFindsIt();
  TestTableSizeTracksTheEntityLimit();
  TestTheGridIsBuiltOnceAndSurvivesTheReaders();

  TestCollisionsAreDetectedAndDelivered();
  TestTouchingPairsJustOutOfRangeAreNotReported();
  TestTheChannelCountsWhatItDrops();

  TestAnEntityNeverCollidesWithItself();

  const int code = GLFD::Test::Summarize();
  std::fflush(stdout);

  // **`return code` にしない。** JobSystem の停止経路に取りこぼしがあり
  // (要件書 §18.7)、デストラクタで固まることがある。結果は上で出し切って
  // いるので、ここで打ち切っても失われない。**避けているだけで直してはいない。**
  std::_Exit(code);
}
