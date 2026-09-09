/**
 * @file  EcsCommandBufferTests.cpp
 * @brief ECS 1-4: 構造変更の遅延 — T-ECS-4 / 12 / 13 / 14
 *
 * @details
 *  ## 中核は T-ECS-4 と T-ECS-12
 *   - T-ECS-4  反復しながら破棄と生成を積み、**反復中に dense が動かない**こと
 *   - T-ECS-12 適用の意味論(順序 / 二重積み / 死んだハンドル / 診断)
 *
 *  ## 押さえられなかったもの
 *  **積み place の 16 バイト境界そのものは、外から観測できない。** バッファの
 *  内部アドレスに触る手段が無いためである。代わりに次の 2 つで裏を取っている:
 *   - `alignof(T) <= alignof(PayloadSlot)` を `CommandBuffer::Add` の
 *     `static_assert` が**成立している場所で**固定している(要件書 §12)
 *   - 大きさの違う型を混ぜて積み、**値が 1 ビットも変わらずに往復する**ことを見る
 *     (`Wide` は 32 バイトで 2 スロットにまたがる)
 *
 *  境界が崩れれば `DynamicArray<T>` 側の `alignof(T)` 確保との食い違いで
 *  いずれ露見するが、**このテストが直接押さえているのは値の往復までである。**
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "ECS/CommandBuffer.h"
#include "ECS/Entity.h"
#include "ECS/Registry.h"
#include "ECS/View.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::ECS::ApplyReport;
using GLFD::ECS::CommandBuffer;
using GLFD::ECS::CommandKind;
using GLFD::ECS::DropReason;
using GLFD::ECS::Entity;
using GLFD::ECS::Registry;
using GLFD::Test::MockMemoryResource;

namespace {

  struct Health { int hp = 0; };
  struct Pickup { int value = 0; };
  struct Marker { int id = 0; };

  /// 32 バイト / 16 バイト境界。**複数スロットにまたがる経路**を踏むため
  struct alignas(16) Wide {
    float v[8];
  };

  [[nodiscard]] bool SameWide(const Wide& a, const Wide& b) {
    for (int i = 0; i < 8; ++i) {
      if (a.v[i] != b.v[i]) { return false; }
    }
    return true;
  }

  /// 報告の中に「この種類とこの理由」の明細があるか
  [[nodiscard]] bool HasRecord(const ApplyReport& report, CommandKind kind, DropReason reason) {
    for (std::uint32_t i = 0; i < report.RecordedCount(); ++i) {
      const GLFD::ECS::DroppedCommand& dropped = report.Recorded(i);
      if (dropped.kind == kind && dropped.reason == reason) { return true; }
    }
    return false;
  }

  // ===========================================================================
  // T-ECS-4 反復中の生成・破棄(**中核**)
  // ===========================================================================

  void TestStructuralChangesDuringIteration() {
    GLFD::Test::BeginCase("T-ECS-4: queuing while iterating leaves the dense arrays untouched");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    // 敵を 8 体
    Entity enemies[8];
    for (int i = 0; i < 8; ++i) {
      enemies[i] = registry.CreateEntity();
      CHECK(registry.AddComponent<Health>(enemies[i], i) != nullptr);
    }
    CHECK(registry.AliveCount() == 8u);

    auto pool = registry.View<Health>();

    // **反復を始める前の姿を控える**
    const Health* const  data   = pool.BaseComponents();
    const Entity* const  owners = pool.BaseEntities();
    const std::size_t    size   = pool.BaseSize();

    // 「敵が死んだら経験値を出す」の最小形。**反復しながら積む**
    int spawned = 0;
    for (std::size_t i = 0; i < size; ++i) {
      const Entity enemy = owners[i];
      if (data[i].hp % 2 != 0) {
        continue;
      }
      CHECK(commands.Destroy(enemy));

      // **生成は即時** (R-17)。index と generation がその場で確定する
      const Entity xp = registry.CreateEntity();
      CHECK(xp.IsValid());
      CHECK(registry.IsAlive(xp));
      CHECK(commands.Add(xp, Pickup{ data[i].hp * 10 }));
      ++spawned;
    }
    CHECK(spawned == 4);

    // **反復中に dense は 1 ミリも動いていないこと。** これが遅延の目的である。
    // View を取り直して**生きているプール**を見る(控えた View の BaseSize は
    // 構築時の範囲なので、ここでは歯にならない)
    CHECK(registry.View<Health>().BaseComponents() == data);
    CHECK(registry.View<Health>().BaseEntities() == owners);
    CHECK(registry.View<Health>().BaseSize() == size);

    // §2.2: 生成が即時なので、**成分を 1 つも持たないエンティティが今いる**。
    // 成分の反復には現れないが AliveCount には数えられる
    CHECK(registry.AliveCount() == 12u);
    CHECK(registry.View<Health>().BaseSize() == 8u);

    CHECK(commands.Size() == 8u);      // 破棄 4 + 追加 4
    CHECK(!commands.IsEmpty());

    registry.ApplyCommands(commands);

    const ApplyReport& report = commands.Report();
    CHECK(report.Applied() == 8u);
    CHECK(report.Dropped() == 0u);
    CHECK(!report.Truncated());

    // 適用後: 敵が 4 体消え、経験値が 4 つ成分を持っている
    CHECK(registry.AliveCount() == 8u);
    CHECK(registry.View<Health>().BaseSize() == 4u);
    CHECK(registry.View<Pickup>().BaseSize() == 4u);

    for (int i = 0; i < 8; ++i) {
      const bool shouldBeDead = (i % 2 == 0);
      CHECK(registry.IsAlive(enemies[i]) != shouldBeDead);
    }

    // **適用したバッファは空になる**(二重適用を構造的に防ぐ)
    CHECK(commands.IsEmpty());
    CHECK(commands.Size() == 0u);
  }

  // ===========================================================================
  // T-ECS-12 適用の意味論(§3 の表を全ケース固定する)
  // ===========================================================================

  /// **積んだ順に適用されること** (R-34)。逆順に適用する変異はここで落ちる
  void TestCommandsApplyInTheOrderTheyWereQueued() {
    GLFD::Test::BeginCase("T-ECS-12a: commands apply in the order they were queued (R-34)");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    // 同じ 2 つの操作を、順序だけ変えて 2 体に積む。**結果が違わなければならない**
    const Entity addThenRemove = registry.CreateEntity();
    const Entity removeThenAdd = registry.CreateEntity();

    CHECK(commands.Add(addThenRemove, Marker{ 1 }));
    CHECK(commands.Remove<Marker>(addThenRemove));

    CHECK(commands.Remove<Marker>(removeThenAdd));
    CHECK(commands.Add(removeThenAdd, Marker{ 2 }));

    registry.ApplyCommands(commands);

    // 追加してから削除 -> 残らない
    CHECK(registry.GetComponent<Marker>(addThenRemove) == nullptr);
    // 削除してから追加 -> 残る
    const Marker* const kept = registry.GetComponent<Marker>(removeThenAdd);
    CHECK(kept != nullptr);
    if (kept != nullptr) {
      CHECK(kept->id == 2);
    }

    // 持っていない成分の削除は**捨てない**(安全な no-op であり、意図は果たされている)
    CHECK(commands.Report().Applied() == 4u);
    CHECK(commands.Report().Dropped() == 0u);

    // 破棄と追加の順序も同じく効くこと
    {
      const Entity destroyThenAdd = registry.CreateEntity();
      CHECK(commands.Destroy(destroyThenAdd));
      CHECK(commands.Add(destroyThenAdd, Marker{ 3 }));
      registry.ApplyCommands(commands);

      CHECK(!registry.IsAlive(destroyThenAdd));
      CHECK(commands.Report().Applied() == 1u);
      CHECK(commands.Report().Dropped() == 1u);
      // **黙って捨てないこと。** 何が捨てられたかが分かる形で残っている
      CHECK(HasRecord(commands.Report(), CommandKind::Add, DropReason::DeadEntity));
    }
  }

  /// 死んだハンドルへの操作と、二重の破棄
  void TestOperationsOnDeadEntitiesAreDroppedAndReported() {
    GLFD::Test::BeginCase("T-ECS-12b: operations on dead entities are dropped and reported");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    const Entity victim = registry.CreateEntity();
    CHECK(registry.AddComponent<Health>(victim, 5) != nullptr);
    registry.DestroyEntity(victim);          // 反復の外なので即時 API を使ってよい
    CHECK(!registry.IsAlive(victim));

    CHECK(commands.Add(victim, Marker{ 1 }));
    CHECK(commands.Remove<Health>(victim));
    CHECK(commands.Destroy(victim));

    registry.ApplyCommands(commands);

    const ApplyReport& report = commands.Report();
    CHECK(report.Applied() == 0u);
    CHECK(report.Dropped() == 3u);
    CHECK(report.RecordedCount() == 3u);
    CHECK(!report.Truncated());
    CHECK(!report.IsQuiet());

    CHECK(HasRecord(report, CommandKind::Add, DropReason::DeadEntity));
    CHECK(HasRecord(report, CommandKind::Remove, DropReason::DeadEntity));
    // 破棄の二重積みは**理由を分けてある**(異常ではないため)
    CHECK(HasRecord(report, CommandKind::Destroy, DropReason::AlreadyDestroyed));

    // 生存管理が狂っていないこと
    CHECK(registry.AliveCount() == 0u);
  }

  void TestDoubleDestroyIsSafe() {
    GLFD::Test::BeginCase("T-ECS-12c: queuing the same destroy twice is safe");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    const Entity e = registry.CreateEntity();
    CHECK(registry.AddComponent<Health>(e, 1) != nullptr);

    CHECK(commands.Destroy(e));
    CHECK(commands.Destroy(e));
    registry.ApplyCommands(commands);

    CHECK(!registry.IsAlive(e));
    CHECK(registry.AliveCount() == 0u);
    CHECK(registry.View<Health>().BaseSize() == 0u);

    CHECK(commands.Report().Applied() == 1u);
    CHECK(commands.Report().Dropped() == 1u);
    CHECK(HasRecord(commands.Report(), CommandKind::Destroy, DropReason::AlreadyDestroyed));
  }

  void TestAddingAComponentTwiceIsReported() {
    GLFD::Test::BeginCase("T-ECS-12d: adding a component the entity already has is reported");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    const Entity e = registry.CreateEntity();
    CHECK(commands.Add(e, Marker{ 1 }));
    CHECK(commands.Add(e, Marker{ 2 }));
    registry.ApplyCommands(commands);

    const Marker* const kept = registry.GetComponent<Marker>(e);
    CHECK(kept != nullptr);
    if (kept != nullptr) {
      CHECK(kept->id == 1);              // 先に積んだ方が残る
    }
    CHECK(commands.Report().Applied() == 1u);
    CHECK(commands.Report().Dropped() == 1u);
    // **確保失敗と区別されていること。** どちらも AddComponent は nullptr を返す
    CHECK(HasRecord(commands.Report(), CommandKind::Add, DropReason::AlreadyPresent));
    CHECK(!HasRecord(commands.Report(), CommandKind::Add, DropReason::AllocationFailed));
  }

  void TestApplyingTwiceDoesNothingTheSecondTime() {
    GLFD::Test::BeginCase("T-ECS-12e: applying a buffer twice does not apply it twice");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    const Entity a = registry.CreateEntity();
    const Entity b = registry.CreateEntity();
    CHECK(commands.Add(a, Marker{ 1 }));
    CHECK(commands.Destroy(b));

    registry.ApplyCommands(commands);
    CHECK(commands.Report().Applied() == 2u);
    CHECK(registry.AliveCount() == 1u);

    // 2 回目は**何も起きない**。バッファが空になっているため
    registry.ApplyCommands(commands);
    CHECK(commands.Report().IsQuiet());
    CHECK(commands.Report().Applied() == 0u);
    CHECK(commands.Report().Dropped() == 0u);
    CHECK(registry.AliveCount() == 1u);
    CHECK(registry.View<Marker>().BaseSize() == 1u);
  }

  /// 値が往復すること。**大きさの違う型を混ぜる**(`Wide` は 2 スロットにまたがる)
  void TestPayloadsSurviveMixedSizes() {
    GLFD::Test::BeginCase("T-ECS-12f: queued values survive intact when sizes are mixed");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    Wide wide{};
    for (int i = 0; i < 8; ++i) {
      wide.v[i] = static_cast<float>(i) + 0.5f;
    }

    Entity entities[6];
    for (int i = 0; i < 6; ++i) {
      entities[i] = registry.CreateEntity();
    }

    // 小さい型と大きい型を交互に積み、**開始スロットが 0 でない状態**を作る
    CHECK(commands.Add(entities[0], Marker{ 11 }));
    CHECK(commands.Add(entities[1], wide));
    CHECK(commands.Add(entities[2], Health{ 22 }));
    CHECK(commands.Add(entities[3], wide));
    CHECK(commands.Add(entities[4], Pickup{ 33 }));
    CHECK(commands.Add(entities[5], wide));

    registry.ApplyCommands(commands);
    CHECK(commands.Report().Applied() == 6u);

    const Marker* const m = registry.GetComponent<Marker>(entities[0]);
    const Health* const h = registry.GetComponent<Health>(entities[2]);
    const Pickup* const p = registry.GetComponent<Pickup>(entities[4]);
    CHECK(m != nullptr && m->id == 11);
    CHECK(h != nullptr && h->hp == 22);
    CHECK(p != nullptr && p->value == 33);

    for (int i : { 1, 3, 5 }) {
      const Wide* const w = registry.GetComponent<Wide>(entities[i]);
      CHECK(w != nullptr);
      if (w != nullptr) {
        CHECK(SameWide(*w, wide));
        // 成分の側の境界も見ておく (DynamicArray<Wide> は alignof(Wide) で確保する)
        CHECK(reinterpret_cast<std::uintptr_t>(w) % alignof(Wide) == 0u);
      }
    }
  }

  /// 上限を超えた明細が**件数だけは正しい**こと
  void TestReportCountsEverythingEvenWhenDetailOverflows() {
    GLFD::Test::BeginCase("T-ECS-12g: the report counts every drop even when the detail overflows");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    const Entity dead = registry.CreateEntity();
    registry.DestroyEntity(dead);

    const std::uint32_t queued = ApplyReport::kMaxRecorded + 5u;
    for (std::uint32_t i = 0; i < queued; ++i) {
      CHECK(commands.Add(dead, Marker{ static_cast<int>(i) }));
    }
    registry.ApplyCommands(commands);

    const ApplyReport& report = commands.Report();
    CHECK(report.Dropped() == queued);                    // **件数は取りこぼさない**
    CHECK(report.RecordedCount() == ApplyReport::kMaxRecorded);
    CHECK(report.Truncated());                            // 明細だけが切れたと伝わる
  }

  // ===========================================================================
  // T-ECS-13 確保失敗
  // ===========================================================================

  void TestQueuingFailsCleanlyWhenAllocationFails() {
    GLFD::Test::BeginCase("T-ECS-13a: queuing reports allocation failure and stays consistent");

    MockMemoryResource mock;
    Registry           registry(&mock);

    const Entity e = registry.CreateEntity();

    CommandBuffer commands(&mock);
    // バッファの伸長を止める
    mock.SetFailAfter(mock.AllocateCalls());

    // 1 件目で既に配列を確保できない
    CHECK(!commands.Add(e, Wide{}));
    CHECK(commands.IsEmpty());
    CHECK(commands.Size() == 0u);
    CHECK(!commands.Destroy(e));
    CHECK(!commands.Remove<Marker>(e));
    CHECK(commands.IsEmpty());

    // **確保できるようになれば、そのまま使えること**(壊れた状態が残っていない)
    mock.ClearFailure();
    CHECK(commands.Add(e, Marker{ 7 }));
    CHECK(commands.Size() == 1u);
    registry.ApplyCommands(commands);
    CHECK(commands.Report().Applied() == 1u);

    const Marker* const kept = registry.GetComponent<Marker>(e);
    CHECK(kept != nullptr && kept->id == 7);
  }

  /// **適用中に失敗しても残りを捨てない** (§4 論点4)
  void TestApplyKeepsGoingAfterAFailure() {
    GLFD::Test::BeginCase("T-ECS-13b: a failure during apply does not abandon the rest");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    const Entity target  = registry.CreateEntity();
    const Entity doomed  = registry.CreateEntity();
    CHECK(registry.AddComponent<Health>(doomed, 1) != nullptr);

    // 新しい型のプールを作らせる = 疎配列の確保が要る -> ここを失敗させる
    CHECK(commands.Add(target, Wide{}));
    CHECK(commands.Destroy(doomed));       // **失敗の後ろに置く**

    mock.SetFailAfter(mock.AllocateCalls());
    registry.ApplyCommands(commands);
    mock.ClearFailure();

    const ApplyReport& report = commands.Report();
    CHECK(report.Applied() == 1u);
    CHECK(report.Dropped() == 1u);
    CHECK(HasRecord(report, CommandKind::Add, DropReason::AllocationFailed));

    // **後ろの Destroy は実行されている。** 止めてしまうと、確保が苦しいときに
    // 限ってメモリを手放す操作を取りやめることになる
    CHECK(!registry.IsAlive(doomed));
    CHECK(registry.GetComponent<Wide>(target) == nullptr);
    CHECK(registry.IsAlive(target));
  }

  /// N 回目から総当たりで注入し、**どこで落としてもクラッシュしない**こと
  void TestAllocationFailureInjectionIsSurvivable() {
    GLFD::Test::BeginCase("T-ECS-13c: injecting failure from the Nth allocation stays survivable");

    bool everyRunSurvived = true;

    for (int failAfter = 0; failAfter < 24; ++failAfter) {
      MockMemoryResource mock;
      {
        Registry      registry(&mock);
        CommandBuffer commands(&mock);

        Entity created[6];
        for (int i = 0; i < 6; ++i) {
          created[i] = registry.CreateEntity();
        }

        mock.SetFailAfter(failAfter);

        std::size_t queued = 0;
        for (int i = 0; i < 6; ++i) {
          if (commands.Add(created[i], Marker{ i })) { ++queued; }
          if (commands.Add(created[i], Wide{}))      { ++queued; }
          if (commands.Destroy(created[i]))          { ++queued; }
        }
        if (commands.Size() != queued) { everyRunSurvived = false; }

        registry.ApplyCommands(commands);

        const ApplyReport& report = commands.Report();
        // **すべてのコマンドが applied か dropped のどちらかに数えられること。**
        // 黙って消える経路が無いことが要件
        if (report.Applied() + report.Dropped() != static_cast<std::uint32_t>(queued)) {
          everyRunSurvived = false;
        }
        if (!commands.IsEmpty()) { everyRunSurvived = false; }

        // 明細は上限で切れ得るが、切れたなら必ずそう言うこと
        if (report.RecordedCount() > ApplyReport::kMaxRecorded) { everyRunSurvived = false; }
        if (report.Truncated() != (report.Dropped() > ApplyReport::kMaxRecorded)) {
          everyRunSurvived = false;
        }
      }
      mock.ClearFailure();

      if (mock.DoubleFree() || mock.UnknownFree()
          || mock.SizeMismatch() || mock.AlignmentMismatch()) {
        everyRunSurvived = false;
      }
      if (mock.LiveBytes() != 0u) { everyRunSurvived = false; }   // R-15
    }

    CHECK(everyRunSurvived);
  }

  // ===========================================================================
  // T-ECS-14 スレッド境界 (R-18 / N-ECS-1)
  // ===========================================================================

  void TestStructuralChangesAreBoundToOneThread() {
    GLFD::Test::BeginCase("T-ECS-14a: a buffer belongs to the thread that made it");

    MockMemoryResource mock;
    CommandBuffer      commands(&mock);

    // **機械的に検出できること。** Debug では同じ判定が assert になっている
    CHECK(commands.IsOwnerThread());

    bool workerSawItselfAsOwner = true;
    bool workerOwnsItsOwnBuffer = false;
    {
      std::thread worker([&mock, &commands, &workerSawItselfAsOwner, &workerOwnsItsOwnBuffer]() {
        workerSawItselfAsOwner = commands.IsOwnerThread();

        // フェーズ2への道が塞がっていないこと: **ワーカが自分のバッファを持てる**。
        // 適用はメインスレッドで行う形になる (R-18)
        CommandBuffer ownBuffer(&mock);
        workerOwnsItsOwnBuffer = ownBuffer.IsOwnerThread();
      });
      worker.join();
    }

    CHECK(!workerSawItselfAsOwner);      // ワーカから見れば所有者ではない
    CHECK(workerOwnsItsOwnBuffer);
    CHECK(commands.IsOwnerThread());     // メインスレッドの所有は変わらない
  }

  void TestParallelSystemsMayWriteComponentValues() {
    GLFD::Test::BeginCase("T-ECS-14b: parallel systems may write component values");

    MockMemoryResource mock;
    Registry           registry(&mock);

    for (int i = 0; i < 64; ++i) {
      const Entity e = registry.CreateEntity();
      CHECK(registry.AddComponent<Health>(e, 0) != nullptr);
    }

    auto                pool   = registry.View<Health>();
    Health* const       data   = pool.BaseComponents();
    const std::size_t   count  = pool.BaseSize();
    const Entity* const owners = pool.BaseEntities();
    const std::uint32_t aliveBefore = registry.AliveCount();

    // **値だけを書き換える。** 構造には触れない (R-18)
    {
      std::thread a([data]() { for (std::size_t i = 0;  i < 32u; ++i) { data[i].hp = 1; } });
      std::thread b([data]() { for (std::size_t i = 32; i < 64u; ++i) { data[i].hp = 2; } });
      a.join();
      b.join();
    }

    // 構造が動いていないこと(生きているプールを見る)
    CHECK(registry.View<Health>().BaseComponents() == data);
    CHECK(registry.View<Health>().BaseEntities() == owners);
    CHECK(registry.View<Health>().BaseSize() == count);
    CHECK(registry.AliveCount() == aliveBefore);

    bool allWritten = true;
    for (std::size_t i = 0; i < count; ++i) {
      const int expected = (i < 32u) ? 1 : 2;
      if (data[i].hp != expected) { allWritten = false; }
    }
    CHECK(allWritten);
  }

  // ===========================================================================
  // 使い回し(容量が保たれること)
  // ===========================================================================

  void TestBufferReusesItsCapacityAcrossFrames() {
    GLFD::Test::BeginCase("1-4: the buffer stops allocating once it reaches its high-water mark");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    Entity pool[16];
    for (int i = 0; i < 16; ++i) {
      pool[i] = registry.CreateEntity();
    }

    // 1 フレーム目: 伸ばしながら積む
    for (int i = 0; i < 16; ++i) {
      CHECK(commands.Add(pool[i], Marker{ i }));
    }
    registry.ApplyCommands(commands);

    // 2 フレーム目: **同じ量を積み直しても確保が増えないこと**
    const int allocationsAfterFirstFrame = mock.AllocateCalls();
    for (int i = 0; i < 16; ++i) {
      CHECK(commands.Remove<Marker>(pool[i]));
    }
    registry.ApplyCommands(commands);
    CHECK(mock.AllocateCalls() == allocationsAfterFirstFrame);

    // TryReserve は先に取っておくための口。**取れたなら確保はもう起きない**
    CHECK(commands.TryReserve(64, 64));
    const int allocationsAfterReserve = mock.AllocateCalls();
    for (int i = 0; i < 16; ++i) {
      CHECK(commands.Add(pool[i], Marker{ i }));
    }
    CHECK(mock.AllocateCalls() == allocationsAfterReserve);
    registry.ApplyCommands(commands);
    CHECK(commands.Report().Applied() == 16u);
  }

  void TestClearDiscardsWithoutApplying() {
    GLFD::Test::BeginCase("1-4: Clear discards the queue without applying it");

    MockMemoryResource mock;
    Registry           registry(&mock);
    CommandBuffer      commands(&mock);

    const Entity e = registry.CreateEntity();
    CHECK(commands.Destroy(e));
    CHECK(commands.Add(e, Marker{ 1 }));
    CHECK(!commands.IsEmpty());

    commands.Clear();

    CHECK(commands.IsEmpty());
    CHECK(commands.Report().IsQuiet());     // 報告も初期化される
    CHECK(registry.IsAlive(e));             // 何も適用されていない

    registry.ApplyCommands(commands);
    CHECK(commands.Report().IsQuiet());
    CHECK(registry.IsAlive(e));
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsCommandBuffer (ECS 1-4)");

  TestStructuralChangesDuringIteration();

  TestCommandsApplyInTheOrderTheyWereQueued();
  TestOperationsOnDeadEntitiesAreDroppedAndReported();
  TestDoubleDestroyIsSafe();
  TestAddingAComponentTwiceIsReported();
  TestApplyingTwiceDoesNothingTheSecondTime();
  TestPayloadsSurviveMixedSizes();
  TestReportCountsEverythingEvenWhenDetailOverflows();

  TestQueuingFailsCleanlyWhenAllocationFails();
  TestApplyKeepsGoingAfterAFailure();
  TestAllocationFailureInjectionIsSurvivable();

  TestStructuralChangesAreBoundToOneThread();
  TestParallelSystemsMayWriteComponentValues();

  TestBufferReusesItsCapacityAcrossFrames();
  TestClearDiscardsWithoutApplying();

  return GLFD::Test::Summarize();
}
