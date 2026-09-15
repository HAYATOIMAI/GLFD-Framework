/**
 * @file  EcsGuideTests.cpp
 * @brief ECS 1-8: 利用ガイド (`Source/ECS/README.md`) の例が、書いてあるとおりに動くこと
 *
 * @details
 *  **README の例をここで実際にコンパイルし、振る舞いを確かめる。** 例がテストの外に
 *  あると、API を変えたときに例だけが古くなる。
 *
 *  README の節とこのファイルのケースは 1 対 1 に対応する。各ケースの
 *  `---- README n ----` から `---- ここまで ----` の間は README の例と同じ文である。
 *  **片方を変えたらもう片方も変えること。**
 *
 *  ## プロセスの終了
 *  節 2 の並列の例が `JobSystem` を使う。末尾で `std::_Exit` を使うのは
 *  停止経路の取りこぼし (§18.7) を避けているだけで、直してはいない。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>

#include "ECS/CommandBuffer.h"
#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "ECS/Registry.h"
#include "ECS/View.h"
#include "Threading/JobSystem.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

namespace GLFD {
  namespace {

    // README の例で使う成分(README ではゲーム側で定義する想定)
    struct Health { float current; };
    struct Pickup { float value; };

    Thread::JobSystem* g_jobs = nullptr;

    // =========================================================================
    // 1. エンティティを作って成分を足す
    // =========================================================================

    void GuideCreateAndAdd() {
      Test::BeginCase("T-ECS-G1: create an entity and add components (README 1)");

      Test::MockMemoryResource mock;
      Memory::IMemoryResource* const resource = &mock;

      // ---- README 1 ----
      ECS::Registry registry(resource);

      const ECS::Entity e = registry.CreateEntity();
      if (!e.IsValid()) {
        // 上限 (ECS::MaxEntities = 65,536) か確保失敗。**戻り値を必ず見る** (R-10)
      }

      if (registry.AddComponent<Components::Position>(e, 1.0f, 2.0f, 0.0f, 0.0f) == nullptr
          || registry.AddComponent<Health>(e, 10.0f) == nullptr) {
        registry.DestroyEntity(e);
      }

      Components::Position* const pos = registry.GetComponent<Components::Position>(e);
      const bool hasHealth = registry.HasComponent<Health>(e);
      // ---- ここまで ----

      CHECK(e.IsValid());
      CHECK(registry.IsAlive(e));
      CHECK(pos != nullptr && pos->x == 1.0f && pos->y == 2.0f);
      CHECK(hasHealth);
      CHECK(!registry.HasComponent<Pickup>(e));
      CHECK(registry.AliveCount() == 1u);
    }

    // =========================================================================
    // 2. View<Ts...> で反復する
    // =========================================================================

    void GuideIterate() {
      Test::BeginCase("T-ECS-G2: single, combined and sliced parallel iteration (README 2)");

      Test::MockMemoryResource mock;
      ECS::Registry registry(&mock);

      // 6 体。x = 0.5, 1.5, ... 5.5。偶数番だけが Velocity を持つ
      ECS::Entity made[6];
      for (int i = 0; i < 6; ++i) {
        made[i] = registry.CreateEntity();
        CHECK(registry.AddComponent<Components::Position>(
                  made[i], static_cast<float>(i) + 0.5f, 0.0f, 0.0f, 0.0f) != nullptr);
        if (i % 2 == 0) {
          CHECK(registry.AddComponent<Components::Velocity>(made[i], 1.0f, 0.0f, 0.0f, 0.0f)
                != nullptr);
        }
      }
      Thread::JobSystem& jobs = *g_jobs;

      // ---- README 2 ----
      float sumX = 0.0f;
      for (auto [e, pos] : registry.View<Components::Position>()) {
        (void)e;
        sumX += pos.x;
      }

      int moving = 0;
      for (auto [e, pos, vel] : registry.View<Components::Position, Components::Velocity>()) {
        (void)e;
        pos.x += vel.vx;
        ++moving;
      }

      auto view = registry.View<Components::Position, Components::Velocity>();
      const std::size_t half = view.BaseSize() / 2;

      Thread::JobCounter counter;
      auto handle = jobs.CreateHandle(counter);
      jobs.KickJob([view, half]() {
          for (auto [e, pos, vel] : view.Slice(0, half)) { (void)e; pos.x += vel.vx; }
      }, &handle);
      jobs.KickJob([view, half]() {
          for (auto [e, pos, vel] : view.Slice(half, view.BaseSize())) { (void)e; pos.x += vel.vx; }
      }, &handle);
      jobs.WaitFor(handle);
      // ---- ここまで ----

      CHECK(sumX == 18.0f);   // 0.5 + 1.5 + ... + 5.5
      CHECK(moving == 3);     // Velocity を持たない 3 体は飛ばされた
      CHECK(half > 0u);       // 並列の 2 本がどちらも空でないこと(空だと 1 本しか検査していない)

      // **組を持つものは 2 回ずつ進み(逐次で 1 回、並列で 1 回)、持たないものは動かない**
      bool exact = true;
      for (int i = 0; i < 6; ++i) {
        const Components::Position* const p = registry.GetComponent<Components::Position>(made[i]);
        const float expected = static_cast<float>(i) + 0.5f + ((i % 2 == 0) ? 2.0f : 0.0f);
        exact = exact && (p != nullptr) && (p->x == expected);
      }
      CHECK(exact);
    }

    // =========================================================================
    // 3. 反復中に構造を変える
    // =========================================================================

    void GuideChangeStructureWhileIterating() {
      Test::BeginCase("T-ECS-G3: destroy and create while iterating through a CommandBuffer (README 3)");

      Test::MockMemoryResource mock;
      Memory::IMemoryResource* const resource = &mock;
      ECS::Registry registry(resource);

      // 体力 0 / 5 / 0 の 3 体
      const float lives[3] = { 0.0f, 5.0f, 0.0f };
      for (int i = 0; i < 3; ++i) {
        const ECS::Entity e = registry.CreateEntity();
        CHECK(registry.AddComponent<Health>(e, lives[i]) != nullptr);
        CHECK(registry.AddComponent<Components::Position>(
                  e, 10.0f * static_cast<float>(i + 1), 3.0f, 0.0f, 0.0f) != nullptr);
      }

      // ---- README 3 ----
      ECS::CommandBuffer commands(resource);

      for (auto [e, hp, pos] : registry.View<Health, Components::Position>()) {
        if (hp.current > 0.0f) { continue; }
        if (!commands.Destroy(e)) { continue; }

        const ECS::Entity drop = registry.CreateEntity();
        if (drop.IsValid()) {
          (void)commands.Add(drop, Components::Position{ pos.x, pos.y, 0.0f, 0.0f });
          (void)commands.Add(drop, Pickup{ 1.0f });
        }
      }

      registry.ApplyCommands(commands);
      const ECS::ApplyReport& report = commands.Report();
      // ---- ここまで ----

      CHECK(report.Applied() == 6u);    // 破棄 2 + 追加 2 x 2
      CHECK(report.Dropped() == 0u);
      CHECK(registry.AliveCount() == 3u);
      CHECK(registry.View<Health>().BaseSize() == 1u);
      CHECK(registry.View<Pickup>().BaseSize() == 2u);

      // 経験値は倒れた 2 体の位置(x = 10 と 30)に出ている
      float xs = 0.0f;
      for (auto [e, pos, pick] : registry.View<Components::Position, Pickup>()) {
        (void)e;
        (void)pick;
        xs += pos.x;
      }
      CHECK(xs == 40.0f);
    }

    // =========================================================================
    // 4. 破棄と、死んだハンドルの扱い
    // =========================================================================

    void GuideDeadHandles() {
      Test::BeginCase("T-ECS-G4: a destroyed handle stays valid-looking but dead (README 4)");

      Test::MockMemoryResource mock;
      ECS::Registry registry(&mock);

      const ECS::Entity target = registry.CreateEntity();
      CHECK(registry.AddComponent<Health>(target, 3.0f) != nullptr);

      // ---- README 4 ----
      registry.DestroyEntity(target);

      const bool alive = registry.IsAlive(target);
      const bool valid = target.IsValid();
      const Health* const hp = registry.GetComponent<Health>(target);

      const ECS::Entity next = registry.CreateEntity();
      // ---- ここまで ----

      CHECK(!alive);
      CHECK(valid);
      CHECK(hp == nullptr);

      CHECK(next.Index() == target.Index());            // 空いた index が再利用された
      CHECK(next.Generation() != target.Generation());
      CHECK(registry.IsAlive(next));
      CHECK(registry.AddComponent<Health>(next, 7.0f) != nullptr);
      CHECK(registry.GetComponent<Health>(target) == nullptr);   // 古いハンドルでは新しい住人に触れない
      const Health* const occupant = registry.GetComponent<Health>(next);
      CHECK(occupant != nullptr && occupant->current == 7.0f);
    }

  }
}

int main() {
  GLFD::Test::BeginSuite("EcsGuide (ECS 1-8)");

  // **意図的に解放しない。** ファイル冒頭の「プロセスの終了」を参照
  GLFD::Thread::JobSystem jobs;
  GLFD::g_jobs = &jobs;

  GLFD::GuideCreateAndAdd();
  GLFD::GuideIterate();
  GLFD::GuideChangeStructureWhileIterating();
  GLFD::GuideDeadHandles();

  const int code = GLFD::Test::Summarize();
  std::fflush(stdout);
  std::_Exit(code);
}
