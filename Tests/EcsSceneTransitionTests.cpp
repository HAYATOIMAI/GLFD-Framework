/**
 * @file  EcsSceneTransitionTests.cpp
 * @brief ECS 2-1: シーン遷移の安全性と `OnExit` の契約 — T-ECS-25 / T-ECS-26
 *
 * @details
 *  ## 本番の経路を呼ぶ (4.7)
 *  `SceneManager` も `EventBus` も `Registry` も本物である。`AttachSurvivor` /
 *  `DetachSurvivor` も本物を呼ぶ。**写した手順を試すのではない。**
 *
 *  ## 押さえられないもの
 *  **`SurvivorScene` / `BoidDemoScene` そのものは呼べない。** `OnEnter` が
 *  `ResourceManager` からテクスチャを読み、DX11 を要求するためである。ここで
 *  固定できるのは `SceneManager` の側(外し忘れの検出と安全網、エンティティの
 *  後始末、往復)であって、**実物のシーンがその契約を守っているかではない**。
 *  それは実機での往復が受け持つ。
 *
 *  ## `assert` は使わない (4.12)
 *  発火すると `abort()` するのでテストから確かめられない。`EventBus` の
 *  `SubscriberCount()` と `SceneManager::Report()` を読む。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "Core/DoubleStackAllocator.h"
#include "Core/GameContext.h"
#include "Core/StackAllocator.h"
#include "Core/StackResource.h"

#include "ECS/CommandBuffer.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"

#include "Events/EventBus.h"
#include "Events/Events.h"

#include "Game/SurvivorComponents.h"
#include "Game/SurvivorLoop.h"

#include "Scene/SceneManager.h"

#include "TestHarness.h"

using GLFD::ECS::Entity;
using GLFD::Events::SubscriptionId;
using GLFD::Scene::SceneManager;
using GLFD::Scene::TransitionReport;

namespace {

  // ===========================================================================
  // 世界 1 つぶん。**エンジンが組むのと同じ部品**を、DX11 抜きで並べる
  // ===========================================================================
  struct World {
    GLFD::Memory::StackAllocator mainStack{ 64u * 1024u * 1024u };
    GLFD::Memory::StackResource  resource{ mainStack };
    GLFD::ECS::Registry          registry{ &resource };
    GLFD::ECS::CommandBuffer     commands{ &resource };
    GLFD::Events::EventBus       bus{ &resource };
    SceneManager                 scenes{ &resource };

    [[nodiscard]] GLFD::GameContext Context() {
      GLFD::GameContext ctx{};
      ctx.globalResource = &resource;
      ctx.frameResource  = nullptr;
      ctx.jobSystem      = nullptr;
      ctx.registry       = &registry;
      ctx.commands       = &commands;
      ctx.eventBus       = &bus;
      ctx.grid           = nullptr;
      ctx.window         = nullptr;
      ctx.input          = nullptr;
      ctx.fileManager    = nullptr;
      ctx.dt             = 0.016f;
      ctx.renderer       = nullptr;
      ctx.totalTime      = 0.0f;
      ctx.sceneManager   = &scenes;
      ctx.resourceManager = nullptr;
      return ctx;
    }
  };

  /// 使い捨ての観測点。**購読が生きていれば増える**
  struct Counter {
    std::uint32_t hits = 0;
  };

  void CountHit(void* context, const GLFD::Events::HitEvent&) {
    ++static_cast<Counter*>(context)->hits;
  }

  Entity MakeEnemy(GLFD::ECS::Registry& registry, float x) {
    const Entity e = registry.CreateEntity();
    if (!e.IsValid()) { return e; }
    (void)registry.AddComponent<GLFD::Components::Position>(e, x, 0.0f, 0.0f, 0.0f);
    (void)registry.AddComponent<GLFD::Components::Health>(e, 3.0f);
    return e;
  }

  // ===========================================================================
  // テスト用のシーン。**契約を守るものと、わざと破るものの両方**を用意する
  // ===========================================================================

  /// 契約どおりのシーン。`AttachSurvivor` / `DetachSurvivor` を本物で通す
  class WellBehavedScene : public GLFD::Scene::IScene {
  public:
    explicit WellBehavedScene(int id, int enemies) : m_id(id), m_enemies(enemies) {}

    void OnEnter(GLFD::GameContext& ctx) override {
      ++s_entered;
      m_attached = GLFD::Game::AttachSurvivor(m_state, *ctx.registry, *ctx.commands,
                                              *ctx.eventBus);
      for (int i = 0; i < m_enemies; ++i) {
        // **座標を原点や先頭一致に寄せない** (4.2)。id ごとに別の位置へ置く
        (void)MakeEnemy(*ctx.registry, 10.0f + static_cast<float>(m_id * 7 + i));
      }
    }
    void OnUpdate(GLFD::GameContext&) override { ++m_updates; }
    void OnRender(GLFD::GameContext&) override { ++m_renders; }
    void OnExit(GLFD::GameContext& ctx) override {
      ++s_exited;
      (void)GLFD::Game::DetachSurvivor(m_state, *ctx.eventBus);
      (void)ctx.registry->DestroyAll();
    }

    [[nodiscard]] bool Attached() const { return m_attached; }
    [[nodiscard]] int  Updates() const { return m_updates; }
    [[nodiscard]] int  Renders() const { return m_renders; }

    static inline int s_entered = 0;
    static inline int s_exited  = 0;

  private:
    int  m_id;
    int  m_enemies;
    int  m_updates = 0;
    int  m_renders = 0;
    bool m_attached = false;
    GLFD::Game::SurvivorState m_state{};
  };

  /// 契約を破るシーン。**外さない。消さない。**(T-ECS-26)
  class LeakyScene : public GLFD::Scene::IScene {
  public:
    explicit LeakyScene(Counter* counter) : m_counter(counter) {}

    void OnEnter(GLFD::GameContext& ctx) override {
      m_id = ctx.eventBus->Subscribe<GLFD::Events::HitEvent>(m_counter, &CountHit);
      (void)MakeEnemy(*ctx.registry, 31.0f);
      (void)MakeEnemy(*ctx.registry, 37.0f);
      (void)MakeEnemy(*ctx.registry, 41.0f);
    }
    void OnUpdate(GLFD::GameContext&) override {}
    void OnRender(GLFD::GameContext&) override {}
    void OnExit(GLFD::GameContext&) override { /* わざと何もしない */ }

    [[nodiscard]] SubscriptionId Id() const { return m_id; }

  private:
    Counter* m_counter;
    SubscriptionId m_id{};
  };

  /// `OnEnter` から遷移を要求するシーン。**保留配列が再確保される**(1-8 の欠陥)
  class ReentrantScene : public GLFD::Scene::IScene {
  public:
    void OnEnter(GLFD::GameContext& ctx) override {
      ++s_entered;
      // 入った瞬間に「別のシーンへ」と言う。素朴に書くと自然に出る形である
      for (int i = 0; i < 8; ++i) {
        (void)ctx.sceneManager->PushScene(std::make_unique<ReentrantScene>());
      }
      (void)ctx.sceneManager->PopScene();
    }
    void OnUpdate(GLFD::GameContext&) override {}
    void OnRender(GLFD::GameContext&) override {}
    void OnExit(GLFD::GameContext&) override {}

    static inline int s_entered = 0;
  };

  // ===========================================================================
  // T-ECS-25 遷移の安全性
  // ===========================================================================

  /// 抜けた後に配信してもクラッシュしない = **購読が本当に外れている**
  void TestLeavingRemovesTheSubscription() {
    GLFD::Test::BeginCase("T-ECS-25a: a scene that left is no longer subscribed");
    World world;
    GLFD::GameContext ctx = world.Context();

    const std::size_t before = world.bus.SubscriberCount();
    CHECK(before == 0u);

    CHECK(world.scenes.PushScene(std::make_unique<WellBehavedScene>(1, 4)));
    world.scenes.ProcessPendingTransitions(ctx);
    CHECK(world.bus.SubscriberCount() == 1u);
    CHECK(world.registry.AliveCount() == 4u);

    CHECK(world.scenes.PopScene());
    world.scenes.ProcessPendingTransitions(ctx);

    // **数で確かめる。** 解放済みのシーンを呼んでも「たいてい」落ちないので、
    // 落ちないことを根拠にしてはならない
    CHECK(world.bus.SubscriberCount() == 0u);
    CHECK(world.registry.AliveCount() == 0u);
    CHECK(world.scenes.Report().leakedSubscriptions == 0u);
    CHECK(world.scenes.Report().leakedEntities == 0u);

    // 抜けた後の配信。購読が残っていれば解放済みの `SurvivorState` を触る
    world.bus.Publish(GLFD::Events::HitEvent{ Entity::Invalid(), Entity::Invalid() });
    world.bus.DispatchAll();
    CHECK(world.bus.SubscriberCount() == 0u);
  }

  /// 入り直しても購読者が二重にならない
  void TestReenteringDoesNotDoubleSubscribe() {
    GLFD::Test::BeginCase("T-ECS-25b: re-entering does not leave two subscribers");
    World world;
    GLFD::GameContext ctx = world.Context();

    for (int lap = 0; lap < 4; ++lap) {
      CHECK(world.scenes.ChangeScene(std::make_unique<WellBehavedScene>(lap + 1, 3)));
      world.scenes.ProcessPendingTransitions(ctx);
      // **毎周 1 件でなければならない。** 積み上がるなら解除が効いていない
      CHECK(world.bus.SubscriberCount() == 1u);
      CHECK(world.registry.AliveCount() == 3u);
    }

    CHECK(world.scenes.PopScene());
    world.scenes.ProcessPendingTransitions(ctx);
    CHECK(world.bus.SubscriberCount() == 0u);
    CHECK(world.registry.AliveCount() == 0u);
    CHECK(world.scenes.Report().leakedSubscriptions == 0u);
  }

  /// 前のシーンのエンティティが次のシーンに残らない
  void TestEntitiesDoNotSurviveTheTransition() {
    GLFD::Test::BeginCase("T-ECS-25c: the next scene starts with an empty registry");
    World world;
    GLFD::GameContext ctx = world.Context();

    CHECK(world.scenes.ChangeScene(std::make_unique<WellBehavedScene>(1, 9)));
    world.scenes.ProcessPendingTransitions(ctx);
    CHECK(world.registry.AliveCount() == 9u);

    // **`BaseEntities()` は空なら nullptr を返す。** 添字を付ける前に確かめる:
    // 直前の CHECK が落ちている状況で素朴に [0] と書くと、テストは
    // **失敗ではなく異常終了する**(そして 2-1 の時点のランナーは、それを
    // 合格として数えていた)
    const auto healthView = world.registry.View<GLFD::Components::Health>();
    CHECK(healthView.BaseSize() == 9u);
    if (healthView.BaseSize() == 0u) { return; }
    const Entity stale = healthView.BaseEntities()[0];
    CHECK(world.registry.IsAlive(stale));

    CHECK(world.scenes.ChangeScene(std::make_unique<WellBehavedScene>(2, 5)));
    world.scenes.ProcessPendingTransitions(ctx);

    CHECK(world.registry.AliveCount() == 5u);
    // **世代が上がっているので、前のシーンのハンドルは死んでいる。**
    // `DestroyAll` が世代を 0 に戻すと、ここが生き返る
    CHECK(!world.registry.IsAlive(stale));
    CHECK(world.registry.GetComponent<GLFD::Components::Health>(stale) == nullptr);
  }

  /// A -> B -> A -> B と往復できる
  void TestRoundTrip() {
    GLFD::Test::BeginCase("T-ECS-25d: A -> B -> A -> B round trip stays balanced");
    World world;
    GLFD::GameContext ctx = world.Context();

    WellBehavedScene::s_entered = 0;
    WellBehavedScene::s_exited  = 0;

    const int counts[4] = { 6, 2, 7, 3 };   // **毎周ちがう数**。0 にも一致にも寄せない
    for (int lap = 0; lap < 4; ++lap) {
      CHECK(world.scenes.ChangeScene(std::make_unique<WellBehavedScene>(lap + 1, counts[lap])));
      world.scenes.ProcessPendingTransitions(ctx);
      world.scenes.Update(ctx);
      world.scenes.Render(ctx);

      CHECK(world.registry.AliveCount() == static_cast<std::uint32_t>(counts[lap]));
      CHECK(world.bus.SubscriberCount() == 1u);
      CHECK(world.scenes.Depth() == 1u);
    }

    CHECK(WellBehavedScene::s_entered == 4);
    CHECK(WellBehavedScene::s_exited == 3);      // 最初の入場には対になる退場が無い
    CHECK(world.scenes.Report().transitions == 4u);
    CHECK(!world.scenes.Report().HasProblem());
  }

  /// `OnEnter` からの遷移要求で保留配列が再確保されても壊れない (1-8 の欠陥)
  void TestRequestsMadeWhileEnteringAreDeferred() {
    GLFD::Test::BeginCase("T-ECS-25e: a transition requested from OnEnter is deferred");
    World world;
    GLFD::GameContext ctx = world.Context();

    ReentrantScene::s_entered = 0;
    CHECK(world.scenes.PushScene(std::make_unique<ReentrantScene>()));
    world.scenes.ProcessPendingTransitions(ctx);

    // **1 回の呼び出しでは 1 つしか入らない。** 素朴になぞる実装だと、
    // `OnEnter` の 8 件で保留配列が伸びて参照が宙に浮く
    CHECK(ReentrantScene::s_entered == 1);
    CHECK(world.scenes.Depth() == 1u);

    world.scenes.ProcessPendingTransitions(ctx);   // ここで 8 件 + Pop が処理される
    CHECK(ReentrantScene::s_entered == 9);
    CHECK(world.scenes.Depth() == 8u);             // 1 + 8 - 1(Pop)
  }

  /// 積み残したコマンドを次のシーンへ渡さない
  void TestQueuedCommandsDoNotCrossTheTransition() {
    GLFD::Test::BeginCase("T-ECS-25f: queued commands are discarded at the transition");
    World world;
    GLFD::GameContext ctx = world.Context();

    CHECK(world.scenes.ChangeScene(std::make_unique<WellBehavedScene>(1, 4)));
    world.scenes.ProcessPendingTransitions(ctx);

    const auto victims = world.registry.View<GLFD::Components::Health>();
    CHECK(victims.BaseSize() == 4u);
    if (victims.BaseSize() == 0u) { return; }
    const Entity victim = victims.BaseEntities()[0];
    CHECK(world.commands.Destroy(victim));
    CHECK(world.commands.Add<GLFD::Components::Damage>(victim, GLFD::Components::Damage{ 2.0f }));
    CHECK(world.commands.Size() == 2u);

    CHECK(world.scenes.ChangeScene(std::make_unique<WellBehavedScene>(2, 4)));
    world.scenes.ProcessPendingTransitions(ctx);

    // 渡してしまうと、次のシーンで「死んだ相手への 2 件」として取りこぼしに
    // 数えられ、**前のシーンの原因が次のシーンの ERROR として出る** (6.7)
    CHECK(world.commands.IsEmpty());
    CHECK(world.scenes.Report().discardedCommands == 2u);

    world.registry.ApplyCommands(world.commands);
    CHECK(world.commands.Report().Dropped() == 0u);
    CHECK(world.registry.AliveCount() == 4u);
  }

  /**
   * @brief 上に積んだシーンを抜けても、下のシーンのものが巻き添えにならない
   *
   * @details
   *  **この検査は変異 M4 が生き残ったから足した。** 「入場時の控えを取らない」
   *  という壊し方を入れても、最初は 1 件も落ちなかった。どのテストも
   *  **下に何も居ない状態でしか遷移していなかった**ので、控えの値が全部 0 で、
   *  0 と取り違えても差が出なかった。§4.2 の 6 例目である。
   *
   *  ここでは下のシーンが購読 1 件とエンティティ 4 体を持った状態で上を積む。
   *  控えが 0 のままなら、上を抜けた時点で下の分まで「外し忘れ」に数えられ、
   *  安全網が**下のシーンの購読を外してしまう**。
   */
  void TestPoppingDoesNotDisturbTheSceneBelow() {
    GLFD::Test::BeginCase("T-ECS-25g: popping an overlay leaves the scene below intact");
    World world;
    GLFD::GameContext ctx = world.Context();
    Counter below, above;

    // --- 下のシーン: 購読 1 件 + エンティティ 4 体 ---
    const SubscriptionId belowId =
        world.bus.Subscribe<GLFD::Events::HitEvent>(&below, &CountHit);
    CHECK(belowId.IsValid());
    CHECK(world.scenes.PushScene(std::make_unique<WellBehavedScene>(1, 4)));
    world.scenes.ProcessPendingTransitions(ctx);
    CHECK(world.bus.SubscriberCount() == 2u);   // 下の 1 件 + シーンの 1 件
    CHECK(world.registry.AliveCount() == 4u);
    world.scenes.ResetReport();

    // --- 上に積む。自分の購読とエンティティを持つ ---
    const std::uint32_t markAbove = world.bus.NextSerial();
    CHECK(world.scenes.PushScene(std::make_unique<LeakyScene>(&above)));
    world.scenes.ProcessPendingTransitions(ctx);
    CHECK(world.scenes.Depth() == 2u);
    CHECK(world.bus.SubscriberCount() == 3u);
    CHECK(world.registry.AliveCount() == 7u);   // 4 + 3。**どちらも 0 ではない**
    CHECK(world.bus.SubscriberCountSince(markAbove) == 1u);

    // --- 上を抜く。上は契約を破っているので安全網が働く ---
    CHECK(world.scenes.PopScene());
    world.scenes.ProcessPendingTransitions(ctx);

    CHECK(world.scenes.Depth() == 1u);
    CHECK(world.scenes.Report().leakedSubscriptions == 1u);   // 上の 1 件だけ
    // **下のシーンの購読 2 件は残っていなければならない**
    CHECK(world.bus.SubscriberCount() == 2u);
    // 上に積んだシーンのエンティティは掃かない(どれが誰のか分からない)。
    // 下の 4 体が消えていないことがここの要点である
    CHECK(world.registry.AliveCount() == 7u);
    CHECK(world.scenes.Report().sweptEntities == 0u);

    // 配信が下へ届く = 巻き添えで外されていない
    world.bus.Publish(GLFD::Events::HitEvent{ Entity::Invalid(), Entity::Invalid() });
    world.bus.DispatchAll();
    CHECK(below.hits == 1u);
    CHECK(above.hits == 0u);                     // 上はもう届かない
  }

  // ===========================================================================
  // T-ECS-26 `OnExit` の契約
  // ===========================================================================

  /// 契約を破ったシーンが検出される
  void TestBrokenContractIsDetected() {
    GLFD::Test::BeginCase("T-ECS-26a: a scene that does not clean up is reported");
    World world;
    GLFD::GameContext ctx = world.Context();
    Counter counter;

    CHECK(world.scenes.PushScene(std::make_unique<LeakyScene>(&counter)));
    world.scenes.ProcessPendingTransitions(ctx);
    CHECK(world.bus.SubscriberCount() == 1u);
    CHECK(world.registry.AliveCount() == 3u);
    world.scenes.ResetReport();

    CHECK(world.scenes.PopScene());
    world.scenes.ProcessPendingTransitions(ctx);

    const TransitionReport& report = world.scenes.Report();
    CHECK(report.leakedSubscriptions == 1u);   // 外し忘れを数えている
    CHECK(report.leakedEntities == 3u);        // 残したエンティティも数えている
    CHECK(report.sweptEntities == 3u);         // 一番下なので掃いた
    CHECK(report.HasProblem());
  }

  /// 検出するだけでなく、**use-after-free を実際に止めている**
  void TestTheSafetyNetActuallyRemovesTheSubscription() {
    GLFD::Test::BeginCase("T-ECS-26b: the safety net removes what OnExit forgot");
    World world;
    GLFD::GameContext ctx = world.Context();
    Counter counter;

    CHECK(world.scenes.PushScene(std::make_unique<LeakyScene>(&counter)));
    world.scenes.ProcessPendingTransitions(ctx);

    // まだ生きているので届く。**両方の項を 0 でない値にしておく** (4.2/4.6)
    world.bus.Publish(GLFD::Events::HitEvent{ Entity::Invalid(), Entity::Invalid() });
    world.bus.DispatchAll();
    CHECK(counter.hits == 1u);

    CHECK(world.scenes.PopScene());
    world.scenes.ProcessPendingTransitions(ctx);
    CHECK(world.bus.SubscriberCount() == 0u);

    // 外れているので、もう届かない。**届いたらそれは解放済みへの配信である**
    world.bus.Publish(GLFD::Events::HitEvent{ Entity::Invalid(), Entity::Invalid() });
    world.bus.DispatchAll();
    CHECK(counter.hits == 1u);
    CHECK(world.registry.AliveCount() == 0u);
  }

  /// 契約を守ったシーンでは報告が静かなまま (6.2: 沈黙を証拠にする)
  void TestAGoodSceneReportsNothing() {
    GLFD::Test::BeginCase("T-ECS-26c: a scene that keeps the contract reports nothing");
    World world;
    GLFD::GameContext ctx = world.Context();

    CHECK(world.scenes.PushScene(std::make_unique<WellBehavedScene>(3, 5)));
    world.scenes.ProcessPendingTransitions(ctx);
    world.scenes.ResetReport();

    CHECK(world.scenes.PopScene());
    world.scenes.ProcessPendingTransitions(ctx);

    const TransitionReport& report = world.scenes.Report();
    CHECK(report.leakedSubscriptions == 0u);
    CHECK(report.leakedEntities == 0u);
    CHECK(report.sweptEntities == 0u);
    CHECK(report.discardedCommands == 0u);
    CHECK(report.refused == 0u);
    CHECK(!report.HasProblem());
  }

  // ===========================================================================
  // 解除そのものの性質
  // ===========================================================================

  void TestUnsubscribeIsSafeToRepeat() {
    GLFD::Test::BeginCase("T-ECS-26d: double unsubscribe and stale handles are safe");
    World world;
    Counter a, b;

    const SubscriptionId first  = world.bus.Subscribe<GLFD::Events::HitEvent>(&a, &CountHit);
    const SubscriptionId second = world.bus.Subscribe<GLFD::Events::HitEvent>(&b, &CountHit);
    CHECK(first.IsValid());
    CHECK(second.IsValid());
    CHECK(!(first == second));          // **番号は使い回されない**
    CHECK(world.bus.SubscriberCount() == 2u);

    CHECK(world.bus.Unsubscribe(first));
    CHECK(!world.bus.Unsubscribe(first));          // 二重解除は false
    CHECK(!world.bus.Unsubscribe(SubscriptionId{}));  // 無効なハンドルも false
    CHECK(world.bus.SubscriberCount() == 1u);

    // 残った方だけに届く。**外した方に届いたら解放済みへの配信になる**
    world.bus.Publish(GLFD::Events::HitEvent{ Entity::Invalid(), Entity::Invalid() });
    world.bus.DispatchAll();
    CHECK(a.hits == 0u);
    CHECK(b.hits == 1u);
  }

  void TestRemovalKeepsRegistrationOrder() {
    GLFD::Test::BeginCase("T-ECS-26e: removing a subscriber keeps the others in order");
    World world;
    Counter counters[4];
    SubscriptionId ids[4];

    static int order[8];
    static int written;
    written = 0;
    for (int i = 0; i < 4; ++i) { order[i] = -1; }

    struct Recorder {
      static void Note(void* context, const GLFD::Events::HitEvent&) {
        order[written++] = static_cast<Counter*>(context)->hits;
      }
    };
    for (int i = 0; i < 4; ++i) {
      counters[i].hits = i + 1;         // **0 を混ぜない**。並びの差が消える (4.2)
      ids[i] = world.bus.Subscribe<GLFD::Events::HitEvent>(&counters[i], &Recorder::Note);
      CHECK(ids[i].IsValid());
    }

    CHECK(world.bus.Unsubscribe(ids[1]));     // 真ん中を抜く
    world.bus.Publish(GLFD::Events::HitEvent{ Entity::Invalid(), Entity::Invalid() });
    world.bus.DispatchAll();

    // 末尾と入れ替えていたら 1, 4, 3 になる。**詰めるので 1, 3, 4**
    CHECK(written == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 3);
    CHECK(order[2] == 4);
  }

  void TestSerialsIdentifyWhatAsceneAdded() {
    GLFD::Test::BeginCase("T-ECS-26f: serials separate one scene's subscriptions from another's");
    World world;
    Counter early, late;

    const SubscriptionId before = world.bus.Subscribe<GLFD::Events::HitEvent>(&early, &CountHit);
    CHECK(before.IsValid());

    const std::uint32_t mark = world.bus.NextSerial();
    const SubscriptionId after = world.bus.Subscribe<GLFD::Events::HitEvent>(&late, &CountHit);
    CHECK(after.IsValid());

    CHECK(world.bus.SubscriberCount() == 2u);
    CHECK(world.bus.SubscriberCountSince(mark) == 1u);   // 控えた後の 1 件だけ

    CHECK(world.bus.UnsubscribeSince(mark) == 1u);
    CHECK(world.bus.SubscriberCount() == 1u);

    // **控えより前の購読は残っている。** これが「下のシーンを巻き添えにしない」
    world.bus.Publish(GLFD::Events::HitEvent{ Entity::Invalid(), Entity::Invalid() });
    world.bus.DispatchAll();
    CHECK(early.hits == 1u);
    CHECK(late.hits == 0u);
  }

  /// `DestroyAll` は空き番号を二重に積まない (`IsAlive` が世代しか見ないため)
  void TestDestroyAllDoesNotDoubleCountFreeSlots() {
    GLFD::Test::BeginCase("T-ECS-26g: DestroyAll skips slots that are already free");
    World world;

    Entity made[6];
    for (int i = 0; i < 6; ++i) { made[i] = MakeEnemy(world.registry, 20.0f + i); }
    CHECK(world.registry.AliveCount() == 6u);

    // 先に穴を開けておく。**空き枠を「破棄」すると空き集合へ二重に積まれ、
    // `AliveCount` が桁下がりする**
    world.registry.DestroyEntity(made[1]);
    world.registry.DestroyEntity(made[4]);
    CHECK(world.registry.AliveCount() == 4u);

    CHECK(world.registry.DestroyAll() == 4u);
    CHECK(world.registry.AliveCount() == 0u);

    // 再利用しても数が合う
    for (int i = 0; i < 3; ++i) { (void)MakeEnemy(world.registry, 50.0f + i); }
    CHECK(world.registry.AliveCount() == 3u);
    CHECK(world.registry.DestroyAll() == 3u);
    CHECK(world.registry.AliveCount() == 0u);
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsSceneTransition (ECS 2-1)");

  TestLeavingRemovesTheSubscription();
  TestReenteringDoesNotDoubleSubscribe();
  TestEntitiesDoNotSurviveTheTransition();
  TestRoundTrip();
  TestRequestsMadeWhileEnteringAreDeferred();
  TestQueuedCommandsDoNotCrossTheTransition();
  TestPoppingDoesNotDisturbTheSceneBelow();

  TestBrokenContractIsDetected();
  TestTheSafetyNetActuallyRemovesTheSubscription();
  TestAGoodSceneReportsNothing();
  TestUnsubscribeIsSafeToRepeat();
  TestRemovalKeepsRegistrationOrder();
  TestSerialsIdentifyWhatAsceneAdded();
  TestDestroyAllDoesNotDoubleCountFreeSlots();

  return GLFD::Test::Summarize();
}
