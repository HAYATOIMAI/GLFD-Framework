/**
 * @file  CollisionObservationTests.cpp
 * @brief T-ECS-50: 衝突の観測が保たれる / 購読者が居ないときは配らない (ECS 2-12)
 *
 * @details
 *  2-11 まで、Boid は `CollisionEvent` を数えるためだけに購読していた。密な群れで 1 刻み最大 12 万件を
 *  積み、1 件 43〜136 ns を払って溢れていた(2-12 手順1)。2-12 で `CollisionSystem` は、刻みの頭に
 *  1 回だけ購読者が居るかを聞き、居なければ発行しない(B')。観測は系が返す**検出の数**で行う。
 *
 *  本番の `GridBuildSystem` / `CollisionSystem` / `EventBus` / `ReportCollisionContacts` を、実物の
 *  `GameContext` と `Logger` で呼ぶ(手順を写さない。開発手法 §4.7)。
 *
 *  ## 確かめること
 *   - 50a: `EventBus::SubscriberCountOf<T>` が型ごとの購読者の数を返す(チャネルが無ければ 0)
 *   - 50b: 購読者が居なければ 1 件も発行しない。それでも検出は数えられ、押し返しは行われる
 *   - 50c: 購読者が居れば今までどおり 1 件ずつ届き、数は検出の数と一致し、押し返しは 50b と同じ
 *   - 50d: 本物の検出の数を観測に渡すと「初めて検出した刻み」の 1 行が出る。衝突体が居るのに 0 件に
 *          なると始まりの 1 行、戻ると止みの 1 行が出る(**検出が壊れたら観測が気づく**)
 *   - 50e: 衝突体が 1 体以下なら 0 件でも始まりにしない。初めての行は 1 回だけ
 *
 *  ## 押さえないもの
 *   - 3 分回して溢れないこと: `collision_overflow_check.ps1`(T-ECS-49)
 *   - 購読者が居るときの費用(1 件 100 ns 超)は変わらない。その規模で使えることは保証しない
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

#include "Core/FailureGate.h"
#include "Core/GameContext.h"
#include "Core/Logger.h"
#include "Core/SimulationClock.h"
#include "Core/StackAllocator.h"
#include "Core/StackResource.h"
#include "ECS/CommandBuffer.h"
#include "ECS/Registry.h"
#include "ECS/View.h"
#include "Events/EventBus.h"
#include "Events/Events.h"
#include "Game/EcsDiagnosticsLog.h"
#include "Game/GridBulidSystem.h"
#include "Physics/CollisionComponents.h"
#include "Physics/CollisionSystem.h"

#include "MockMemoryResource.h"
#include "TestHarness.h"

using GLFD::Components::Collider;
using GLFD::Components::Position;
using GLFD::Components::Velocity;
using GLFD::ECS::Entity;
using GLFD::Events::CollisionEvent;

namespace {

  GLFD::Thread::JobSystem* g_jobs = nullptr;

  /// 衝突に要るものだけを実物で用意する(EcsCollisionTests と同じ形)
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
      (void)m_eventBus.Register<CollisionEvent>();
    }

    [[nodiscard]] GLFD::GameContext MakeContext() {
      return GLFD::GameContext{
          &m_global, &m_frame, g_jobs, &m_registry, &m_commands, &m_eventBus,
          nullptr, nullptr, nullptr,
          GLFD::Core::kSimulationStep,
          nullptr, nullptr };
    }

    [[nodiscard]] Entity Spawn(float x, float y, float radius) {
      const Entity e = m_registry.CreateEntity();
      if (!e.IsValid()) { return e; }
      if (m_registry.AddComponent<Position>(e, x, y, 0.0f, 0.0f) == nullptr) { return Entity::Invalid(); }
      if (m_registry.AddComponent<Velocity>(e, 0.0f, 0.0f, 0.0f, 0.0f) == nullptr) { return Entity::Invalid(); }
      if (m_registry.AddComponent<Collider>(e, radius) == nullptr) { return Entity::Invalid(); }
      return e;
    }

    /// 1 刻みぶん: グリッドを組み、衝突を見る(BoidDemoScene の Collision の段と同じ呼び方)
    GLFD::Systems::CollisionStepResult Step() {
      GLFD::GameContext ctx = MakeContext();
      GLFD::Systems::GridBuildSystem::Update(ctx);
      return GLFD::Systems::CollisionSystem::Update(ctx);
    }

    GLFD::ECS::Registry&    Registry() { return m_registry; }
    GLFD::Events::EventBus& EventBus() { return m_eventBus; }

  private:
    GLFD::Memory::StackAllocator m_mainStack;
    GLFD::Memory::StackAllocator m_frameStack;
    GLFD::Memory::StackResource  m_global;
    GLFD::Memory::StackResource  m_frame;
    GLFD::ECS::Registry          m_registry;
    GLFD::ECS::CommandBuffer     m_commands;
    GLFD::Events::EventBus       m_eventBus;
  };

  void CountEvent(void* context, const CollisionEvent&) { ++*static_cast<std::uint32_t*>(context); }

  void TempPath(char (&path)[512], const char* name) {
    char        tempDir[400]{};
    std::size_t tempLen = 0;
    const bool  hasTemp = getenv_s(&tempLen, tempDir, sizeof tempDir, "TEMP") == 0 && tempLen > 0;
    std::snprintf(path, sizeof path, "%s\\%s", hasTemp ? tempDir : ".", name);
  }

  std::vector<std::string> ReadLines(const char* path) {
    std::vector<std::string> lines;
    FILE* const f = _fsopen(path, "rb", _SH_DENYNO);
    if (f == nullptr) { return lines; }
    std::string current;
    int c = 0;
    while ((c = std::fgetc(f)) != EOF) {
      if (c == '\n') {
        if (!current.empty() && current.back() == '\r') { current.pop_back(); }
        lines.push_back(current);
        current.clear();
      }
      else { current.push_back(static_cast<char>(c)); }
    }
    std::fclose(f);
    return lines;
  }

  int CountContaining(const std::vector<std::string>& lines, const char* part) {
    int n = 0;
    for (const std::string& l : lines) { if (l.find(part) != std::string::npos) { ++n; } }
    return n;
  }

  // ===========================================================================
  // 50a 型ごとの購読者の数
  // ===========================================================================
  void TestSubscriberCountOf() {
    GLFD::Test::BeginCase("T-ECS-50a: EventBus::SubscriberCountOf counts the subscribers of one type");

    GLFD::Test::MockMemoryResource mock;
    GLFD::Events::EventBus bus(&mock);
    CHECK(bus.SubscriberCountOf<CollisionEvent>() == 0u);            // チャネルが無い
    CHECK(bus.Register<CollisionEvent>());
    CHECK(bus.SubscriberCountOf<CollisionEvent>() == 0u);            // チャネルはあるが購読者は居ない

    std::uint32_t hits = 0;
    const GLFD::Events::SubscriptionId other = bus.Subscribe<GLFD::Events::HitEvent>(&hits, [](void* c, const GLFD::Events::HitEvent&) { ++*static_cast<std::uint32_t*>(c); });
    CHECK(other.IsValid());
    CHECK(bus.SubscriberCountOf<CollisionEvent>() == 0u);            // 別の型の購読者は数えない
    CHECK(bus.SubscriberCountOf<GLFD::Events::HitEvent>() == 1u);

    std::uint32_t delivered = 0;
    const GLFD::Events::SubscriptionId id = bus.Subscribe<CollisionEvent>(&delivered, &CountEvent);
    CHECK(id.IsValid());
    CHECK(bus.SubscriberCountOf<CollisionEvent>() == 1u);
    CHECK(bus.Unsubscribe(id));
    CHECK(bus.SubscriberCountOf<CollisionEvent>() == 0u);
  }

  // ===========================================================================
  // 50b / 50c 購読者が居なければ配らない。居れば今までどおり
  // ===========================================================================
  struct PairResult {
    GLFD::Systems::CollisionStepResult step;
    std::uint32_t published = 0;
    std::uint32_t delivered = 0;
    float         vxA = 0.0f, vxB = 0.0f;
  };

  /// 半径 0.5 の 2 体を 0.6 離して置き(重なる)、離れた 2 体を加える。1 刻み回す
  PairResult RunPair(bool subscribe) {
    Harness harness;
    std::uint32_t delivered = 0;
    if (subscribe) { (void)harness.EventBus().Subscribe<CollisionEvent>(&delivered, &CountEvent); }
    const Entity a = harness.Spawn(0.0f, 0.0f, 0.5f);
    const Entity b = harness.Spawn(0.6f, 0.0f, 0.5f);
    CHECK(harness.Spawn(40.0f, 40.0f, 0.5f).IsValid());
    CHECK(harness.Spawn(60.0f, 60.0f, 0.5f).IsValid());
    CHECK(a.IsValid() && b.IsValid());

    PairResult r;
    r.step = harness.Step();
    r.published = harness.EventBus().Counters().published;
    harness.EventBus().DispatchAll();
    r.delivered = delivered;
    const Velocity* va = harness.Registry().GetComponent<Velocity>(a);
    const Velocity* vb = harness.Registry().GetComponent<Velocity>(b);
    CHECK(va != nullptr && vb != nullptr);
    if (va != nullptr) { r.vxA = va->vx; }
    if (vb != nullptr) { r.vxB = vb->vx; }
    return r;
  }

  void TestNoSubscriberNoEvents() {
    GLFD::Test::BeginCase("T-ECS-50b/c: with no subscriber nothing is published, yet contacts are counted and the pair is pushed apart");

    const PairResult quiet = RunPair(false);
    CHECK(quiet.step.colliders == 4u);
    CHECK(quiet.step.contacts == 2u);         // 重なった組を両側から 1 件ずつ
    CHECK(quiet.published == 0u);             // **購読者が居なければ 1 件も積まない**
    CHECK(quiet.vxA < 0.0f);                  // 押し返しは系の中で行われている
    CHECK(quiet.vxB > 0.0f);

    const PairResult heard = RunPair(true);
    CHECK(heard.step.contacts == 2u);
    CHECK(heard.published == 2u);             // 購読者が居れば今までどおり
    CHECK(heard.delivered == 2u);
    CHECK(heard.published == heard.step.contacts);
    CHECK(heard.vxA == quiet.vxA);            // 配るかどうかで動きは変わらない
    CHECK(heard.vxB == quiet.vxB);
  }

  // ===========================================================================
  // 50d / 50e 観測
  // ===========================================================================
  void TestObservationSeesRealContacts() {
    GLFD::Test::BeginCase("T-ECS-50d: real contacts give the first line; colliders with 0 contacts start and stop the alarm");

    char path[512]{};
    TempPath(path, "glfd_collision_observation.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));

    Harness harness;
    std::vector<Entity> crowd;
    for (int i = 0; i < 24; ++i) { crowd.push_back(harness.Spawn(0.1f * static_cast<float>(i), 0.0f, 0.5f)); }
    bool first = false;
    GLFD::Core::FailureGate gate;
    std::uint32_t contactsCrowded = 0;
    for (int step = 0; step < 3; ++step) {
      const GLFD::Systems::CollisionStepResult r = harness.Step();
      contactsCrowded += r.contacts;
      GLFD::Game::ReportCollisionContacts(r.contacts, r.colliders, first, gate);
    }
    // 散らす: 衝突体は 24 体のまま、どれも重ならない(本物の検出が 0 を返す)
    for (std::size_t i = 0; i < crowd.size(); ++i) {
      Position* p = harness.Registry().GetComponent<Position>(crowd[i]);
      CHECK(p != nullptr);
      if (p != nullptr) { p->x = 10.0f * static_cast<float>(i); p->y = 0.0f; }
    }
    std::uint32_t contactsApart = 0;
    for (int step = 0; step < 2; ++step) {
      const GLFD::Systems::CollisionStepResult r = harness.Step();
      contactsApart += r.contacts;
      GLFD::Game::ReportCollisionContacts(r.contacts, r.colliders, first, gate);
    }
    // 戻す
    for (std::size_t i = 0; i < crowd.size(); ++i) {
      Position* p = harness.Registry().GetComponent<Position>(crowd[i]);
      if (p != nullptr) { p->x = 0.1f * static_cast<float>(i); p->y = 0.0f; }
    }
    const GLFD::Systems::CollisionStepResult again = harness.Step();
    GLFD::Game::ReportCollisionContacts(again.contacts, again.colliders, first, gate);
    GLFD::Core::Logger::Get().Shutdown();

    const std::vector<std::string> lines = ReadLines(path);
    std::remove(path);
    CHECK(contactsCrowded > 0u);
    CHECK(contactsApart == 0u);
    CHECK(again.contacts > 0u);
    CHECK(CountContaining(lines, "[INFO] collision: first step with hits. contacts=") == 1);
    CHECK(CountContaining(lines, "colliders=24") >= 1);
    CHECK(CountContaining(lines, "[ERR ] collision: no contacts detected this step although 24 colliders exist") == 1);
    CHECK(CountContaining(lines, "[INFO] collision: detecting contacts again after 2 step(s) (2 bad step(s) total)") == 1);
    if (CountContaining(lines, "collision: first step with hits") != 1) {
      for (const std::string& l : lines) { std::printf("      line: %s\n", l.c_str()); }
    }
  }

  void TestObservationEdges() {
    GLFD::Test::BeginCase("T-ECS-50e: one collider or none is not an alarm; the first line is written once");

    char path[512]{};
    TempPath(path, "glfd_collision_observation_edges.log");
    CHECK(GLFD::Core::Logger::Get().Initialize(path));
    bool first = false;
    GLFD::Core::FailureGate gate;
    GLFD::Game::ReportCollisionContacts(0u, 0u, first, gate);     // 衝突体が居ない
    GLFD::Game::ReportCollisionContacts(0u, 1u, first, gate);     // 1 体では重なりようがない
    GLFD::Game::ReportCollisionContacts(5u, 30u, first, gate);
    GLFD::Game::ReportCollisionContacts(7u, 30u, first, gate);    // 2 回目の検出は行にしない
    GLFD::Core::Logger::Get().Shutdown();

    const std::vector<std::string> lines = ReadLines(path);
    std::remove(path);
    CHECK(lines.size() == 1u);
    CHECK(CountContaining(lines, "collision: first step with hits. contacts=5 colliders=30") == 1);
    CHECK(CountContaining(lines, "no contacts detected") == 0);
    CHECK(!gate.IsFailing());
  }

}

int main() {
  GLFD::Test::BeginSuite("CollisionObservation (ECS 2-12)");
  GLFD::Thread::JobSystem jobs;
  g_jobs = &jobs;

  TestSubscriberCountOf();
  TestNoSubscriberNoEvents();
  TestObservationSeesRealContacts();
  TestObservationEdges();

  return GLFD::Test::Summarize();
}
