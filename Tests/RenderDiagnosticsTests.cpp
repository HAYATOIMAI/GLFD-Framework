/**
 * @file  RenderDiagnosticsTests.cpp
 * @brief T-ECS-38d: 頂点を組めないときの診断の行 / T-ECS-38f: 描く大きさと当たり判定 (ECS 2-3)
 *
 * @details
 *  **1-6 から「報告の形は実機の動作確認が受け持つ」としてきた部分を、テストで押さえる。**
 *  (`EcsDiagnosticsTests.cpp` の冒頭の「押さえられないもの」。2-3 で頂点を組む処理を
 *  `Game::BuildSurvivorVertices` に切り出したので、DX11 なしで失敗を起こせるようになった)
 *
 *  本番と同じ組み合わせを呼ぶ: `BuildSurvivorVertices` が返した `RenderStatus` を
 *  `Game::ReportRenderStatus` と `FailureGate` に渡し、**`Logger` が実際に書いた行**を読む。
 *
 *  ## 確かめること
 *   - 描けている間は 1 行も出ない
 *   - 描けなくなった最初のフレームで ERROR が 1 行だけ出る(理由と要求した数を含む)
 *   - 描けないフレームが続いても黙っている
 *   - 直ったフレームで INFO が 1 行だけ出る(続いたフレーム数と累計を含む)
 *   - 38f: **本物の生成の経路**(`SpawnEnemy` / `FireBullet` / `QueuePickup`)で作ったものが、
 *     付いた `Collider` の半径の大きさで描かれる。プレイヤーは `reachRadius`。
 *     `SurvivorLoop.h` を引き込むので、`SurvivorRenderTests` ではなくここに置く (2-3 (3))
 *
 *  ## 押さえないもの
 *   - Boid の経路(`RenderSystem::Update`)は DX11 を要求するので呼べない。
 *     文言と門は同じ `ReportRenderStatus` を通る
 *
 *  @note `EcsDiagnosticsLog.h` は `GameContext.h` 経由で `d3d11.h` を引き込む(既存の
 *        `EcsSurvivorTests` と同じ)。頂点のデータの検証は `SurvivorRenderTests` に分け、
 *        そちらは `d3d11.h` を引き込まない。
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Core/DynamicArray.h"
#include "Core/FailureGate.h"
#include "Core/Logger.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Game/EcsDiagnosticsLog.h"
#include "Game/SurvivorComponents.h"
#include "Game/SurvivorRender.h"
#include "Graphics/RenderStatus.h"
#include "Graphics/SimpleVertex.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::Core::FailureGate;
using GLFD::Systems::RenderStatus;
using GLFD::Test::MockMemoryResource;

namespace {

  constexpr int kMaxLines   = 16;
  constexpr int kLineLength = 512;

  /// Logger が書いたファイルを行ごとに読む
  int ReadLines(const char* path, char (&lines)[kMaxLines][kLineLength]) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) { return -1; }
    int n = 0;
    char buf[kLineLength];
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
      if (n < kMaxLines) { strcpy_s(lines[n], buf); }
      ++n;
    }
    std::fclose(f);
    return n;
  }

  bool Contains(const char* line, const char* part) { return std::strstr(line, part) != nullptr; }

  void TestRenderFailureIsReportedOnlyWhenTheStateChanges() {
    GLFD::Test::BeginCase("T-ECS-38d: a vertex array that cannot be sized is logged once when it starts and once when it recovers");

    // ログの書き先は一時フォルダ(リポジトリを汚さない)
    char path[512]{};
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\glfd_render_diagnostics_test.log", hasTemp ? tempDir : ".");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    // 描くものを置く(敵 2・弾 1・経験値 1)。原点から離す
    MockMemoryResource world;
    GLFD::ECS::Registry registry(&world);
    const float xs[] = { 7.5f, -11.25f, 3.5f, -4.75f };
    const float ys[] = { 9.0f, 5.5f, -12.25f, -6.5f };
    for (int i = 0; i < 4; ++i) {
      const GLFD::ECS::Entity e = registry.CreateEntity();
      (void)registry.AddComponent<GLFD::Components::Position>(e, xs[i], ys[i], 0.0f, 0.0f);
      if (i < 2)       { (void)registry.AddComponent<GLFD::Components::Health>(e, 2.0f); }
      else if (i == 2) { (void)registry.AddComponent<GLFD::Components::Damage>(e, 1.0f); }
      else             { (void)registry.AddComponent<GLFD::Components::Pickup>(e, 1.0f); }
    }

    MockMemoryResource frame;
    FailureGate gate;
    std::size_t requestedWhenFailing = 0;
    int drawn = 0, failed = 0;

    // 描ける 3 フレーム → 描けない 5 フレーム → 描ける 2 フレーム
    for (int f = 0; f < 10; ++f) {
      const bool failing = (f >= 3 && f < 8);
      if (failing) { frame.SetFailAfter(0); } else { frame.ClearFailure(); }
      GLFD::DynamicArray<GLFD::Graphics::SimpleVertex> vertices(&frame);
      const RenderStatus status = GLFD::Game::BuildSurvivorVertices(
          registry, GLFD::Game::SurvivorDrawRadiiOf(GLFD::Game::SmallSurvivorParams()),
          GLFD::Game::SurvivorScreenOf(1280, 720), vertices);
      if (status.outcome == RenderStatus::Outcome::Drawn) { ++drawn; } else { ++failed; requestedWhenFailing = status.requestedVertices; }
      GLFD::Game::ReportRenderStatus(status, gate);   // シーンと同じ呼び方
    }
    GLFD::Core::Logger::Get().Shutdown();
    CHECK(drawn == 5);
    CHECK(failed == 5);

    char lines[kMaxLines][kLineLength]{};
    const int n = ReadLines(path, lines);
    std::remove(path);

    CHECK(n == 2);   // 10 フレームで 2 行だけ
    if (n >= 2) {
      char expectedCount[64];
      std::snprintf(expectedCount, sizeof expectedCount, "(%zu vertices requested)", requestedWhenFailing);
      CHECK(Contains(lines[0], "[ERR ]"));
      CHECK(Contains(lines[0], "RenderSystem: not drawing this frame."));
      CHECK(Contains(lines[0], "the vertex buffer could not be sized from frame memory"));
      CHECK(Contains(lines[0], expectedCount));
      CHECK(Contains(lines[1], "[INFO]"));
      CHECK(Contains(lines[1], "RenderSystem: drawing again after 5 frame(s) (5 dropped frame(s) total)"));
    }
    CHECK(requestedWhenFailing >= 5u);   // 4 体 + プレイヤー
  }

  /// 38f の画面。下限なし(大きさがそのまま出る)
  constexpr GLFD::Game::SurvivorScreen kNoMinimumScreen{ 1.0f / 60.0f, 0.0f };

  /// 描いた頂点のうち、NDC の位置が (x, y) のものの z。無ければ -1
  float ZAt(const GLFD::DynamicArray<GLFD::Graphics::SimpleVertex>& v, float x, float y) {
    for (std::size_t i = 0; i < v.GetSize(); ++i) {
      if (std::fabs(v[i].Pos.x - x * kNoMinimumScreen.scaleX) <= 1e-6f
          && std::fabs(v[i].Pos.y - y * GLFD::Game::kSurvivorScaleY) <= 1e-6f) {
        return v[i].Pos.z;
      }
    }
    return -1.0f;
  }

  /**
   * @brief 本物の生成の経路で作ったものの大きさが、**付いた `Collider` の半径**と一致する
   * @details `SurvivorRenderTests` は `SurvivorLoop.h` を引き込めない(`d3d11.h` まで入る)。
   *          このスイートは診断のために既に引き込んでいるので、ここで生成の経路とつなぐ
   */
  void TestDrawnSizeFollowsTheColliders() {
    GLFD::Test::BeginCase("T-ECS-38f: entities made by the real spawn paths are drawn with the radius of their Collider");

    MockMemoryResource world;
    GLFD::ECS::Registry registry(&world);
    MockMemoryResource commandMemory;
    GLFD::ECS::CommandBuffer commands(&commandMemory);

    GLFD::Game::SurvivorState s;
    s.params   = GLFD::Game::SmallSurvivorParams();
    s.registry = &registry;
    s.commands = &commands;

    const GLFD::ECS::Entity enemy  = GLFD::Game::SpawnEnemy(s, 6.5f, -3.25f, 0.0f, 0.0f);
    const GLFD::ECS::Entity bullet = GLFD::Game::FireBullet(s, -2.75f, 4.5f, 0.0f, 0.0f);
    GLFD::Game::QueuePickup(s, GLFD::Components::Position{ 9.25f, 7.75f, 0.0f, 0.0f });
    CHECK(enemy.IsValid());
    CHECK(bullet.IsValid());
    CHECK(s.thisStep.pickupsCreated == 1u);
    registry.ApplyCommands(commands);

    // 経験値は遅延で作られるので、Pickup を持つものを探す
    GLFD::ECS::Entity pickup = GLFD::ECS::Entity::Invalid();
    for (auto [e, pos, pick] : registry.View<GLFD::Components::Position, GLFD::Components::Pickup>()) {
      (void)pos; (void)pick;
      pickup = e;
    }
    CHECK(pickup.IsValid());

    const GLFD::Components::Collider* ce = registry.GetComponent<GLFD::Components::Collider>(enemy);
    const GLFD::Components::Collider* cb = registry.GetComponent<GLFD::Components::Collider>(bullet);
    const GLFD::Components::Collider* cp = registry.GetComponent<GLFD::Components::Collider>(pickup);
    CHECK(ce != nullptr && cb != nullptr && cp != nullptr);
    if (ce == nullptr || cb == nullptr || cp == nullptr) { return; }

    // 下限なし: z = Collider の半径 x 縦の縮尺。プレイヤーは reachRadius
    MockMemoryResource frame;
    GLFD::DynamicArray<GLFD::Graphics::SimpleVertex> v(&frame);
    const RenderStatus status = GLFD::Game::BuildSurvivorVertices(
        registry, GLFD::Game::SurvivorDrawRadiiOf(s.params), kNoMinimumScreen, v);
    CHECK(status.outcome == RenderStatus::Outcome::Drawn);
    CHECK(v.GetSize() == 4u);
    CHECK(ZAt(v, 6.5f, -3.25f)  == ce->radius * GLFD::Game::kSurvivorScaleY);
    CHECK(ZAt(v, -2.75f, 4.5f)  == cb->radius * GLFD::Game::kSurvivorScaleY);
    CHECK(ZAt(v, 9.25f, 7.75f)  == cp->radius * GLFD::Game::kSurvivorScaleY);
    CHECK(ZAt(v, 0.0f, 0.0f)    == s.params.reachRadius * GLFD::Game::kSurvivorScaleY);
    // 実際の値(1-8 の small): 敵 0.8 / 弾 0.3 / 経験値 0.3 / プレイヤー 1.5
    CHECK(ce->radius == 0.8f && cb->radius == 0.3f && cp->radius == 0.3f && s.params.reachRadius == 1.5f);
  }

}

int main() {
  GLFD::Test::BeginSuite("RenderDiagnostics (ECS 2-3)");
  TestRenderFailureIsReportedOnlyWhenTheStateChanges();
  TestDrawnSizeFollowsTheColliders();
  return GLFD::Test::Summarize();
}
