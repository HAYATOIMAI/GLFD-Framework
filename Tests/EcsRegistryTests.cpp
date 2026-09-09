/**
 * @file  EcsRegistryTests.cpp
 * @brief ECS 1-1: `Entity` の 64bit 化と `Registry` の生存管理
 *        — T-ECS-1 / 2 / 3 / 5 と、config 由来の上限
 *
 * @details
 *  `EcsEntityLimit`(ECS-0c)を統合した。上限と `AliveCount()` は同じ不変条件の
 *  表と裏なので、別のスイートに置くと片方だけ見ることになる。
 *
 *  ## ECS-0c から引き継いだ「押さえられなかったもの」
 *  旧スイートには次の記録があった:
 *
 *  > `CreateEntity` を「増やしてから判定する」形に変える変異は**落ちなかった**。
 *  > 上限後も NullEntity を返し続けるため。カウンタが止まっていることまでは
 *  > 押さえられていない。
 *
 *  **これは解消された。** 生存管理が `m_generations` / `m_freeIndices` の 2 本で
 *  表現され、`AliveCount()` がそこから**導出**されるようになったため、
 *  「カウンタだけが進む」状態が存在しない。T-ECS-5 は上限に達した後も
 *  `AliveCount()` が動かないことを直接見る。
 *
 *  @note **Debug と Release でチェック数が違う。** `Entity::Make` で組み立てた
 *        壊れたハンドルを `SparseSet::TryEmplace` へ通す経路は Debug で
 *        `assert` に当たるため、その一部は `NDEBUG` のときだけ試せる。
 *        意図した非対称であり、該当箇所に理由を書いてある。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "Core/GameConfig.h"
#include "Core/Json/Json.h"
#include "ECS/Entity.h"
#include "ECS/Registry.h"
#include "ECS/View.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::GameConfig;
using GLFD::kDefaultEntityCount;
using GLFD::kMaxConfigurableEntityCount;
using GLFD::StringView;
using GLFD::ECS::Entity;
using GLFD::ECS::MaxEntities;
using GLFD::ECS::Registry;
using GLFD::Json::ArchiveContext;
using GLFD::Json::ArchiveErrorKind;
using GLFD::Json::ArchiveFlags;
using GLFD::Json::Document;
using GLFD::Json::LoadFromJson;
using GLFD::Json::ParseFlags;
using GLFD::Test::MockMemoryResource;

namespace {

  struct Health { int hp = 0; };
  struct Armour { int value = 0; };
  struct Tag    { int id = 0; };

  /// メモリ実測用。**型ごとに別の型**になるよう番号で分ける
  template <int N> struct Bulk { float a, b, c, d; };   // 16 B

  // ===========================================================================
  // T-ECS-1 世代によるダングリング検出(**中核**)
  // ===========================================================================

  void TestGenerationDetectsDangling() {
    GLFD::Test::BeginCase("T-ECS-1: a stale handle is rejected after the index is reused");

    MockMemoryResource mock;
    Registry           registry(&mock);

    const Entity first = registry.CreateEntity();
    CHECK(first.IsValid());
    CHECK(registry.IsAlive(first));
    CHECK(registry.AddComponent<Health>(first, 100) != nullptr);

    registry.DestroyEntity(first);
    CHECK(!registry.IsAlive(first));
    // **Entity::IsValid() と Registry::IsAlive() は別物** (R-3)
    CHECK(first.IsValid());

    // 同じ index が再利用されること。ここが前提でないとこのテストは無意味
    const Entity second = registry.CreateEntity();
    CHECK(second.IsValid());
    CHECK(second.Index() == first.Index());
    CHECK(second.Generation() != first.Generation());

    // **古いハンドルは死んだまま。新しいハンドルは生きている**
    CHECK(!registry.IsAlive(first));
    CHECK(registry.IsAlive(second));

    // 新しいエンティティに成分を持たせる
    CHECK(registry.AddComponent<Health>(second, 42) != nullptr);

    // --- 古いハンドルで触っても、新しいエンティティに影響しないこと ----------
    CHECK(registry.GetComponent<Health>(first) == nullptr);
    CHECK(registry.AddComponent<Armour>(first, 7) == nullptr);
    registry.RemoveComponent<Health>(first);          // 何も起きてはならない
    registry.DestroyEntity(first);                    // 二重破棄も安全

    const Health* const alive = registry.GetComponent<Health>(second);
    CHECK(alive != nullptr);
    CHECK(alive != nullptr && alive->hp == 42);
    CHECK(!registry.HasComponent<Armour>(second));
    CHECK(registry.IsAlive(second));
    CHECK(registry.AliveCount() == 1u);

    // **同じ成分は 2 回足せない。** 足せてしまうと dense に 2 つ並び、
    // 疎配列は後から書いた方だけを指すので、前の 1 つが取り残される
    CHECK(registry.AddComponent<Health>(second, 999) == nullptr);
    const Health* const unchanged = registry.GetComponent<Health>(second);
    CHECK(unchanged != nullptr && unchanged->hp == 42);
    CHECK(registry.View<Health>().BaseSize() == 1u);
  }

  // ===========================================================================
  // T-ECS-2 破棄の伝播
  // ===========================================================================

  void TestDestroyRemovesEveryComponent() {
    GLFD::Test::BeginCase("T-ECS-2: destroying an entity removes all of its components");

    MockMemoryResource mock;
    Registry           registry(&mock);

    const Entity target   = registry.CreateEntity();
    const Entity bystander = registry.CreateEntity();

    CHECK(registry.AddComponent<Health>(target, 10) != nullptr);
    CHECK(registry.AddComponent<Armour>(target, 20) != nullptr);
    CHECK(registry.AddComponent<Tag>(target, 30) != nullptr);

    CHECK(registry.AddComponent<Health>(bystander, 11) != nullptr);
    CHECK(registry.AddComponent<Armour>(bystander, 21) != nullptr);
    CHECK(registry.AddComponent<Tag>(bystander, 31) != nullptr);

    CHECK(registry.AliveCount() == 2u);

    registry.DestroyEntity(target);

    // **3 つとも消えていること**
    CHECK(registry.GetComponent<Health>(target) == nullptr);
    CHECK(registry.GetComponent<Armour>(target) == nullptr);
    CHECK(registry.GetComponent<Tag>(target) == nullptr);
    CHECK(!registry.HasComponent<Health>(target));

    // **swap-and-pop で動かされた側を、ハンドルで引き直せること。**
    // `target` は dense の先頭にいたので、末尾の `bystander` が穴へ移っている。
    // 疎配列の付け替えを忘れると、ここで古い添字を引いて別物が返る
    const Health* const moved = registry.GetComponent<Health>(bystander);
    CHECK(moved != nullptr);
    CHECK(moved != nullptr && moved->hp == 11);
    CHECK(moved == registry.View<Health>().BaseComponents());   // 先頭へ移動している

    // **隣のエンティティは無傷であること**
    const Health* const h = registry.GetComponent<Health>(bystander);
    const Armour* const a = registry.GetComponent<Armour>(bystander);
    const Tag*    const t = registry.GetComponent<Tag>(bystander);
    CHECK(h != nullptr && h->hp == 11);
    CHECK(a != nullptr && a->value == 21);
    CHECK(t != nullptr && t->id == 31);
    CHECK(registry.AliveCount() == 1u);
  }

  // ===========================================================================
  // T-ECS-3 無効な Entity への全操作
  // ===========================================================================

  void TestInvalidHandlesFailSafely() {
    GLFD::Test::BeginCase("T-ECS-3: every operation on an invalid handle fails safely");

    MockMemoryResource mock;
    Registry           registry(&mock);

    const Entity living = registry.CreateEntity();
    CHECK(registry.AddComponent<Health>(living, 5) != nullptr);

    const Entity destroyed = registry.CreateEntity();
    registry.DestroyEntity(destroyed);

    // 範囲外の index。**Registry から出てきたハンドルではない**
    const Entity outOfRange = Entity::Make(0xFFFFFFF0u, 0u);

    const Entity broken[] = { Entity::Invalid(), destroyed, outOfRange };

    for (const Entity bad : broken) {
      CHECK(!registry.IsAlive(bad));
      CHECK(registry.GetComponent<Health>(bad) == nullptr);
      CHECK(!registry.HasComponent<Health>(bad));

      registry.RemoveComponent<Health>(bad);   // 何も起きてはならない
      registry.DestroyEntity(bad);             // 二重破棄・無効値も安全
      registry.DestroyEntity(bad);
    }

#ifdef NDEBUG
    // `AddComponent` は `IsAlive` で弾くので nullptr になる。Debug では
    // `Entity::Invalid()` が `SparseSet::TryEmplace` の assert に届く前に
    // Registry で止まるが、**壊れたハンドルを assert 付きの経路へ通す形**は
    // NDEBUG でだけ試す(assert の有無で振る舞いを変えないことの確認)
    for (const Entity bad : broken) {
      CHECK(registry.AddComponent<Health>(bad, 1) == nullptr);
    }
#else
    std::printf("    (skipped: AddComponent on broken handles is release-only here)\n");
#endif

    // **生きている方は一切影響を受けていない**
    const Health* const h = registry.GetComponent<Health>(living);
    CHECK(h != nullptr && h->hp == 5);
    CHECK(registry.IsAlive(living));
    CHECK(registry.AliveCount() == 1u);
  }

  // ===========================================================================
  // T-ECS-5 上限と AliveCount
  // ===========================================================================

  void TestAliveCountTracksCreateDestroyReuse() {
    GLFD::Test::BeginCase("T-ECS-5: AliveCount follows a mixed create / destroy / reuse sequence");

    MockMemoryResource mock;
    Registry           registry(&mock);

    CHECK(registry.AliveCount() == 0u);

    Entity handles[8] = {};
    for (int i = 0; i < 8; ++i) {
      handles[i] = registry.CreateEntity();
      CHECK(handles[i].IsValid());
    }
    CHECK(registry.AliveCount() == 8u);

    // 飛び飛びに壊す
    registry.DestroyEntity(handles[1]);
    registry.DestroyEntity(handles[4]);
    registry.DestroyEntity(handles[7]);
    CHECK(registry.AliveCount() == 5u);

    // 二重破棄は数を動かさない
    registry.DestroyEntity(handles[1]);
    CHECK(registry.AliveCount() == 5u);

    // 再利用。**index は使い回されるが generation は進む**
    const Entity reused = registry.CreateEntity();
    CHECK(registry.AliveCount() == 6u);
    CHECK(reused.Index() == handles[7].Index());     // スタックなので最後に返した index
    CHECK(!registry.IsAlive(handles[7]));
    CHECK(registry.IsAlive(reused));

    // 空きを使い切ってから新規へ移る
    const Entity more1 = registry.CreateEntity();
    const Entity more2 = registry.CreateEntity();
    const Entity fresh = registry.CreateEntity();
    CHECK(registry.AliveCount() == 9u);
    CHECK(more1.IsValid() && more2.IsValid() && fresh.IsValid());

    // 全部壊す
    registry.DestroyEntity(reused);
    registry.DestroyEntity(more1);
    registry.DestroyEntity(more2);
    registry.DestroyEntity(fresh);
    for (int i = 0; i < 8; ++i) {
      registry.DestroyEntity(handles[i]);           // 既に死んでいるものも混ざる
    }
    CHECK(registry.AliveCount() == 0u);
  }

  void TestCreateStopsAtTheLimit() {
    GLFD::Test::BeginCase("T-ECS-5: CreateEntity stops at MaxEntities and stays safe");

    MockMemoryResource mock;
    Registry           registry(&mock);

    bool allValid = true;
    for (std::size_t i = 0; i < MaxEntities; ++i) {
      if (!registry.CreateEntity().IsValid()) { allValid = false; break; }
    }
    CHECK(allValid);
    CHECK(registry.AliveCount() == static_cast<std::uint32_t>(MaxEntities));

    // **上限を超えたら Invalid。以後も安全**
    bool allInvalid = true;
    for (int i = 0; i < 1000; ++i) {
      if (registry.CreateEntity().IsValid()) { allInvalid = false; break; }
    }
    CHECK(allInvalid);

    // ECS-0c で押さえられなかったのがここ。**カウンタが進まないことを直接見る**。
    // `AliveCount()` は `m_generations` と `m_freeIndices` からの導出なので、
    // 「上限後も内部カウンタだけ進む」という状態が表現できない
    CHECK(registry.AliveCount() == static_cast<std::uint32_t>(MaxEntities));

    // 1 つ壊せば 1 つ作れる(上限で固まってしまわないこと)
    const Entity recycled = registry.CreateEntity();
    CHECK(!recycled.IsValid());
    registry.DestroyEntity(Entity::Make(0u, 0u));
    CHECK(registry.AliveCount() == static_cast<std::uint32_t>(MaxEntities) - 1u);
    const Entity afterFree = registry.CreateEntity();
    CHECK(afterFree.IsValid());
    CHECK(registry.AliveCount() == static_cast<std::uint32_t>(MaxEntities));
  }

  // ===========================================================================
  // config の値域検査(ECS-0c から引き継ぎ)
  // ===========================================================================

  /// テストが**自分で書いた** config を読む。出荷 config には触らない
  struct ConfigLoad {
    MockMemoryResource mock;
    Document           doc{ &mock };
    ArchiveContext     ctx{ doc.Arena(), 0, ArchiveFlags::ReportUnknown };
    GameConfig         config{ &mock };

    [[nodiscard]] bool Run(const char* json) {
      return LoadFromJson(config, StringView(json), doc, ctx, ParseFlags::None);
    }
    [[nodiscard]] bool SawRangeOverflow() const {
      for (std::uint32_t i = 0; i < ctx.IssueCount(); ++i) {
        if (ctx.Issues()[i].kind == ArchiveErrorKind::RangeOverflow) { return true; }
      }
      return false;
    }
  };

  void TestEntityCountIsRangeChecked() {
    GLFD::Test::BeginCase("ECS-0c: an out-of-range entityCount is reported and left at the default");

    {
      char json[128];
      std::snprintf(json, sizeof(json), "{ \"simulation\": { \"entityCount\": %d } }",
                    kMaxConfigurableEntityCount);
      ConfigLoad load;
      CHECK(load.Run(json));
      CHECK(!load.SawRangeOverflow());
      CHECK(load.config.simulation.entityCount == kMaxConfigurableEntityCount);
    }
    {
      char json[128];
      std::snprintf(json, sizeof(json), "{ \"simulation\": { \"entityCount\": %d } }",
                    kMaxConfigurableEntityCount + 1);
      ConfigLoad load;
      CHECK(load.Run(json));                    // RangeOverflow は Fatal ではない
      CHECK(load.SawRangeOverflow());
      CHECK(!load.ctx.HasFatal());
      CHECK(load.config.simulation.entityCount == kDefaultEntityCount);
    }
    {
      ConfigLoad load;
      CHECK(load.Run("{ \"simulation\": { \"entityCount\": 5000000 } }"));
      CHECK(load.SawRangeOverflow());
      CHECK(load.config.simulation.entityCount == kDefaultEntityCount);
      CHECK(static_cast<std::size_t>(load.config.simulation.entityCount) < MaxEntities);
    }
    {
      ConfigLoad load;
      CHECK(load.Run("{ \"simulation\": { \"entityCount\": -1 } }"));
      CHECK(load.SawRangeOverflow());
      CHECK(load.config.simulation.entityCount == kDefaultEntityCount);
    }
    {
      ConfigLoad load;
      CHECK(load.Run("{ \"simulation\": { \"entityCount\": 9999999, \"maxSpeed\": 3.5 } }"));
      CHECK(load.SawRangeOverflow());
      CHECK(load.config.simulation.entityCount == kDefaultEntityCount);
      CHECK(load.config.simulation.maxSpeed == 3.5f);   // 巻き添えにならない
    }
  }

  void TestCapStaysBelowTheStructuralLimit() {
    GLFD::Test::BeginCase("T-ECS-10: the three limits stay consistent with each other");

    // **規約が変わった (1-2)。** 以前は「1 桁の余裕」だったが、`MaxEntities` 自体が
    // 65,536 まで下がったので過剰になった。今は**半分**を規約とする
    CHECK(static_cast<std::size_t>(kMaxConfigurableEntityCount) < MaxEntities);
    CHECK(static_cast<std::size_t>(kMaxConfigurableEntityCount) * 2u <= MaxEntities);
    CHECK(kDefaultEntityCount <= kMaxConfigurableEntityCount);

    // 出荷 config の既定値が上限に張り付いていないこと。**余裕がゼロだと
    // チューニングのたびに上限へ触る**
    CHECK(kDefaultEntityCount < kMaxConfigurableEntityCount);

    // 疎配列の番兵と実値が衝突しない条件 (`SparseSet` の static_assert と同じ関係)
    CHECK(MaxEntities <= 0xFFFFFFFEu);
  }

  // ===========================================================================
  // T-ECS-6 反復順序への非依存 (R-16)
  // ===========================================================================

  /// dense を舐めて合計を出す。**順序に依存しない集計**なので、
  /// swap-and-pop で並びが変わっても結果は変わらないはず
  [[nodiscard]] int SumHealth(Registry& registry) {
    int total = 0;
    // **1-5 で View 経由にした。** 「システムの結果」を見る集計なので、
    // 本番と同じ経路で回す方が意味がある (R-32)
    for (auto [entity, h] : registry.View<Health>()) {
      (void)entity;
      total += h.hp;
    }
    return total;
  }

  void TestResultDoesNotDependOnDenseOrder() {
    GLFD::Test::BeginCase("T-ECS-6: swap-and-pop reorders dense, but the result is the same");

    // 同じ集合 {10, 20, 30, 40} を、**違う生成・破棄の順序**で作る
    const int kWanted[] = { 10, 20, 30, 40 };
    const int kExpected = 10 + 20 + 30 + 40;

    int sums[3]   = { 0, 0, 0 };
    int orders[3] = { 0, 0, 0 };   // dense の先頭に来た値。並びが違うことの証拠

    {   // (a) 素直に 4 体
      MockMemoryResource mock;
      Registry           registry(&mock);
      for (int v : kWanted) {
        const Entity e = registry.CreateEntity();
        CHECK(registry.AddComponent<Health>(e, v) != nullptr);
      }
      sums[0]   = SumHealth(registry);
      orders[0] = registry.View<Health>().BaseComponents()[0].hp;
    }
    {   // (b) 余分に作ってから**間を抜く**。swap-and-pop が走る
      MockMemoryResource mock;
      Registry           registry(&mock);
      Entity made[6] = {};
      const int filler[] = { 10, 99, 20, 98, 30, 40 };
      for (int i = 0; i < 6; ++i) {
        made[i] = registry.CreateEntity();
        CHECK(registry.AddComponent<Health>(made[i], filler[i]) != nullptr);
      }
      registry.DestroyEntity(made[1]);   // 99 を抜く -> 末尾が穴へ来る
      registry.DestroyEntity(made[3]);   // 98 を抜く
      sums[1]   = SumHealth(registry);
      orders[1] = registry.View<Health>().BaseComponents()[0].hp;
    }
    {   // (c) 逆順に作る
      MockMemoryResource mock;
      Registry           registry(&mock);
      for (int i = 3; i >= 0; --i) {
        const Entity e = registry.CreateEntity();
        CHECK(registry.AddComponent<Health>(e, kWanted[i]) != nullptr);
      }
      sums[2]   = SumHealth(registry);
      orders[2] = registry.View<Health>().BaseComponents()[0].hp;
    }

    // **結果は 3 つとも一致する**
    CHECK(sums[0] == kExpected);
    CHECK(sums[1] == kExpected);
    CHECK(sums[2] == kExpected);

    // **並びは実際に違っている**(違わなければこのテストは何も確かめていない)
    CHECK(orders[0] != orders[2]);
    (void)orders[1];
  }

  // ===========================================================================
  // T-ECS-7 確保失敗の注入
  // ===========================================================================

  void TestAllocationFailureIsSurvivable() {
    GLFD::Test::BeginCase("T-ECS-7: injected allocation failures return Invalid / nullptr, never crash");

    // まず失敗させずに、何回確保するかを測る
    int baseline = 0;
    {
      MockMemoryResource mock;
      Registry           registry(&mock);
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Health>(e, 1) != nullptr);
      CHECK(registry.AddComponent<Armour>(e, 2) != nullptr);
      registry.DestroyEntity(e);
      baseline = mock.AllocateCalls();
    }
    CHECK(baseline > 0);

    // **N 回目から総当たり**で失敗させる (JSON の T-10 / T-25 と同じ手法)
    bool everyRunSurvived = true;
    for (int failAfter = 0; failAfter <= baseline; ++failAfter) {
      MockMemoryResource mock;
      mock.SetFailAfter(failAfter);
      Registry registry(&mock);

      const Entity e = registry.CreateEntity();
      // 作れたなら生きている。作れなければ Invalid。**その中間は無い**
      if (e.IsValid() != registry.IsAlive(e)) { everyRunSurvived = false; }

      // 失敗しても nullptr が返るだけで、例外もクラッシュも起きない
      Health* const h = registry.AddComponent<Health>(e, 1);
      Armour* const a = registry.AddComponent<Armour>(e, 2);
      if (h != nullptr && registry.GetComponent<Health>(e) != h) { everyRunSurvived = false; }
      if (a != nullptr && registry.GetComponent<Armour>(e) != a) { everyRunSurvived = false; }

      // 成分が付いていなくても破棄は安全
      registry.DestroyEntity(e);
      if (registry.IsAlive(e)) { everyRunSurvived = false; }

      // **生存数を厳密に固定する。** 「1 以下」では緩すぎた:
      // 空き集合を伸ばせずに引退した分を `AliveCount` が引き忘れる変異 (M5) を
      // 見逃す。作れたものは必ず壊してあるので、どの走りでも 0 でなければならない
      if (registry.AliveCount() != 0u) { everyRunSurvived = false; }

      if (mock.DoubleFree() || mock.UnknownFree()
          || mock.SizeMismatch() || mock.AlignmentMismatch()) {
        everyRunSurvived = false;
      }
    }
    CHECK(everyRunSurvived);

    // **確保と解放が対になっていること** (R-15)。以前は Deallocate を呼んで
    // いなかったので、ここで漏れが出る
    {
      MockMemoryResource mock;
      {
        Registry registry(&mock);
        const Entity e = registry.CreateEntity();
        CHECK(registry.AddComponent<Health>(e, 1) != nullptr);
        CHECK(registry.AddComponent<Armour>(e, 2) != nullptr);
      }
      CHECK(mock.LiveBlockCount() == 0u);
      CHECK(mock.LiveBytes() == 0u);
      CHECK(!mock.DoubleFree());
      CHECK(!mock.UnknownFree());
    }
  }

  // ===========================================================================
  // T-ECS-11 メモリ実測 (1-2)
  // ===========================================================================

  /// 型を N 個登録したときの生存バイト数
  template <int... Is>
  [[nodiscard]] std::size_t LiveBytesForTypes(std::integer_sequence<int, Is...>) {
    MockMemoryResource mock;
    {
      Registry           registry(&mock);
      const Entity       e = registry.CreateEntity();
      ((void)registry.AddComponent<Bulk<Is>>(e), ...);
    }
    // 破棄後は 0 でなければならない (R-15: 確保と解放が対)
    return mock.LiveBytes();
  }

  template <int... Is>
  [[nodiscard]] std::size_t PeakBytesForTypes(std::integer_sequence<int, Is...>,
                                              bool& leaked) {
    MockMemoryResource mock;
    std::size_t peak = 0;
    {
      Registry     registry(&mock);
      const Entity e = registry.CreateEntity();
      ((void)registry.AddComponent<Bulk<Is>>(e), ...);
      peak = mock.LiveBytes();
    }
    leaked = (mock.LiveBytes() != 0u);
    return peak;
  }

  void TestSparseArrayMemoryStaysWithinBudget() {
    GLFD::Test::BeginCase("T-ECS-11: the sparse arrays cost what the design says they cost");

    // **値を固定しない。** 上界で書く。`MaxEntities` や要素型を将来変えても
    // 意味が保たれる形にしてある
    const std::size_t sparsePerType = MaxEntities * sizeof(std::uint32_t);

    bool leaked1 = false;
    bool leaked4 = false;
    const std::size_t one  = PeakBytesForTypes(std::make_integer_sequence<int, 1>{}, leaked1);
    const std::size_t four = PeakBytesForTypes(std::make_integer_sequence<int, 4>{}, leaked4);

    CHECK(!leaked1);
    CHECK(!leaked4);

    // 1 型ぶんは「疎配列 + わずかな付帯」に収まること。
    // 付帯 = dense(要素 1 個ぶんの初期容量)・生存配列・プール一覧・SparseSet 本体
    CHECK(one >= sparsePerType);
    CHECK(one <= sparsePerType + 64u * 1024u);

    // **型ごとに疎配列 1 本ぶんだけ増えること。** ここが設計の要点で、
    // 「型を足すと固定費が線形に乗る」ことを固定する
    const std::size_t perExtraType = (four - one) / 3u;
    CHECK(perExtraType >= sparsePerType);
    CHECK(perExtraType <= sparsePerType + 64u * 1024u);

    // 想定する型数 (VS 型で 15〜25) での総量が予算に収まること。
    // **上界だけ書く。** 実測値そのものは固定しない
    const std::size_t twentyTypes = one + perExtraType * 19u;
    CHECK(twentyTypes <= 16u * 1024u * 1024u);   // 16 MB 未満

    std::printf("    (measured: 1 type = %zu B, per extra type = %zu B, "
                "20 types = %zu B / %.2f MB)\n",
                one, perExtraType, twentyTypes,
                static_cast<double>(twentyTypes) / (1024.0 * 1024.0));

    // 解放後に残らないこと
    CHECK(LiveBytesForTypes(std::make_integer_sequence<int, 4>{}) == 0u);
  }

  // ===========================================================================
  // Entity 自体の性質
  // ===========================================================================

  void TestEntityHandleShape() {
    GLFD::Test::BeginCase("R-1..R-4: the handle packs index and generation and has no valid default");

    CHECK(sizeof(Entity) == 8u);
    CHECK(!Entity{}.IsValid());
    CHECK(!Entity::Invalid().IsValid());
    CHECK(Entity::Invalid() == Entity{});

    const Entity e = Entity::Make(123u, 456u);
    CHECK(e.IsValid());
    CHECK(e.Index() == 123u);
    CHECK(e.Generation() == 456u);

    // index が同じでも generation が違えば別物であること。**これが 64bit 化の要点**
    CHECK(Entity::Make(123u, 456u) == e);
    CHECK(Entity::Make(123u, 457u) != e);
    CHECK(Entity::Make(124u, 456u) != e);

    // 無効値は「使えそうな index」に見えないこと (R-4)
    CHECK(Entity::Invalid().Index() == 0xFFFFFFFFu);
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsRegistry (ECS 1-1)");

  TestEntityHandleShape();
  TestGenerationDetectsDangling();
  TestDestroyRemovesEveryComponent();
  TestInvalidHandlesFailSafely();
  TestAliveCountTracksCreateDestroyReuse();
  TestCreateStopsAtTheLimit();
  TestResultDoesNotDependOnDenseOrder();
  TestAllocationFailureIsSurvivable();
  TestEntityCountIsRangeChecked();
  TestCapStaysBelowTheStructuralLimit();
  TestSparseArrayMemoryStaysWithinBudget();

  return GLFD::Test::Summarize();
}
