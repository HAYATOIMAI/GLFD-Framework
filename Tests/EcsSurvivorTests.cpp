/**
 * @file  EcsSurvivorTests.cpp
 * @brief ECS 1-8: 実データ疎通 — T-ECS-9 / T-ECS-22 / T-ECS-23 / T-ECS-24
 *
 * @details
 *  ## 本番のループをそのまま回す
 *  `RunSurvivorFrame`(シーンが呼ぶのと同じ関数)を、実物の `GameContext` で
 *  回している。**順序表をテスト側に写していない。** 1-6 で `kUpdateOrder` が
 *  テストから見えなかった (§19.7) ので、1-8 は表をヘッダに置いた。
 *
 *  ## 「クラッシュしない」を「動いた」と読まない
 *  段ごとの件数を `SurvivorCounts` から取り、**保存則**で確かめる。
 *   - 湧いた敵   = 倒した + 触れて消えた + 生きている
 *   - 撃った弾   = 当たって消えた + 期限切れ + 生きている
 *   - 作った経験値 = 回収 + 期限切れ + 生きている
 *   - 倒した数   = 作った経験値 + 上限で見送り + 作れなかった
 *  **二重破棄やリークがあると、どれかの等式が崩れる。**
 *
 *  ## どの敵が倒れるかは実行ごとに変わる
 *  `HitEvent` は複数のワーカーから発行されるので、配信の順は競合で決まる。
 *  1 発が 2 体に重なっていたとき、倒れるのは必ず 1 体だが**どちらかは決まらない**。
 *  個体を指定するケースは、候補が 1 体しかない配置にしてある。
 *
 *  ## Debug と Release の差 (受け入れ条件 3)
 *  Debug では `HitSystem` の照合 (R-43) と `View::IsStale()` の `assert` が生きている。
 *  **発火すればプロセスが止まり、このスイートは FAILED ではなく途中終了になる。**
 *  Release ではそれらが消えるので、同じ判定を `SurvivorCounts::gridMismatches` で
 *  数えて T-ECS-22c が見る。**どちらの構成でも意味のある検査になる。**
 *
 *  ## 入力を原点に寄せない (§12.1)
 *  原点は回収点なので、手で置く配置は原点から離し、セルの境目をまたがせる
 *  (`CELL_SIZE` 1.8 の両側)。
 *
 *  ## プロセスの終了
 *  末尾で `std::_Exit` を使う。`JobSystem` の停止経路の取りこぼし (§18.7) を
 *  **避けているだけ**で、直してはいない。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>

#include "Core/FailureGate.h"
#include "Core/GameContext.h"
#include "Core/StackAllocator.h"
#include "Core/StackResource.h"
#include "Core/SystemSchedule.h"
#include "ECS/CommandBuffer.h"
#include "ECS/Entity.h"
#include "ECS/Registry.h"
#include "ECS/View.h"
#include "Events/EventBus.h"
#include "Events/Events.h"
#include "Game/SurvivorComponents.h"
#include "Game/SurvivorLoop.h"
#include "Physics/CollisionComponents.h"

#include "TestHarness.h"

using GLFD::Components::Collider;
using GLFD::Components::Damage;
using GLFD::Components::Health;
using GLFD::Components::Lifetime;
using GLFD::Components::Pickup;
using GLFD::Components::Position;
using GLFD::ECS::Entity;
using GLFD::Game::SurvivorCounts;
using GLFD::Game::SurvivorParams;
using GLFD::Game::SurvivorState;

namespace {

  // ===========================================================================
  // 足場
  // ===========================================================================

  /// スイート全体で 1 本だけ使う。**ケースごとに作らない**(停止経路で固まる。§18.7)
  GLFD::Thread::JobSystem* g_jobs = nullptr;

  /// ウィンドウも DX11 も使わない。ループに要るものだけを実物で持つ
  class Harness {
  public:
    Harness()
      : m_mainStack(64u * 1024u * 1024u)
      , m_frameStack(16u * 1024u * 1024u)
      , m_global(m_mainStack)
      , m_frame(m_frameStack)
      , m_registry(&m_global)
      , m_commands(&m_global)
      , m_eventBus(&m_global) {}

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    /// **フレームメモリを毎フレーム戻す。** グリッドは毎フレームそこへ組まれる
    [[nodiscard]] GLFD::GameContext BeginFrame() {
      m_frameStack.Clear();
      return GLFD::GameContext{
          &m_global, &m_frame, g_jobs, &m_registry, &m_commands, &m_eventBus,
          nullptr,                 // grid: GridBuild が差し込む
          nullptr, nullptr, nullptr,
          0.016f,
          nullptr, 0.0f, nullptr, nullptr };
    }

    GLFD::ECS::Registry&      Registry() { return m_registry; }
    GLFD::ECS::CommandBuffer& Commands() { return m_commands; }
    GLFD::Events::EventBus&   EventBus() { return m_eventBus; }

  private:
    GLFD::Memory::StackAllocator m_mainStack;
    GLFD::Memory::StackAllocator m_frameStack;
    GLFD::Memory::StackResource  m_global;
    GLFD::Memory::StackResource  m_frame;
    GLFD::ECS::Registry          m_registry;
    GLFD::ECS::CommandBuffer     m_commands;
    GLFD::Events::EventBus       m_eventBus;
  };

  /// ループ 1 本ぶん。**購読者が `state` を参照で掴むので動かさない**
  struct World {
    Harness                 harness;
    SurvivorState           state;
    GLFD::Core::FrameReport report;

    explicit World(const SurvivorParams& params) {
      state.params = params;
      (void)GLFD::Game::AttachSurvivor(state, harness.Registry(), harness.Commands(),
                                 harness.EventBus());
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void Step() {
      GLFD::GameContext ctx = harness.BeginFrame();
      GLFD::Game::RunSurvivorFrame(state, ctx, report);
    }

    GLFD::ECS::Registry& Reg() { return harness.Registry(); }
  };

  struct Population {
    std::size_t enemies = 0;
    std::size_t bullets = 0;
    std::size_t pickups = 0;
  };

  [[nodiscard]] Population Count(GLFD::ECS::Registry& r) {
    Population p;
    p.enemies = r.View<Health>().BaseSize();
    p.bullets = r.View<Damage>().BaseSize();
    p.pickups = r.View<Pickup>().BaseSize();
    return p;
  }

  [[nodiscard]] std::uint32_t CreatedIn(const SurvivorCounts& c) {
    return c.enemiesSpawned + c.bulletsFired + c.pickupsCreated;
  }

  [[nodiscard]] std::uint32_t DestroyedIn(const SurvivorCounts& c) {
    return c.kills + c.enemiesReached + c.bulletsSpent + c.bulletsExpired
         + c.pickupsExpired + c.pickupsCollected;
  }

  /// 反復で 1 度でも出たか。**同じ index が 2 度出たら重複**
  std::uint8_t g_seen[GLFD::ECS::MaxEntities];

  /**
   * @brief `View<Position, Kind>` の反復が正確か
   *
   * @details
   *  - 出てきたものは全員生きている
   *  - 同じエンティティが 2 度出ない(swap-and-pop による重複の検出)
   *  - 件数が `Kind` のプールの大きさと一致する(飛ばしの検出)。
   *    このループでは `Kind` を持つものは必ず `Position` も持つ
   */
  template <class Kind>
  [[nodiscard]] bool ViewIsExact(GLFD::ECS::Registry& r) {
    bool        ok      = true;
    std::size_t yielded = 0;

    for (auto entry : r.View<Position, Kind>()) {
      const Entity e = std::get<0>(entry);
      if (!r.IsAlive(e))                         { ok = false; }
      if (e.Index() >= GLFD::ECS::MaxEntities)   { ok = false; continue; }
      if (g_seen[e.Index()] != 0u)               { ok = false; }
      g_seen[e.Index()] = 1u;
      ++yielded;
    }
    for (auto entry : r.View<Position, Kind>()) {
      const Entity e = std::get<0>(entry);
      if (e.Index() < GLFD::ECS::MaxEntities) { g_seen[e.Index()] = 0u; }
    }

    if (yielded != r.View<Kind>().BaseSize()) { ok = false; }
    return ok;
  }

  /**
   * @brief 破棄と再利用で dense の並びが index の並びから崩れているか
   * @note  **検査が空振りしていないことの証拠。** 並びが恒等のままなら、
   *        dense 添字と index を取り違える不具合があっても表に出ない (1-5)
   */
  [[nodiscard]] bool DenseOrderScrambled(GLFD::ECS::Registry& r) {
    const auto         view   = r.View<Position>();
    const Entity* const owners = view.BaseEntities();
    for (std::size_t i = 0; i < view.BaseSize(); ++i) {
      if (owners[i].Index() != i) { return true; }
    }
    return false;
  }

  // ===========================================================================
  // T-ECS-9 実データ疎通(中核)
  // ===========================================================================

  void TestOneLap() {
    GLFD::Test::BeginCase("T-ECS-9a: a bullet kills an enemy and experience appears where it died");

    SurvivorParams params = GLFD::Game::ManualSurvivorParams();
    params.pickupLifetime = 0.1f;   // 期限切れの段も同じケースで通す
    World w(params);

    // セルの境目をまたがせる: 37.7 / 1.8 = 20.94 (セル 20)、37.9 / 1.8 = 21.06 (セル 21)
    const Entity enemy  = GLFD::Game::SpawnEnemy(w.state, 37.7f, -21.9f, 0.0f, 0.0f);
    const Entity bullet = GLFD::Game::FireBullet(w.state, 37.9f, -21.9f, 0.0f, 0.0f);
    CHECK(enemy.IsValid() && bullet.IsValid());

    w.Step();
    const SurvivorCounts& f = w.state.thisFrame;

    CHECK(w.report.AllRan());
    // **各段が実際に起きたこと**
    CHECK(f.hitsDelivered == 1u);     // 衝突
    CHECK(f.bulletsSpent == 1u);
    CHECK(f.kills == 1u);             // 敵の破棄
    CHECK(f.pickupsCreated == 1u);    // 経験値の生成
    CHECK(f.hitsStale == 0u && f.hitsIgnored == 0u);

    CHECK(!w.Reg().IsAlive(enemy));
    CHECK(!w.Reg().IsAlive(bullet));

    // **経験値が、死んだ敵の位置に生成される**
    std::size_t pickups     = 0;
    bool        atTheEnemy  = false;
    Entity      experience  = Entity::Invalid();
    for (auto entry : w.Reg().View<Position, Pickup>()) {
      const Position& at = std::get<1>(entry);
      ++pickups;
      atTheEnemy = (at.x == 37.7f && at.y == -21.9f);
      experience = std::get<0>(entry);
    }
    CHECK(pickups == 1u);
    CHECK(atTheEnemy);
    CHECK(w.Reg().AliveCount() == 1u);

    // 破棄 2 件 + 経験値の成分 4 件。**捨てられたものは無い**
    const GLFD::ECS::ApplyReport& applied = w.harness.Commands().Report();
    CHECK(applied.Applied() == 6u);
    CHECK(applied.Dropped() == 0u);

    // 期限切れの段: 0.1 秒 = 7 フレーム
    for (int i = 0; i < 10; ++i) { w.Step(); }
    CHECK(w.state.total.pickupsExpired == 1u);
    CHECK(!w.Reg().IsAlive(experience));
    CHECK(w.Reg().AliveCount() == 0u);
  }

  void TestCollectAndReach() {
    GLFD::Test::BeginCase("T-ECS-9b: experience near the player is collected and enemies that reach it vanish");

    SurvivorParams params = GLFD::Game::ManualSurvivorParams();
    params.collectRadius = 6.0f;
    params.reachRadius   = 1.5f;
    params.pickupValue   = 3.0f;
    World w(params);

    // 原点から 3.86。回収の範囲 (6.3) の内側、触れる範囲 (2.0) の外側
    const Entity enemy  = GLFD::Game::SpawnEnemy(w.state, 3.1f, 2.3f, 0.0f, 0.0f);
    const Entity bullet = GLFD::Game::FireBullet(w.state, 3.3f, 2.3f, 0.0f, 0.0f);
    CHECK(enemy.IsValid() && bullet.IsValid());

    w.Step();
    CHECK(w.state.thisFrame.kills == 1u);
    CHECK(w.state.thisFrame.pickupsCreated == 1u);

    w.Step();
    CHECK(w.state.total.pickupsCollected == 1u);
    CHECK(w.state.experience == 3.0);
    CHECK(w.Reg().AliveCount() == 0u);

    // 触れて消える段: 原点から 1.08。触れる範囲 1.5 + 半径 0.5 の内側
    const Entity intruder = GLFD::Game::SpawnEnemy(w.state, 0.9f, -0.6f, 0.0f, 0.0f);
    CHECK(intruder.IsValid());
    w.Step();
    CHECK(w.state.thisFrame.enemiesReached == 1u);
    CHECK(w.state.thisFrame.pickupsCreated == 0u);   // 触れて消えた敵は経験値を出さない
    CHECK(!w.Reg().IsAlive(intruder));
  }

  void TestManyLaps() {
    GLFD::Test::BeginCase("T-ECS-9c: over many laps the population neither diverges nor runs dry");

    const SurvivorParams params = GLFD::Game::SmallSurvivorParams();
    World w(params);

    const int kFrames = 1200;
    const int kLate   = 600;   // ここから後を定常状態として見る

    std::size_t   minEnemiesLate = static_cast<std::size_t>(-1);
    std::size_t   minBulletsLate = static_cast<std::size_t>(-1);
    std::size_t   maxEnemiesLate = 0;
    std::size_t   maxBulletsLate = 0;
    std::size_t   maxPickupsLate = 0;
    std::uint32_t droppedTotal   = 0;
    bool          allRan         = true;

    for (int frame = 0; frame < kFrames; ++frame) {
      w.Step();
      allRan        = allRan && w.report.AllRan();
      droppedTotal += w.harness.Commands().Report().Dropped();

      if (frame >= kLate) {
        const Population p = Count(w.Reg());
        if (p.enemies < minEnemiesLate) { minEnemiesLate = p.enemies; }
        if (p.bullets < minBulletsLate) { minBulletsLate = p.bullets; }
        if (p.enemies > maxEnemiesLate) { maxEnemiesLate = p.enemies; }
        if (p.bullets > maxBulletsLate) { maxBulletsLate = p.bullets; }
        if (p.pickups > maxPickupsLate) { maxPickupsLate = p.pickups; }
      }
    }

    const SurvivorCounts& t   = w.state.total;
    const Population      now = Count(w.Reg());

    std::printf("    totals: spawned %u fired %u hits %u (ignored %u) kills %u xp %u "
                "collected %u xpExpired %u bulletsExpired %u reached %u\n",
                t.enemiesSpawned, t.bulletsFired, t.hitsDelivered, t.hitsIgnored, t.kills,
                t.pickupsCreated, t.pickupsCollected, t.pickupsExpired, t.bulletsExpired,
                t.enemiesReached);
    std::printf("    late window: enemies %zu..%zu bullets %zu..%zu pickups ..%zu\n",
                minEnemiesLate, maxEnemiesLate, minBulletsLate, maxBulletsLate, maxPickupsLate);

    CHECK(allRan);

    // **全段が実際に起きた**
    CHECK(t.enemiesSpawned > 0u);
    CHECK(t.bulletsFired > 0u);
    CHECK(t.hitsDelivered > 0u);
    CHECK(t.kills > 0u);
    CHECK(t.pickupsCreated > 0u);
    CHECK(t.pickupsCollected > 0u);
    CHECK(t.pickupsExpired > 0u);
    CHECK(t.bulletsExpired > 0u);
    CHECK(t.enemiesReached > 0u);

    // **保存則**。二重破棄やリークがあると崩れる
    CHECK(t.enemiesSpawned == t.kills + t.enemiesReached + now.enemies);
    CHECK(t.bulletsFired   == t.bulletsSpent + t.bulletsExpired + now.bullets);
    CHECK(t.pickupsCreated == t.pickupsCollected + t.pickupsExpired + now.pickups);
    CHECK(t.kills          == t.pickupsCreated + t.pickupsSkippedAtCap + t.pickupsLost);

    // **枯渇しない**
    CHECK(minEnemiesLate > 0u);
    CHECK(minBulletsLate > 0u);
    // **発散しない。** 上限に張り付いていたら、それは上限が止めているだけ
    CHECK(maxEnemiesLate < params.maxEnemies);
    CHECK(maxBulletsLate < params.maxBullets);
    CHECK(maxPickupsLate < params.maxPickups);

    // 1 体への破棄は 1 回 (論点3 の c)。**このループでは二重破棄が構造的に 0 件**。
    // ただし「重複が 1 度も起きなかったので 0 件」では検査にならない。
    // 使い切った弾 / 倒れ済みの敵への命中が**実際に届き、値で素通りした**こと
    CHECK(t.hitsIgnored > 0u);
    CHECK(droppedTotal == 0u);
    CHECK(t.createFailures == 0u && t.buildFailures == 0u);
    CHECK(t.queueFailures == 0u && t.orphans == 0u);
  }

  // ===========================================================================
  // T-ECS-22 破棄が起きても壊れない(このフェーズの本題)
  // ===========================================================================

  void TestViewsStayExactUnderChurn() {
    GLFD::Test::BeginCase("T-ECS-22a: views stay exact while destruction reshuffles the dense arrays");

    World w(GLFD::Game::SmallSurvivorParams());

    bool exact           = true;
    bool scrambled       = false;
    int  framesDestroyed = 0;

    for (int frame = 0; frame < 900; ++frame) {
      w.Step();
      exact = exact && ViewIsExact<Health>(w.Reg())
                    && ViewIsExact<Damage>(w.Reg())
                    && ViewIsExact<Pickup>(w.Reg());
      scrambled = scrambled || DenseOrderScrambled(w.Reg());
      if (DestroyedIn(w.state.thisFrame) > 0u) { ++framesDestroyed; }
    }

    CHECK(exact);
    // **空振りしていないこと**: 破棄が起き、実際に並びが崩れた
    CHECK(framesDestroyed > 0);
    CHECK(scrambled);
  }

  void TestDeadHandleStaysDead() {
    GLFD::Test::BeginCase("T-ECS-22b: a dead enemy's handle stays dead after its index is reused");

    World w(GLFD::Game::ManualSurvivorParams());
    GLFD::ECS::Registry& reg = w.Reg();

    const Entity enemy  = GLFD::Game::SpawnEnemy(w.state, 21.1f, 13.7f, 0.0f, 0.0f);
    const Entity bullet = GLFD::Game::FireBullet(w.state, 21.3f, 13.7f, 0.0f, 0.0f);
    CHECK(enemy.IsValid() && bullet.IsValid());

    w.Step();
    // **生きたハンドルなら解決される**(後半の「死んだハンドルは素通り」と対にする)
    CHECK(w.state.total.kills == 1u);
    CHECK(!reg.IsAlive(enemy));
    CHECK(!reg.IsAlive(bullet));

    // 死んだ敵の index を別のエンティティに再利用させる
    Entity reused = Entity::Invalid();
    for (int i = 0; i < 8 && !reused.IsValid(); ++i) {
      const Entity e = reg.CreateEntity();
      if (e.IsValid() && e.Index() == enemy.Index()) { reused = e; }
    }
    CHECK(reused.IsValid());
    CHECK(reused.Generation() != enemy.Generation());
    CHECK(!reg.IsAlive(enemy));
    CHECK(reg.IsAlive(reused));

    // **新しい住人に、古いハンドルが信じられたら削られる体を与える**
    CHECK(reg.AddComponent<Position>(reused, 21.1f, 13.7f, 0.0f, 0.0f) != nullptr);
    CHECK(reg.AddComponent<Health>(reused, 5.0f) != nullptr);
    CHECK(reg.AddComponent<Collider>(reused, 0.5f) != nullptr);

    // 遠くの生きた弾と、**死んだ敵のハンドル**を組にした命中を送る
    const Entity bullet2 = GLFD::Game::FireBullet(w.state, -35.3f, 28.9f, 0.0f, 0.0f);
    CHECK(bullet2.IsValid());
    w.harness.EventBus().Publish(GLFD::Events::HitEvent{ bullet2, enemy });

    w.Step();
    const SurvivorCounts& f = w.state.thisFrame;
    CHECK(f.hitsDelivered == 1u);
    CHECK(f.hitsStale == 1u);        // 世代で弾いた
    CHECK(f.kills == 0u);
    CHECK(f.bulletsSpent == 0u);

    const Health* const occupant = reg.GetComponent<Health>(reused);
    CHECK(occupant != nullptr && occupant->current == 5.0f);   // 新しい住人は無傷
    CHECK(reg.IsAlive(bullet2));
    const Lifetime* const life = reg.GetComponent<Lifetime>(bullet2);
    CHECK(life != nullptr && life->remaining > 0.0f);          // 弾も使い切られていない
  }

  void TestBuildStampHoldsUnderChurn() {
    GLFD::Test::BeginCase("T-ECS-22c: the target grid's build stamp matches on every frame of churn");

    World w(GLFD::Game::SmallSurvivorParams());
    const std::uint32_t versionAtStart = w.Reg().StructureVersion();

    int framesWithHits = 0;
    for (int frame = 0; frame < 600; ++frame) {
      w.Step();
      if (w.state.thisFrame.hitsDelivered > 0u) { ++framesWithHits; }
    }

    CHECK(w.state.total.gridMismatches == 0u);
    // **空振りしていないこと**: 構造は実際に変わり続け、照合は実際に走った
    CHECK(w.Reg().StructureVersion() != versionAtStart);
    CHECK(framesWithHits > 0);
  }

  void TestAliveCountUnderMixedChurn() {
    GLFD::Test::BeginCase("T-ECS-22d: AliveCount agrees with the views while creation and destruction mix");

    World w(GLFD::Game::SmallSurvivorParams());

    int mismatches  = 0;
    int mixedFrames = 0;
    for (int frame = 0; frame < 600; ++frame) {
      w.Step();
      const Population p = Count(w.Reg());
      const std::size_t expected = p.enemies + p.bullets + p.pickups + w.state.total.orphans;
      if (w.Reg().AliveCount() != expected) { ++mismatches; }
      if (CreatedIn(w.state.thisFrame) > 0u && DestroyedIn(w.state.thisFrame) > 0u) {
        ++mixedFrames;
      }
    }

    CHECK(mismatches == 0);
    // 生成と破棄が同じフレームに混ざっていたこと(混ざらなければ検査にならない)
    CHECK(mixedFrames > 0);
  }

  /**
   * @brief T-ECS-22e: 標的のプールと位置のプールの並びが違っても、命中が見つかること
   *
   * @details
   *  **変異テストで開いた穴(1-8 の M7b)。** 引く側の `View` から `Health` を
   *  抜くと基準プールが `Position` に変わり、グリッドの番号(`Health` の dense
   *  添字)を**別のプールの添字として読む**。1-5 で指摘された罠そのものである。
   *
   *  **最初に書いた検査は歯が無かった。** 破棄を回して並びをずらし、
   *  「古いハンドルの命中が 0 件」を見ていたが、M7b でも通った。
   *  番号の読み違いは**古い命中ではなく、命中の見逃しとして現れる**。
   *  グリッドは弾の近くの敵の番号を返すが、読み違えた持ち主は遠くにいるので
   *  距離の判定で弾かれ、**イベントがそもそも出ない**。9c の保存則は
   *  「どれだけ当たったか」を問わず、「0 より多い」も偶然の一致で満たされた。
   *
   *  **既存の 9a / 22b が気づかなかった理由は §12.1 と同じ。** 敵を先に作るので
   *  `Health` と `Position` の dense の並びが先頭で一致し、読み違えても同じ
   *  エンティティを指していた。**差が出ない入力**だった。
   *
   *  ここでは遠くのおとりを**先に**作り、`Position` の 0 番をおとりに、
   *  `Health` の 0 番を敵に取らせる(前提そのものを CHECK する)。
   *  正しければグリッドの 0 番は敵を指して命中し、読み違えるとおとりを指して
   *  見逃す。
   */
  void TestHitsNameTheEnemyTheGridMeant() {
    GLFD::Test::BeginCase("T-ECS-22e: a hit is found when the target pool and the position pool are ordered differently");

    World w(GLFD::Game::ManualSurvivorParams());
    GLFD::ECS::Registry& reg = w.Reg();

    const Entity decoy  = GLFD::Game::FireBullet(w.state, -41.3f, 33.7f, 0.0f, 0.0f);
    // セルの境目をまたがせる: 26.9 / 1.8 = 14.94 (セル 14)、27.1 / 1.8 = 15.06 (セル 15)
    const Entity enemy  = GLFD::Game::SpawnEnemy(w.state, 26.9f, -15.1f, 0.0f, 0.0f);
    const Entity bullet = GLFD::Game::FireBullet(w.state, 27.1f, -15.1f, 0.0f, 0.0f);
    CHECK(decoy.IsValid() && enemy.IsValid() && bullet.IsValid());

    // **前提**: 2 つのプールの 0 番が別のエンティティを指している
    {
      const auto targets = reg.View<Health>();
      const auto placed  = reg.View<Position>();
      CHECK(targets.BaseSize() == 1u && placed.BaseSize() == 3u);
      CHECK(targets.BaseEntities()[0] == enemy);
      CHECK(placed.BaseEntities()[0] == decoy);
    }

    w.Step();
    CHECK(w.state.thisFrame.hitsDelivered == 1u);
    CHECK(w.state.thisFrame.kills == 1u);
    CHECK(!reg.IsAlive(enemy));
    CHECK(!reg.IsAlive(bullet));
    CHECK(reg.IsAlive(decoy));     // 読み違えた先は無傷
  }

  /**
   * @brief T-ECS-22f: 同じフレームに 2 つのシステムが消したがっても、破棄は 1 回
   *
   * @details
   *  **変異テストで開いた穴(1-8 の M9)。** `Collect` の「積み済みなら素通り」を
   *  消しても 9c は通った。`Small` では回収範囲に入った経験値は見えた最初の
   *  フレームで回収されるので、**範囲の中で期限が切れる**ことが起きないため。
   *
   *  期限を 1 フレームより短くし、範囲の中に落とす。見えた最初のフレームで
   *  `Lifetime`(先に走る)と `Collect` の両方が消したがる。
   */
  void TestExpiringInsideTheCollectRadiusDestroysOnce() {
    GLFD::Test::BeginCase("T-ECS-22f: a pickup that expires inside the collect radius is destroyed once");

    SurvivorParams params  = GLFD::Game::ManualSurvivorParams();
    params.collectRadius   = 6.0f;
    params.pickupLifetime  = 0.001f;   // dt (0.016) より短い: 見えた最初のフレームで切れる
    World w(params);

    // 原点から 3.86: 回収の範囲 (6.3) の内側
    const Entity enemy  = GLFD::Game::SpawnEnemy(w.state, 3.1f, 2.3f, 0.0f, 0.0f);
    const Entity bullet = GLFD::Game::FireBullet(w.state, 3.3f, 2.3f, 0.0f, 0.0f);
    CHECK(enemy.IsValid() && bullet.IsValid());

    w.Step();                                    // 撃破。経験値は積まれただけで、まだ見えない
    CHECK(w.state.thisFrame.pickupsCreated == 1u);

    w.Step();                                    // 見えた: 期限切れと回収が同じフレーム
    const SurvivorCounts& f = w.state.thisFrame;
    CHECK(f.pickupsExpired == 1u);               // `Lifetime` が先に積んだ
    CHECK(f.pickupsExpired + f.pickupsCollected == 1u);
    CHECK(w.harness.Commands().Report().Dropped() == 0u);
    CHECK(w.Reg().AliveCount() == 0u);
  }

  // ===========================================================================
  // T-ECS-23 1 フレーム遅れ(**バグに見えるが仕様**)
  // ===========================================================================

  /**
   * @brief T-ECS-23: 倒した直後の経験値は、次のフレームで拾われる
   *
   * @details
   *  ## これは欠陥ではない (§5.3)
   *  **回収範囲の中で倒したのに、そのフレームでは拾われない。** 一見バグに見えるが、
   *  構造変更を遅延した設計の帰結として受け入れたものである。
   *
   *  経験値のエンティティは配信中に `CreateEntity` でその場に作られる (R-17) が、
   *  **成分(位置と `Pickup`)が付くのは `ApplyCommands` の後**である。回収 (`Collect`)
   *  は適用より前に走るので、そのフレームの回収からは見えない。代わりに、反復中に
   *  dense 配列が動かないことが構造的に保証される。
   *
   *  **1 フレームで済んでいるのは、配信を適用の前に置いたから**である(論点3 の a)。
   *  後ろに置くと 2 フレームになる(変異 M1)。
   *
   *  このテストが落ちたら「即時に拾えるようにしよう」ではなく、
   *  **何が遅延を崩したのか**を調べること。
   */
  void TestPickupIsCollectedOneFrameAfterTheKill() {
    GLFD::Test::BeginCase("T-ECS-23: experience dropped inside the collect radius is collected one frame later (by design)");

    SurvivorParams params = GLFD::Game::ManualSurvivorParams();
    params.collectRadius  = 6.0f;
    params.pickupLifetime = 10.0f;
    World w(params);

    // 原点から 3.86: 回収の範囲 (6.3) の内側
    const Entity enemy  = GLFD::Game::SpawnEnemy(w.state, 3.1f, 2.3f, 0.0f, 0.0f);
    const Entity bullet = GLFD::Game::FireBullet(w.state, 3.3f, 2.3f, 0.0f, 0.0f);
    CHECK(enemy.IsValid() && bullet.IsValid());

    // --- フレーム N: 倒した ---------------------------------------------------
    w.Step();
    CHECK(w.state.thisFrame.kills == 1u);
    CHECK(w.state.thisFrame.pickupsCreated == 1u);
    CHECK(w.state.thisFrame.pickupsCollected == 0u);   // **このフレームでは拾われない**

    // 経験値はフレーム N の終わりに存在し、しかも範囲の中にいる。
    // 拾われなかったのは遠いからではなく、成分が付いたのが回収の後だったから
    std::size_t inside = 0;
    for (auto entry : w.Reg().View<Position, Pickup>()) {
      const Position& at = std::get<1>(entry);
      if (at.x * at.x + at.y * at.y < params.collectRadius * params.collectRadius) { ++inside; }
    }
    CHECK(inside == 1u);

    // --- フレーム N+1: 拾われる -----------------------------------------------
    w.Step();
    CHECK(w.state.thisFrame.pickupsCollected == 1u);
    CHECK(w.Reg().AliveCount() == 0u);
  }

  // ===========================================================================
  // T-ECS-24 上限
  // ===========================================================================

  /**
   * @brief T-ECS-24a: レジストリが満杯でも落ちず、報告は 1 回、撃破は続く
   *
   * @details
   *  `CreateEntity` は上限で `Invalid()` を返す (R-10)。ループは
   *   - その回の生成を飛ばし(作りかけを残さない)
   *   - **数える**(黙って少ない数で続けない)
   *   - 門で**始まりと終わりだけ**を出す (R-46)
   *  経験値が作れなくても**敵は倒れる**。失われた経験値は `pickupsLost` に残る。
   */
  void TestRegistryFullIsSurvivable() {
    GLFD::Test::BeginCase("T-ECS-24a: a full registry skips creation, reports once, and keeps resolving kills");

    SurvivorParams params   = GLFD::Game::ManualSurvivorParams();
    params.spawnEveryFrames = 1;
    params.enemiesPerSpawn  = 1;
    params.spawnRadius      = 45.0f;
    params.spawnArc         = 0.5f;
    params.enemySpeed       = 0.0f;
    World w(params);
    GLFD::ECS::Registry& reg = w.Reg();

    const Entity enemy  = GLFD::Game::SpawnEnemy(w.state, 33.1f, -18.7f, 0.0f, 0.0f);
    const Entity bullet = GLFD::Game::FireBullet(w.state, -29.3f, 21.1f, 0.0f, 0.0f);   // まだ遠い
    CHECK(enemy.IsValid() && bullet.IsValid());

    std::size_t filled = 0;
    while (reg.CreateEntity().IsValid()) { ++filled; }
    CHECK(filled > 0u);
    CHECK(reg.AliveCount() == GLFD::ECS::MaxEntities);

    GLFD::Core::FailureGate gate;
    int  started   = 0;
    int  recovered = 0;
    bool allRan    = true;
    const auto frame = [&]() {
      w.Step();
      allRan = allRan && w.report.AllRan();
      const GLFD::Core::FailureGate::Change change =
          GLFD::Game::ObserveCreationFailures(w.state.thisFrame, gate);
      if (change == GLFD::Core::FailureGate::Change::Started)   { ++started; }
      if (change == GLFD::Core::FailureGate::Change::Recovered) { ++recovered; }
    };

    // 5 フレーム: 毎フレーム湧かせようとして失敗する。**報告は 1 回だけ**
    for (int i = 0; i < 5; ++i) { frame(); }
    CHECK(w.state.total.createFailures == 5u);
    CHECK(started == 1 && recovered == 0);
    CHECK(reg.AliveCount() == GLFD::ECS::MaxEntities);

    // 弾を敵に重ねる。**満杯のままでも撃破は解決される**
    Position* const at = reg.GetComponent<Position>(bullet);
    CHECK(at != nullptr);
    if (at != nullptr) { at->x = 33.3f; at->y = -18.7f; }
    frame();
    {
      const SurvivorCounts& f = w.state.thisFrame;
      CHECK(f.kills == 1u);
      CHECK(f.pickupsLost == 1u);        // 経験値の置き場が無い。**数えて残す**
      CHECK(f.createFailures == 2u);     // 湧かせる分と経験値の分
      CHECK(started == 1 && recovered == 0);
      CHECK(!reg.IsAlive(enemy) && !reg.IsAlive(bullet));
      CHECK(reg.AliveCount() == GLFD::ECS::MaxEntities - 2u);
    }

    // 空いた 2 枠で次の生成が通り、**直ったことを 1 回だけ**出す
    frame();
    CHECK(w.state.thisFrame.enemiesSpawned == 1u);
    CHECK(w.state.thisFrame.createFailures == 0u);
    CHECK(recovered == 1);

    CHECK(allRan);
    const SurvivorCounts& t = w.state.total;
    CHECK(t.kills == t.pickupsCreated + t.pickupsSkippedAtCap + t.pickupsLost);
  }

  /**
   * @brief T-ECS-24b: 経験値の上限は、まだ適用されていない分も数える
   *
   * @details
   *  同じフレームに何体倒しても、経験値の成分が付くのは適用の後なので、
   *  **プールの大きさだけを見ると同じフレームの分が数えられず上限を越える**。
   *  積んだ数を足して判定していることを確かめる。上限で見送った数も 0 ではない
   *  (見送りが起きなければ検査にならない)。
   */
  void TestPickupCapCountsWhatIsStillQueued() {
    GLFD::Test::BeginCase("T-ECS-24b: the pickup cap counts experience that is queued but not yet applied");

    SurvivorParams params = GLFD::Game::ManualSurvivorParams();
    params.maxPickups     = 2;
    World w(params);

    const float spots[4][2] = { { 25.1f, 17.3f }, { -24.7f, 16.9f }, { 23.9f, -18.1f }, { -26.3f, -17.7f } };
    for (const auto& spot : spots) {
      CHECK(GLFD::Game::SpawnEnemy(w.state, spot[0], spot[1], 0.0f, 0.0f).IsValid());
      CHECK(GLFD::Game::FireBullet(w.state, spot[0] + 0.2f, spot[1], 0.0f, 0.0f).IsValid());
    }

    w.Step();
    const SurvivorCounts& f = w.state.thisFrame;
    CHECK(f.kills == 4u);
    CHECK(f.pickupsCreated == 2u);
    CHECK(f.pickupsSkippedAtCap == 2u);
    CHECK(Count(w.Reg()).pickups == 2u);
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsSurvivor (ECS 1-8)");

  // **意図的に解放しない。** g_jobs の説明を参照
  GLFD::Thread::JobSystem jobs;
  g_jobs = &jobs;

  TestOneLap();
  TestCollectAndReach();
  TestManyLaps();

  TestViewsStayExactUnderChurn();
  TestDeadHandleStaysDead();
  TestBuildStampHoldsUnderChurn();
  TestAliveCountUnderMixedChurn();
  TestHitsNameTheEnemyTheGridMeant();
  TestExpiringInsideTheCollectRadiusDestroysOnce();

  TestPickupIsCollectedOneFrameAfterTheKill();

  TestRegistryFullIsSurvivable();
  TestPickupCapCountsWhatIsStillQueued();

  const int code = GLFD::Test::Summarize();
  std::fflush(stdout);
  // **`return code` にしない。** `JobSystem` の停止経路で固まることがある (§18.7)
  std::_Exit(code);
}
