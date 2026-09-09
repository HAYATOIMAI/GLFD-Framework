/**
 * @file  EcsViewTests.cpp
 * @brief ECS 1-5: `View<Ts...>` — T-ECS-8 / T-ECS-15 / T-ECS-16
 *
 * @details
 *  ## 中核は T-ECS-8
 *  **組み合わせを持つエンティティだけが返り、持たないものが混ざらない**こと。
 *  ここが崩れると VS / EDF 型で「弾に Health を書き込む」種類の壊れ方をする。
 *
 *  ## 基準プールの選択について
 *  当初「どのプールを基準にしたかは観測できない」と書いたが、**間違いだった**。
 *  `BaseSize()` が基準プールの大きさを返すので、**外から分かる**
 *  (変異 M2「常に先頭を基準にする」は T-ECS-8f がこれで捕まえた)。
 *  加えて T-ECS-8b が**型の並び順を変えても結果が一致する**ことを見ている。
 *
 *  ## 押さえられなかったもの
 *  **基準プールから成分を直接引いているかどうかは観測できない**(変異 M3)。
 *  疎配列経由で引いても結果は同じで、**変わるのは速度だけ**である。
 *  同じ理由で「逆引き表を控えているか」(変異 M8)も観測できない。
 *  どちらも `EcsBenchmark` が測る領分であって、テストの領分ではない。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "Core/StackAllocator.h"
#include "Core/StackResource.h"

#include "ECS/CommandBuffer.h"
#include "ECS/Entity.h"
#include "ECS/Registry.h"
#include "ECS/View.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::ECS::CommandBuffer;
using GLFD::ECS::Entity;
using GLFD::ECS::Registry;
using GLFD::Test::MockMemoryResource;

namespace {

  struct Pos    { float x = 0.0f, y = 0.0f; };
  struct Vel    { float vx = 0.0f, vy = 0.0f; };
  struct Health { int hp = 0; };
  struct Tag    { int id = 0; };

  /// 期待値を組み立てるための素朴な集合(テスト側でロジックを写さないための道具)
  struct EntitySet {
    static constexpr std::size_t kMax = 64;
    Entity      items[kMax]{};
    std::size_t count = 0;

    void Add(Entity e) {
      if (count < kMax) { items[count++] = e; }
    }
    [[nodiscard]] bool Contains(Entity e) const {
      for (std::size_t i = 0; i < count; ++i) {
        if (items[i] == e) { return true; }
      }
      return false;
    }
    [[nodiscard]] bool SameAs(const EntitySet& other) const {
      if (count != other.count) { return false; }
      for (std::size_t i = 0; i < count; ++i) {
        if (!other.Contains(items[i])) { return false; }
      }
      return true;
    }
  };

  // ===========================================================================
  // T-ECS-8 View の正しさ(**中核**)
  // ===========================================================================

  void TestViewReturnsOnlyTheEntitiesThatHaveEveryComponent() {
    GLFD::Test::BeginCase("T-ECS-8a: a view yields exactly the entities that have every component");

    MockMemoryResource mock;
    Registry           registry(&mock);

    // 4 種類の組み合わせを作る。**VS 型の敵 / 弾 / 経験値 / エフェクトの縮小版**
    EntitySet posOnly, posVel, posVelHealth, healthOnly;

    for (int i = 0; i < 5; ++i) {                       // Pos だけ
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, 1.0f, 1.0f) != nullptr);
      posOnly.Add(e);
    }
    for (int i = 0; i < 3; ++i) {                       // Pos + Vel
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, 2.0f, 2.0f) != nullptr);
      CHECK(registry.AddComponent<Vel>(e, 0.5f, 0.5f) != nullptr);
      posVel.Add(e);
    }
    for (int i = 0; i < 2; ++i) {                       // Pos + Vel + Health
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, 3.0f, 3.0f) != nullptr);
      CHECK(registry.AddComponent<Vel>(e, 1.5f, 1.5f) != nullptr);
      CHECK(registry.AddComponent<Health>(e, 10) != nullptr);
      posVelHealth.Add(e);
    }
    for (int i = 0; i < 4; ++i) {                       // Health だけ
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Health>(e, 1) != nullptr);
      healthOnly.Add(e);
    }

    // --- 単一: 全件返ること -------------------------------------------------
    {
      EntitySet seen;
      for (auto [entity, pos] : registry.View<Pos>()) {
        (void)pos;
        seen.Add(entity);
      }
      CHECK(seen.count == 10u);                          // 5 + 3 + 2
      CHECK(seen.Contains(posOnly.items[0]));
      CHECK(seen.Contains(posVel.items[0]));
      CHECK(seen.Contains(posVelHealth.items[0]));
      CHECK(!seen.Contains(healthOnly.items[0]));        // **混ざらないこと**
    }

    // --- 2 種: 両方持つものだけ ----------------------------------------------
    {
      EntitySet seen;
      for (auto [entity, pos, vel] : registry.View<Pos, Vel>()) {
        (void)pos; (void)vel;
        seen.Add(entity);
      }
      CHECK(seen.count == 5u);                           // 3 + 2
      for (std::size_t i = 0; i < posVel.count; ++i)       { CHECK(seen.Contains(posVel.items[i])); }
      for (std::size_t i = 0; i < posVelHealth.count; ++i) { CHECK(seen.Contains(posVelHealth.items[i])); }
      for (std::size_t i = 0; i < posOnly.count; ++i)      { CHECK(!seen.Contains(posOnly.items[i])); }
      for (std::size_t i = 0; i < healthOnly.count; ++i)   { CHECK(!seen.Contains(healthOnly.items[i])); }
    }

    // --- 3 種 ----------------------------------------------------------------
    {
      EntitySet seen;
      for (auto [entity, pos, vel, health] : registry.View<Pos, Vel, Health>()) {
        (void)pos; (void)vel; (void)health;
        seen.Add(entity);
      }
      CHECK(seen.SameAs(posVelHealth));
    }

    // --- 該当なし -------------------------------------------------------------
    {
      std::size_t n = 0;
      for (auto [entity, pos, tag] : registry.View<Pos, Tag>()) {
        (void)entity; (void)pos; (void)tag;
        ++n;
      }
      CHECK(n == 0u);
    }
  }

  /// 型の並び順を変えても結果が変わらないこと(= 基準の選び方が並び順に依存しない)
  void TestResultDoesNotDependOnTheOrderOfTheTypeList() {
    GLFD::Test::BeginCase("T-ECS-8b: the result does not depend on which pool is the smallest");

    MockMemoryResource mock;
    Registry           registry(&mock);

    // **わざと大きさを大きく違わせる。** Health が最小になる
    for (int i = 0; i < 20; ++i) {
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, 1.0f, 1.0f) != nullptr);
      if (i < 10) { CHECK(registry.AddComponent<Vel>(e, 1.0f, 1.0f) != nullptr); }
      if (i < 3)  { CHECK(registry.AddComponent<Health>(e, 5) != nullptr); }
    }

    EntitySet a, b, c;
    for (auto [e, p, v, h] : registry.View<Pos, Vel, Health>()) { (void)p; (void)v; (void)h; a.Add(e); }
    for (auto [e, h, p, v] : registry.View<Health, Pos, Vel>()) { (void)h; (void)p; (void)v; b.Add(e); }
    for (auto [e, v, h, p] : registry.View<Vel, Health, Pos>()) { (void)v; (void)h; (void)p; c.Add(e); }

    CHECK(a.count == 3u);
    CHECK(a.SameAs(b));
    CHECK(a.SameAs(c));

    // 2 種でも同じ
    EntitySet d, e2;
    for (auto [e, p, v] : registry.View<Pos, Vel>()) { (void)p; (void)v; d.Add(e); }
    for (auto [e, v, p] : registry.View<Vel, Pos>()) { (void)v; (void)p; e2.Add(e); }
    CHECK(d.count == 10u);
    CHECK(d.SameAs(e2));
  }

  /// 反復子が返す `Entity` が正しいこと。**1-4 で Invalid を入れた経緯があるので厳密に**
  void TestIteratorYieldsTheRightEntityAndReferences() {
    GLFD::Test::BeginCase("T-ECS-8c: the iterator yields the right entity and real references");

    MockMemoryResource mock;
    Registry           registry(&mock);

    Entity made[6];
    for (int i = 0; i < 6; ++i) {
      made[i] = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(made[i], static_cast<float>(i), 0.0f) != nullptr);
      CHECK(registry.AddComponent<Health>(made[i], i * 10) != nullptr);
    }

    bool everyOneMatched = true;
    std::size_t seen = 0;
    for (auto [entity, pos, health] : registry.View<Pos, Health>()) {
      // **返ってきた Entity で引き直したものと、参照が同じ実体を指すこと**
      const Pos* const    viaRegistryPos    = registry.GetComponent<Pos>(entity);
      const Health* const viaRegistryHealth = registry.GetComponent<Health>(entity);
      if (viaRegistryPos != &pos)       { everyOneMatched = false; }
      if (viaRegistryHealth != &health) { everyOneMatched = false; }
      if (!registry.IsAlive(entity))    { everyOneMatched = false; }
      if (entity == Entity::Invalid())  { everyOneMatched = false; }

      // 参照経由で書けること(値の往復)
      health.hp += 1;
      ++seen;
    }
    CHECK(everyOneMatched);
    CHECK(seen == 6u);

    bool everyWriteLanded = true;
    for (int i = 0; i < 6; ++i) {
      const Health* const h = registry.GetComponent<Health>(made[i]);
      if (h == nullptr || h->hp != i * 10 + 1) { everyWriteLanded = false; }
    }
    CHECK(everyWriteLanded);

    // `BaseEntities()` は「dense 添字 -> Entity」の直接引き (1-5 論点3)
    auto view = registry.View<Pos>();
    const Entity* const owners = view.BaseEntities();
    CHECK(owners != nullptr);
    bool everyIndexMatched = true;
    for (auto it = view.begin(); it != view.end(); ++it) {
      if (owners[it.BaseIndex()] != it.GetEntity()) { everyIndexMatched = false; }
    }
    CHECK(everyIndexMatched);
  }

  void TestEmptyViewIsSafe() {
    GLFD::Test::BeginCase("T-ECS-8d: an empty view iterates safely");

    MockMemoryResource mock;
    Registry           registry(&mock);

    // 1 度も使われていない型
    {
      auto view = registry.View<Tag>();
      CHECK(view.IsEmpty());
      CHECK(view.BaseSize() == 0u);
      CHECK(view.begin() == view.end());
      std::size_t n = 0;
      for (auto [e, tag] : view) { (void)e; (void)tag; ++n; }
      CHECK(n == 0u);
      // 空でも Slice は安全
      for (auto [e, tag] : view.Slice(0, 100)) { (void)e; (void)tag; ++n; }
      CHECK(n == 0u);
    }

    // 全部消したあと
    {
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, 1.0f, 1.0f) != nullptr);
      registry.DestroyEntity(e);

      std::size_t n = 0;
      for (auto [entity, pos] : registry.View<Pos>()) { (void)entity; (void)pos; ++n; }
      CHECK(n == 0u);
    }
  }

  /// 確保に失敗しても、View は空として安全に回ること (N-2)
  void TestViewSurvivesAllocationFailure() {
    GLFD::Test::BeginCase("T-ECS-8e: a view whose pool cannot be allocated is empty, not broken");

    bool everyRunSurvived = true;

    for (int failAfter = 0; failAfter < 12; ++failAfter) {
      MockMemoryResource mock;
      {
        Registry registry(&mock);
        const Entity e = registry.CreateEntity();
        (void)registry.AddComponent<Pos>(e, 1.0f, 1.0f);

        mock.SetFailAfter(failAfter);

        std::size_t n = 0;
        for (auto [entity, pos, vel] : registry.View<Pos, Vel>()) {
          (void)entity; (void)pos; (void)vel;
          ++n;
        }
        // 中身は確保できたかによるが、**クラッシュせず、件数が実体と矛盾しない**こと
        if (n > 1u) { everyRunSurvived = false; }

        mock.ClearFailure();
      }
      if (mock.DoubleFree() || mock.UnknownFree() || mock.LiveBytes() != 0u) {
        everyRunSurvived = false;
      }
    }
    CHECK(everyRunSurvived);
  }

  /**
   * @brief **別の View の dense 添字を混ぜてはならない**ことを固定する
   *
   * @details
   *  `SpatialHashGrid` には「グリッドを組んだ View の基準プールの dense 添字」が
   *  入る。近傍探索の自己スキップを `i == neighborId` と書くと、
   *  **反復側の添字(別の View の基準プール)とグリッドの添字を比べる**ことに
   *  なり、自分を飛ばし損ねて無関係な他人を飛ばす。
   *
   *  症状が出にくいのが厄介で、群れの挙動が少し変わるだけで落ちもしない。
   *  **だから添字ではなく `Entity` で比べる。** ここはその根拠を固定する。
   */
  void TestDenseIndicesFromDifferentViewsAreNotInterchangeable() {
    GLFD::Test::BeginCase("T-ECS-8f: dense indices from two different views are not the same");

    MockMemoryResource mock;
    Registry           registry(&mock);

    // Position は全員、Velocity は後半だけ。**2 つのプールの並びがずれる**
    Entity made[8];
    for (int i = 0; i < 8; ++i) {
      made[i] = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(made[i], static_cast<float>(i), 0.0f) != nullptr);
      if (i >= 4) { CHECK(registry.AddComponent<Vel>(made[i], 1.0f, 1.0f) != nullptr); }
    }

    auto posView   = registry.View<Pos>();          // グリッドの基準になる側
    auto pairView  = registry.View<Pos, Vel>();     // システムが反復する側

    // 基準プールが違う(Vel の方が小さいので pairView は Vel を基準に選ぶ)
    CHECK(posView.BaseSize() == 8u);
    CHECK(pairView.BaseSize() == 4u);

    const Entity* const posOwners = posView.BaseEntities();
    CHECK(posOwners != nullptr);

    // **同じエンティティが 2 つの View で違う添字を持つ**ことを見る
    bool foundADisagreement = false;
    for (auto it = pairView.begin(); it != pairView.end(); ++it) {
      const Entity   entity    = it.GetEntity();
      const std::size_t pairIx = it.BaseIndex();

      // posView 側で同じエンティティが何番目か
      std::size_t posIx = posView.BaseSize();
      for (std::size_t k = 0; k < posView.BaseSize(); ++k) {
        if (posOwners[k] == entity) { posIx = k; break; }
      }
      CHECK(posIx < posView.BaseSize());          // 必ず居る

      if (posIx != pairIx) { foundADisagreement = true; }
    }
    // **これが真であることが、添字で比べてはいけない理由そのもの**
    CHECK(foundADisagreement);

    // `Entity` で比べれば正しい。添字で比べると別人になる
    {
      auto        it     = pairView.begin();
      const Entity self  = it.GetEntity();
      const std::size_t selfIx = it.BaseIndex();

      // グリッドが返す添字(= posView の添字)で「自分」を判定したつもりになる
      const Entity whoTheIndexPointsAt = posOwners[selfIx];
      CHECK(whoTheIndexPointsAt != self);          // **別人である**
      CHECK(posOwners[4] == self);                 // 本当の自分は別の位置に居る
    }
  }

  // ===========================================================================
  // T-ECS-15 構造変更の検出 (R-24)
  // ===========================================================================

  void TestStructuralChangeDuringIterationIsDetected() {
    GLFD::Test::BeginCase("T-ECS-15a: changing the shape while a view is alive is detected");

    MockMemoryResource mock;
    Registry           registry(&mock);

    Entity made[4];
    for (int i = 0; i < 4; ++i) {
      made[i] = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(made[i], 1.0f, 1.0f) != nullptr);
    }

    // **`assert` ではなく公開した判定で見る** (1-4 の IsOwnerThread と同じ形)
    {
      auto view = registry.View<Pos>();
      CHECK(!view.IsStale());

      CHECK(registry.AddComponent<Health>(made[0], 1) != nullptr);
      CHECK(view.IsStale());                       // 成分を足した = dense が伸びた
    }
    {
      auto view = registry.View<Pos>();
      registry.RemoveComponent<Health>(made[0]);
      CHECK(view.IsStale());
    }
    {
      auto view = registry.View<Pos>();
      registry.DestroyEntity(made[3]);
      CHECK(view.IsStale());
    }

    // 失敗した追加は構造を動かさない
    {
      auto view = registry.View<Pos>();
      CHECK(registry.AddComponent<Pos>(made[0], 1.0f, 1.0f) == nullptr);   // 既に持っている
      CHECK(!view.IsStale());
    }
  }

  void TestCreateEntityDoesNotInvalidateAView() {
    GLFD::Test::BeginCase("T-ECS-15b: CreateEntity does not make a view stale (1-4 relies on it)");

    MockMemoryResource mock;
    Registry           registry(&mock);

    for (int i = 0; i < 4; ++i) {
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, 1.0f, 1.0f) != nullptr);
    }

    auto view = registry.View<Pos>();
    const std::uint32_t before = registry.StructureVersion();

    for (int i = 0; i < 8; ++i) {
      const Entity fresh = registry.CreateEntity();
      CHECK(fresh.IsValid());
    }
    // **生成はどのプールの dense も動かさない。** ここで stale になると
    // 「反復しながら生成してバッファへ積む」(T-ECS-4) が通らなくなる
    CHECK(registry.StructureVersion() == before);
    CHECK(!view.IsStale());
  }

  void TestQueuingThroughTheCommandBufferKeepsTheViewFresh() {
    GLFD::Test::BeginCase("T-ECS-15c: queuing through a command buffer does not disturb iteration");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    Entity made[6];
    for (int i = 0; i < 6; ++i) {
      made[i] = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(made[i], static_cast<float>(i), 0.0f) != nullptr);
    }

    auto view = registry.View<Pos>();
    std::size_t visited = 0;
    bool stayedFreshThroughout = true;

    for (auto [entity, pos] : view) {
      (void)pos;
      // **これが正しい使い方。** 反復しながら積む
      CHECK(commands.Destroy(entity));
      const Entity spawned = registry.CreateEntity();
      CHECK(commands.Add(spawned, Health{ 1 }));
      if (view.IsStale()) { stayedFreshThroughout = false; }
      ++visited;
    }

    CHECK(visited == 6u);
    CHECK(stayedFreshThroughout);      // 積んだだけでは構造は動いていない

    // 適用すると構造が変わる。**反復の外なのでそれでよい**
    registry.ApplyCommands(commands);
    CHECK(view.IsStale());
    CHECK(registry.AliveCount() == 6u);
  }

  // ===========================================================================
  // T-ECS-16 範囲分割
  // ===========================================================================

  void TestSlicesCoverExactlyTheSameSet() {
    GLFD::Test::BeginCase("T-ECS-16a: slices cover the same set as a single pass");

    MockMemoryResource mock;
    Registry           registry(&mock);

    for (int i = 0; i < 37; ++i) {                    // 割り切れない数を選ぶ
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, static_cast<float>(i), 0.0f) != nullptr);
      if (i % 3 != 0) { CHECK(registry.AddComponent<Vel>(e, 1.0f, 1.0f) != nullptr); }
    }

    EntitySet single;
    for (auto [e, p, v] : registry.View<Pos, Vel>()) { (void)p; (void)v; single.Add(e); }

    for (std::size_t threads = 1; threads <= 8; ++threads) {
      auto              view  = registry.View<Pos, Vel>();
      const std::size_t count = view.BaseSize();
      const std::size_t batch = count / threads;

      EntitySet sliced;
      for (std::size_t t = 0; t < threads; ++t) {
        const std::size_t start = t * batch;
        const std::size_t stop  = (t == threads - 1) ? count : start + batch;
        for (auto [e, p, v] : view.Slice(start, stop)) { (void)p; (void)v; sliced.Add(e); }
      }
      CHECK(sliced.SameAs(single));
    }
  }

  void TestSliceEdges() {
    GLFD::Test::BeginCase("T-ECS-16b: more slices than elements, and no elements at all");

    MockMemoryResource mock;
    Registry           registry(&mock);

    // 要素 3 個に対して 8 分割
    for (int i = 0; i < 3; ++i) {
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, 1.0f, 1.0f) != nullptr);
    }
    {
      auto              view  = registry.View<Pos>();
      const std::size_t count = view.BaseSize();
      const std::size_t batch = count / 8;              // 0 になる
      std::size_t       total = 0;
      for (std::size_t t = 0; t < 8; ++t) {
        const std::size_t start = t * batch;
        const std::size_t stop  = (t == 7) ? count : start + batch;
        for (auto [e, p] : view.Slice(start, stop)) { (void)e; (void)p; ++total; }
      }
      CHECK(total == 3u);

      // 範囲外や逆転を渡しても安全に空になる
      CHECK(view.Slice(100, 200).IsEmpty());
      CHECK(view.Slice(2, 1).IsEmpty());
      CHECK(view.Slice(0, 0).IsEmpty());
      // **切ったものをさらに切るとき、外側の開始位置が効くこと。**
      // ここを `Slice(0, ...)` から始めると、相対でも絶対でも同じ答えになり、
      // 「相対位置を無視する」変異を見逃す(1-5 の変異 M4 で実際に見逃した)
      {
        auto inner = view.Slice(1, 3).Slice(1, 2);   // 元の添字 2 の 1 件
        CHECK(inner.BaseSize() == 1u);
        auto it = inner.begin();
        CHECK(it != inner.end());
        CHECK(it.BaseIndex() == 2u);
      }
      CHECK(view.Slice(0, 3).Slice(1, 2).BaseSize() == 1u);
    }

    // 要素 0 個
    {
      auto view = registry.View<Tag>();
      std::size_t total = 0;
      for (std::size_t t = 0; t < 4; ++t) {
        for (auto [e, tag] : view.Slice(t, t + 1)) { (void)e; (void)tag; ++total; }
      }
      CHECK(total == 0u);
    }
  }

  void TestParallelSlicesMatchTheSingleThreadedResult() {
    GLFD::Test::BeginCase("T-ECS-16c: running slices on threads gives the single-threaded result");

    MockMemoryResource mock;
    Registry           registry(&mock);

    constexpr int kCount = 400;
    for (int i = 0; i < kCount; ++i) {
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Pos>(e, static_cast<float>(i), 0.0f) != nullptr);
      if (i % 2 == 0) { CHECK(registry.AddComponent<Vel>(e, 2.0f, 0.0f) != nullptr); }
    }

    // 単一スレッドの答え
    double expected = 0.0;
    for (auto [e, p, v] : registry.View<Pos, Vel>()) { (void)e; expected += p.x + v.vx; }

    auto              view    = registry.View<Pos, Vel>();
    const std::size_t count   = view.BaseSize();
    constexpr std::size_t kThreads = 4;
    const std::size_t batch   = count / kThreads;

    double partial[kThreads] = {};
    {
      std::thread workers[kThreads];
      for (std::size_t t = 0; t < kThreads; ++t) {
        const std::size_t start = t * batch;
        const std::size_t stop  = (t == kThreads - 1) ? count : start + batch;
        // **View は値でコピーして投げられること**(§2.2)
        workers[t] = std::thread([view, start, stop, &partial, t]() {
          double sum = 0.0;
          for (auto [e, p, v] : view.Slice(start, stop)) { (void)e; sum += p.x + v.vx; }
          partial[t] = sum;
        });
      }
      for (std::size_t t = 0; t < kThreads; ++t) { workers[t].join(); }
    }

    double got = 0.0;
    for (std::size_t t = 0; t < kThreads; ++t) { got += partial[t]; }

    CHECK(got == expected);
    CHECK(expected > 0.0);          // 検算: そもそも仕事をしていること
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsView (ECS 1-5)");

  TestViewReturnsOnlyTheEntitiesThatHaveEveryComponent();
  TestResultDoesNotDependOnTheOrderOfTheTypeList();
  TestIteratorYieldsTheRightEntityAndReferences();
  TestEmptyViewIsSafe();
  TestViewSurvivesAllocationFailure();
  TestDenseIndicesFromDifferentViewsAreNotInterchangeable();

  TestStructuralChangeDuringIterationIsDetected();
  TestCreateEntityDoesNotInvalidateAView();
  TestQueuingThroughTheCommandBufferKeepsTheViewFresh();

  TestSlicesCoverExactlyTheSameSet();
  TestSliceEdges();
  TestParallelSlicesMatchTheSingleThreadedResult();

  return GLFD::Test::Summarize();
}
