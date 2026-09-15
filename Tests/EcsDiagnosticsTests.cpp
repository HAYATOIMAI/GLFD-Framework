/**
 * @file  EcsDiagnosticsTests.cpp
 * @brief ECS 1-6: 診断 — T-ECS-17
 *
 * @details
 *  ## 何を固定するか
 *   - **同じ失敗が続いてもログが溢れない**(`FailureGate` が状態の変わり目
 *     だけを通す)
 *   - **失敗していないときに何も出さない**
 *   - **確保に失敗したグリッドが観測できる形で失敗する**(投げず、
 *     `IsReady()` が false になり、そのまま触っても安全)
 *
 *  ## 押さえられないもの
 *  **`RenderSystem` と `GridBuildSystem` そのものはここから呼べない。**
 *  前者は `DX11Renderer` の実体を、後者は `GameContext`(= `d3d11.h` まで
 *  引き込む)と `JobSystem` を要求する。
 *
 *  そこで**根っこの側**を押さえている。両者の確保失敗はどちらも
 *  「`IMemoryResource` が `nullptr` を返す」に帰着し、
 *   - グリッド側は `SpatialHashGrid` の `IsReady()`(このファイルで検査)
 *   - 描画側は `DynamicArray::TryResize` の戻り値(`DynamicArrayTests` が既に検査)
 *  が入口である。**そこから先の「戻り値を見て報告する」形は実機の動作確認が
 *  受け持つ**(1-6 の報告に Game.log を添える)。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "Core/FailureGate.h"
#include "Core/MemoryResource.h"
#include "ECS/CommandBuffer.h"
#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "ECS/Registry.h"
#include "Game/CommandReportGate.h"
#include "Physics/SpatialHashGrid.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::Core::FailureGate;
using GLFD::Physics::SpatialHashGrid;
using GLFD::Test::MockMemoryResource;
using Change = GLFD::Core::FailureGate::Change;

namespace {

  /**
   * @brief **N 回目の確保だけ**を失敗させる資源
   *
   * @details
   *  `MockMemoryResource::SetFailAfter(n)` は「n 回目以降を全部」失敗させる。
   *  それだと「2 本の配列のうち 1 本目だけ失敗する」場合を作れず、
   *  **1 本目の検査を消す変異を 2 本目の検査が代わりに止めてしまう**
   *  (1-6 の変異 M8 で実際に見逃した)。
   *
   *  @note 共有の `MockMemoryResource` には手を入れない。あちらを変えると
   *        JsonArena の Debug 844 / Release 847 が動く恐れがある(あのヘッダの
   *        @warning を参照)。ここだけで完結させる
   */
  class FailOnlyNth final : public GLFD::Memory::IMemoryResource {
  public:
    explicit FailOnlyNth(int nth) : m_nth(nth) {}

    void* Allocate(size_t size, size_t alignment) override {
      const int index = m_calls++;
      if (index == m_nth) { return nullptr; }
      return m_inner.Allocate(size, alignment);
    }
    void Deallocate(void* p, size_t size, size_t alignment) override {
      m_inner.Deallocate(p, size, alignment);
    }

    [[nodiscard]] int    Calls()     const { return m_calls; }
    [[nodiscard]] size_t LiveBytes() const { return m_inner.LiveBytes(); }

  private:
    MockMemoryResource m_inner;
    int                m_nth   = -1;
    int                m_calls = 0;
  };

  // ===========================================================================
  // T-ECS-17a 抑制の形
  // ===========================================================================

  void TestGateStaysQuietWhileNothingChanges() {
    GLFD::Test::BeginCase("T-ECS-17a: the gate is silent while the state does not change");

    FailureGate gate;

    // 平常時は**一度も通さない**。これが「失敗していないときに余計な出力をしない」
    for (int i = 0; i < 100; ++i) {
      CHECK_QUIET(gate.Observe(false) == Change::None);
    }
    ++GLFD::Test::g_checkCount;
    CHECK(!gate.IsFailing());
    CHECK(gate.TotalFailures() == 0u);

    // 失敗し始めた 1 フレーム目だけ通る
    CHECK(gate.Observe(true) == Change::Started);
    CHECK(gate.IsFailing());

    // **続いている間は黙る。** 60fps で 300 フレーム = 5 秒ぶん
    for (int i = 0; i < 300; ++i) {
      CHECK_QUIET(gate.Observe(true) == Change::None);
    }
    ++GLFD::Test::g_checkCount;

    CHECK(gate.StreakLength() == 301u);
    CHECK(gate.TotalFailures() == 301u);

    // 直った 1 フレーム目だけ通る
    CHECK(gate.Observe(false) == Change::Recovered);
    CHECK(!gate.IsFailing());
    CHECK(gate.StreakLength() == 0u);
    CHECK(gate.LastStreakLength() == 301u);     // **沈黙の長さが残る**
    CHECK(gate.TotalFailures() == 301u);

    // 直った後も黙る
    for (int i = 0; i < 100; ++i) {
      CHECK_QUIET(gate.Observe(false) == Change::None);
    }
    ++GLFD::Test::g_checkCount;
  }

  void TestGateCountsEveryEpisode() {
    GLFD::Test::BeginCase("T-ECS-17b: the gate reports every episode, not just the first");

    FailureGate gate;

    // **「初回だけ」ではない。** 直ってからまた失敗したら、また通る
    CHECK(gate.Observe(true) == Change::Started);
    CHECK(gate.Observe(false) == Change::Recovered);
    CHECK(gate.LastStreakLength() == 1u);

    CHECK(gate.Observe(true) == Change::Started);
    CHECK(gate.Observe(true) == Change::None);
    CHECK(gate.Observe(true) == Change::None);
    CHECK(gate.Observe(false) == Change::Recovered);
    CHECK(gate.LastStreakLength() == 3u);

    CHECK(gate.TotalFailures() == 4u);          // 1 + 3

    // 出力される行数は「区間の数 x 2」で、フレーム数には比例しない。
    // **ここが「溢れない」ことの本体である**
  }

  void TestGateHandlesFailureOnTheVeryFirstFrame() {
    GLFD::Test::BeginCase("T-ECS-17c: failing on the first frame is reported once");

    FailureGate gate;
    CHECK(gate.Observe(true) == Change::Started);   // 前の状態が無くても Started
    CHECK(gate.StreakLength() == 1u);
    CHECK(gate.Observe(true) == Change::None);
  }

  // ===========================================================================
  // T-ECS-17d 確保失敗が観測できること
  // ===========================================================================

  void TestGridReportsAllocationFailureInsteadOfThrowing() {
    GLFD::Test::BeginCase("T-ECS-17d: a grid that cannot allocate says so instead of throwing");

    // --- 確保できる場合 ------------------------------------------------------
    {
      MockMemoryResource mock;
      SpatialHashGrid    grid(&mock, 128);
      CHECK(grid.IsReady());
      CHECK(mock.AllocateCalls() >= 2);           // buckets と next
    }

    // --- 1 本目から失敗する場合 ----------------------------------------------
    {
      MockMemoryResource mock;
      mock.SetFailAfter(0);
      SpatialHashGrid grid(&mock, 128);

      // **投げない。** 以前は投擲版の Resize を呼んでいたので
      // std::bad_alloc が毎フレームの経路から飛んでいた
      CHECK(!grid.IsReady());

      // 触っても安全であること。**Clear は 8 MB の memset なので、
      // 確保できていないときに走らせると nullptr へ書く**
      grid.Clear();
      grid.Insert(0u, GLFD::Components::Position{ 1.0f, 1.0f, 0.0f, 0.0f });

      std::size_t visited = 0;
      grid.Query(GLFD::Components::Position{ 1.0f, 1.0f, 0.0f, 0.0f },
                 [&visited](std::uint32_t) { ++visited; return true; });
      CHECK(visited == 0u);                       // 空として振る舞う

      mock.ClearFailure();
    }

    // --- 2 本目だけ失敗する場合(バケットは取れたが next が取れない)---------
    {
      MockMemoryResource mock;
      mock.SetFailAfter(1);
      SpatialHashGrid grid(&mock, 128);
      CHECK(!grid.IsReady());                     // **片方だけでは ready にしない**

      grid.Clear();
      std::size_t visited = 0;
      grid.Query(GLFD::Components::Position{ 0.0f, 0.0f, 0.0f, 0.0f },
                 [&visited](std::uint32_t) { ++visited; return true; });
      CHECK(visited == 0u);
      mock.ClearFailure();
    }

    // --- **1 本目だけ**失敗する場合 -------------------------------------------
    // 「2 本とも失敗」で済ませると、1 本目の検査を消す変異を 2 本目の検査が
    // 代わりに止めてしまい、差が消える(1-6 の変異 M8 で実際に見逃した)。
    // **どちらの検査も単独で効いていること**を見る
    {
      FailOnlyNth    resource(0);                 // バケットだけ失敗させる
      SpatialHashGrid grid(&resource, 128);

      CHECK(!grid.IsReady());                     // 1 本目の失敗だけで ready にしない
      grid.Clear();                               // ここで nullptr へ 8 MB の memset をしない
      std::size_t visited = 0;
      grid.Query(GLFD::Components::Position{ 0.0f, 0.0f, 0.0f, 0.0f },
                 [&visited](std::uint32_t) { ++visited; return true; });
      CHECK(visited == 0u);
    }
  }

  /// N 回目から総当たりで注入しても、投げず・落ちず・嘘をつかないこと
  void TestGridSurvivesInjectedFailureAtAnyPoint() {
    GLFD::Test::BeginCase("T-ECS-17e: injecting failure at any allocation stays survivable");

    bool everyRunSurvived = true;

    for (int failAfter = 0; failAfter < 6; ++failAfter) {
      MockMemoryResource mock;
      {
        mock.SetFailAfter(failAfter);
        SpatialHashGrid grid(&mock, 64);

        // ready なら使えるはず、ready でないなら空として振る舞うはず
        grid.Clear();
        for (std::uint32_t i = 0; i < 16u; ++i) {
          grid.Insert(i, GLFD::Components::Position{ static_cast<float>(i), 0.0f, 0.0f, 0.0f });
        }

        std::size_t visited = 0;
        grid.Query(GLFD::Components::Position{ 0.0f, 0.0f, 0.0f, 0.0f },
                   [&visited](std::uint32_t) { ++visited; return true; });

        if (!grid.IsReady() && visited != 0u) { everyRunSurvived = false; }

        mock.ClearFailure();
      }
      if (mock.DoubleFree() || mock.UnknownFree()
          || mock.SizeMismatch() || mock.AlignmentMismatch()) {
        everyRunSurvived = false;
      }
      if (mock.LiveBytes() != 0u) { everyRunSurvived = false; }    // N-4
    }

    CHECK(everyRunSurvived);
  }

  // ===========================================================================
  // T-ECS-17f / 17g 適用結果の報告 (1-8 で R-46 を適用)
  // ===========================================================================

  /// 積むだけの小さな成分(`CommandBuffer::Add` の制約を満たす)
  struct Tag { float v; };

  /**
   * @brief T-ECS-17f: 適用の報告は最初の 1 回と、取りこぼしの変わり目だけ
   *
   * @details
   *  **1-4 の `ReportAppliedCommands` は、何か適用されたフレームに毎回 1 行出していた。**
   *  「デモはコマンドを 1 つも積まない」前提で作ったためで、1-8 で破棄が毎フレーム
   *  起きると毎秒 60 行以上になる。しかも **1-4 からこの判断にはテストが無かった**
   *  (`Logger` と DX11 を引き込むヘッダの中にあったため)。
   *
   *  **静かなフレームが「何も適用しなかったので静か」ではないこと**を対にする。
   *  毎フレーム実際に適用が起きている (`Applied() == 1`) 状態で黙ることを見る。
   */
  void TestAppliedCommandsSpeakOnlyWhenTheStateChanges() {
    GLFD::Test::BeginCase("T-ECS-17f: applied commands are reported once, then only when dropping starts or stops");

    MockMemoryResource       mock;
    GLFD::ECS::Registry      registry(&mock);
    GLFD::ECS::CommandBuffer commands(&mock);
    bool                     loggedFirst = false;
    FailureGate              gate;

    const auto apply = [&]() {
      registry.ApplyCommands(commands);
      return GLFD::Game::DecideAppliedCommands(commands.Report(), loggedFirst, gate);
    };

    // 1 フレーム目: 何も積んでいなくても出す(1-4 の目的)
    GLFD::Game::AppliedCommandsNotice n = apply();
    CHECK(n.firstFlush);
    CHECK(n.drops == Change::None);

    // 2〜4 フレーム目: **毎フレーム実際に適用している**が、何も出さない
    bool quiet   = true;
    bool applied = true;
    for (int i = 0; i < 3; ++i) {
      const GLFD::ECS::Entity e = registry.CreateEntity();
      CHECK(commands.Destroy(e));
      n = apply();
      quiet   = quiet && !n.firstFlush && n.drops == Change::None;
      applied = applied && commands.Report().Applied() == 1u;
    }
    CHECK(applied);
    CHECK(quiet);

    // 5 フレーム目: 二重破棄が始まった。1 回だけ出し、WARN で足りる
    GLFD::ECS::Entity twice = registry.CreateEntity();
    CHECK(commands.Destroy(twice));
    CHECK(commands.Destroy(twice));
    n = apply();
    CHECK(commands.Report().Dropped() == 1u);
    CHECK(n.drops == Change::Started);
    CHECK(n.onlyDoubleDestroys);

    // 6〜7 フレーム目: 取りこぼしが続いている間は黙る
    bool silentWhileDropping = true;
    for (int i = 0; i < 2; ++i) {
      twice = registry.CreateEntity();
      CHECK(commands.Destroy(twice));
      CHECK(commands.Destroy(twice));
      n = apply();
      silentWhileDropping = silentWhileDropping && n.drops == Change::None
                         && commands.Report().Dropped() == 1u;
    }
    CHECK(silentWhileDropping);

    // 8 フレーム目: 直った。1 回だけ出す
    n = apply();
    CHECK(n.drops == Change::Recovered);
    CHECK(gate.LastStreakLength() == 3u);
  }

  /**
   * @brief T-ECS-17g: 二重破棄ではない取りこぼしを、軽く扱わない
   *
   * @details
   *  二重破棄は VS 型では起こり得るので WARN に留めるが、**それ以外の理由が 1 件でも
   *  混ざれば ERROR** にする。また明細は 8 件で切れるので、**切れて理由が分からない
   *  取りこぼしが残るなら「全部二重破棄」とは言わない**(残りも二重破棄である
   *  可能性は高いが、見えていないものを軽く扱わない)。
   */
  void TestOtherDropsAreNotSoftened() {
    GLFD::Test::BeginCase("T-ECS-17g: a drop that is not a double destroy, or an unseen one, is not softened");

    {
      MockMemoryResource       mock;
      GLFD::ECS::Registry      registry(&mock);
      GLFD::ECS::CommandBuffer commands(&mock);
      bool                     loggedFirst = true;
      FailureGate              gate;

      const GLFD::ECS::Entity dead = registry.CreateEntity();
      registry.DestroyEntity(dead);                  // 反復の外なので即時でよい

      const GLFD::ECS::Entity twice = registry.CreateEntity();
      CHECK(commands.Destroy(twice));
      CHECK(commands.Destroy(twice));                // 二重破棄
      CHECK(commands.Add(dead, Tag{ 1.0f }));        // 死んだハンドルへの追加

      registry.ApplyCommands(commands);
      const GLFD::Game::AppliedCommandsNotice n =
          GLFD::Game::DecideAppliedCommands(commands.Report(), loggedFirst, gate);
      CHECK(commands.Report().Dropped() == 2u);
      CHECK(n.drops == Change::Started);
      CHECK(!n.onlyDoubleDestroys);
    }

    {
      MockMemoryResource       mock;
      GLFD::ECS::Registry      registry(&mock);
      GLFD::ECS::CommandBuffer commands(&mock);
      bool                     loggedFirst = true;
      FailureGate              gate;

      // 9 件の二重破棄。明細は 8 件で切れる
      for (int i = 0; i < 9; ++i) {
        const GLFD::ECS::Entity e = registry.CreateEntity();
        CHECK(commands.Destroy(e));
        CHECK(commands.Destroy(e));
      }
      registry.ApplyCommands(commands);
      const GLFD::Game::AppliedCommandsNotice n =
          GLFD::Game::DecideAppliedCommands(commands.Report(), loggedFirst, gate);
      CHECK(commands.Report().Dropped() == 9u);
      CHECK(commands.Report().Truncated());
      CHECK(n.drops == Change::Started);
      CHECK(!n.onlyDoubleDestroys);
    }
  }

}

int main() {
  GLFD::Test::BeginSuite("EcsDiagnostics (ECS 1-6)");

  TestGateStaysQuietWhileNothingChanges();
  TestGateCountsEveryEpisode();
  TestGateHandlesFailureOnTheVeryFirstFrame();
  TestGridReportsAllocationFailureInsteadOfThrowing();
  TestGridSurvivesInjectedFailureAtAnyPoint();

  TestAppliedCommandsSpeakOnlyWhenTheStateChanges();
  TestOtherDropsAreNotSoftened();

  return GLFD::Test::Summarize();
}
