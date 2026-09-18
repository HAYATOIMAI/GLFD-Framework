#pragma once

/**
 * @file  SurvivorLoop.h
 * @brief ECS 1-8: 敵の生成 → 弾の発射 → 衝突 → 敵の破棄 → 経験値の生成、の 1 周
 *
 * @details
 *  ## シーンではなくここに書く
 *  シーンは DX11 とウィンドウを要求するので、テストからもベンチからも呼べない。
 *  1-6 で `kUpdateOrder` がテストから見えなかった(§19.7)のと同じ問題を、
 *  最初から作らない。**シーンもテストもベンチも `RunSurvivorFrame` を呼ぶ。**
 *
 *  ## 順序(1-8 論点3 / §17.1)
 *  ```
 *  SpawnEnemies → FireBullets → Movement → GridBuild → Hit
 *    → DispatchEvents → Lifetime → Collect → Reach → ApplyCommands
 *  ```
 *   - **生成は `GridBuild` より前。** 即時 API の `AddComponent` は構造版を進める。
 *     グリッドを組んだ後に足すと、`Hit` の照合 (R-43) が発火する
 *   - **`DispatchEvents` は `ApplyCommands` の前。** 後ろにあると、購読者が積んだ
 *     破棄は次のフレームの最後まで効かず、その間ずっと敵は生きていて、また当たり、
 *     また積む。経験値を拾えるのは命中の 2 フレーム後になり、§5.3 が受け入れた
 *     「1 フレーム遅れ」と食い違う
 *
 *  ## 1 体への破棄は 1 回だけ
 *  **`Destroy` を積むのは `Health::current` / `Lifetime::remaining` が正から 0 以下へ
 *  変わったときに限る。** 積めなかったら値を正に戻し、次のフレームで再挑戦する。
 *  したがってこのループでは `DropReason::AlreadyDestroyed` が**構造的に 0 件**になる。
 *  1 件でも出たら、この規則が壊れている。
 *
 *  ## スレッド
 *  `Hit` だけが並列で、しかも発行するだけ。他はすべてメインスレッドで、
 *  `CommandBuffer` に積むのもメインスレッドだけ (R-18)。カウンタに atomic は要らない。
 */

#include "../Core/FailureGate.h"
#include "../Core/GameContext.h"
#include "../Core/SystemSchedule.h"
#include "../ECS/CommandBuffer.h"
#include "../ECS/Components.h"
#include "../ECS/Entity.h"
#include "../ECS/Registry.h"
#include "../ECS/View.h"
#include "../Events/EventBus.h"
#include "../Events/Events.h"
#include "../Physics/CollisionComponents.h"
#include "GridBulidSystem.h"
#include "HitSystem.h"
#include "SimdSystem.h"
#include "SurvivorComponents.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>

namespace GLFD::Game {

  // ===========================================================================
  // パラメータ
  // ===========================================================================

  /**
   * @brief 1 周の形を決める値
   * @note  `spawnEveryFrames` / `fireEveryFrames` が 0 なら自動の生成・発射をしない
   *        (テストで配置を手で決めるため)
   */
  struct SurvivorParams {
    std::uint32_t maxEnemies = 0;
    std::uint32_t maxBullets = 0;
    std::uint32_t maxPickups = 0;

    std::uint32_t spawnEveryFrames = 0;
    std::uint32_t enemiesPerSpawn  = 0;
    float         spawnRadius      = 0.0f;
    /**
     * 敵が湧く角度の幅(ラジアン)。既定は全周。
     * **全周だと弾の密度と敵の密度が連動する**ので、「弾が外れて期限切れになる」と
     * 「敵が回収点まで届く」が同時に起きない(1-8 の掃引で実測)
     */
    float         spawnArc         = 6.28318531f;
    float         enemySpeed       = 0.0f;
    float         enemyHealth      = 1.0f;
    float         enemyRadius      = 0.5f;

    std::uint32_t fireEveryFrames  = 0;
    std::uint32_t bulletsPerVolley = 0;
    float         bulletSpeed      = 0.0f;
    float         bulletLifetime   = 1.0f;
    float         bulletDamage     = 1.0f;
    float         bulletRadius     = 0.2f;

    float         pickupLifetime   = 1.0f;
    float         pickupRadius     = 0.3f;
    float         pickupValue      = 1.0f;

    /// 原点からこの距離に入った経験値を回収する
    float         collectRadius    = 0.0f;
    /// 原点からこの距離に入った敵は、プレイヤーに触れたものとして消える(経験値なし)
    float         reachRadius      = 0.0f;
  };

  /**
   * @brief 観測用の小さい構成(数百体)
   *
   * @details
   *  上限は定常状態の数から大きく離してある。敵は 20 体/秒で湧き、最長でも
   *  `spawnRadius / enemySpeed` = 10 秒で消えるので、最大でも 200 体。弾は
   *  30 発/秒 × 2 秒 = 60 発。**上限に張り付かないこと自体をテストが確かめる**
   *  (張り付くと「発散しない」を上限が作ってしまい、ループの性質が見えない)。
   *
   *  ## 値は推測ではなく掃引で決めた
   *  **推測で 2 度外した。** 全周で 40 発/秒だと命中率 83% で、回収も接触も 0。
   *  10 発/秒に絞ると今度は弾が群れに全部吸われ、弾の期限切れが 0 になった。
   *  全周では弾の密度と敵の密度が連動するので、**弾の量をどう選んでも**
   *  「弾が外れる」と「敵が届く」が同時に起きない(20 / 30 / 60 発/秒で確認)。
   *
   *  湧く角度を半周に絞ると両立する(1200 フレーム、実測):
   *  | 湧く幅 | 弾/秒 | 撃破 | 回収 | 経験値の期限切れ | 弾の期限切れ | 接触 |
   *  |---|---|---|---|---|---|---|
   *  | 360 | 30 | 257 | 16 | 210 | 42    | **0** |
   *  | 360 | 20 | 186 | 97 | 89  | **0** | 22    |
   *  | 180 | 30 | 128 | 57 | 71  | 262   | 80    |
   *
   *  **保存則のどの項も 0 にならない**ことが要件である。項が 0 だと、その項を
   *  取り違える不具合があっても等式は成り立ってしまう (§12.1)。
   *  同じ構成を繰り返しても件数は一致した(配信順の競合は合計を変えていない)。
   */
  [[nodiscard]] constexpr SurvivorParams SmallSurvivorParams() noexcept {
    return SurvivorParams{
        .maxEnemies = 400, .maxBullets = 400, .maxPickups = 400,
        .spawnEveryFrames = 6, .enemiesPerSpawn = 2,
        .spawnRadius = 30.0f, .spawnArc = 3.14159265f,
        .enemySpeed = 3.0f, .enemyHealth = 2.0f, .enemyRadius = 0.8f,
        .fireEveryFrames = 2, .bulletsPerVolley = 1,
        .bulletSpeed = 12.0f, .bulletLifetime = 2.0f, .bulletDamage = 1.0f, .bulletRadius = 0.3f,
        .pickupLifetime = 3.0f, .pickupRadius = 0.3f, .pickupValue = 1.0f,
        .collectRadius = 8.0f, .reachRadius = 1.5f,
    };
  }

  /**
   * @brief VS 型の規模の構成(数千体)。**基準線の 2 本目に使う**
   *
   * @details
   *  `Small` の湧く数と弾の数をそれぞれ 10 倍にし、形(半周から湧く、射程 24、回収点)は
   *  そのままにした。上限は 4,000 ずつ(合計 12,000 = `MaxEntities` の 18%)。
   *
   *  @warning **`Small` と違い掃引していない。** 段の件数と上限への張り付きは
   *           `EcsSurvivorBenchmark` の出力で確かめること
   */
  [[nodiscard]] constexpr SurvivorParams LargeSurvivorParams() noexcept {
    return SurvivorParams{
        .maxEnemies = 4000, .maxBullets = 4000, .maxPickups = 4000,
        .spawnEveryFrames = 6, .enemiesPerSpawn = 20,
        .spawnRadius = 30.0f, .spawnArc = 3.14159265f,
        .enemySpeed = 3.0f, .enemyHealth = 2.0f, .enemyRadius = 0.8f,
        .fireEveryFrames = 2, .bulletsPerVolley = 10,
        .bulletSpeed = 12.0f, .bulletLifetime = 2.0f, .bulletDamage = 1.0f, .bulletRadius = 0.3f,
        .pickupLifetime = 3.0f, .pickupRadius = 0.3f, .pickupValue = 1.0f,
        .collectRadius = 8.0f, .reachRadius = 1.5f,
    };
  }

  /// 自動の生成・発射をしない構成。**テストが配置を手で決める**
  [[nodiscard]] constexpr SurvivorParams ManualSurvivorParams() noexcept {
    return SurvivorParams{
        .maxEnemies = 1000, .maxBullets = 1000, .maxPickups = 1000,
        .spawnEveryFrames = 0, .enemiesPerSpawn = 0,
        .spawnRadius = 0.0f, .enemySpeed = 0.0f, .enemyHealth = 1.0f, .enemyRadius = 0.5f,
        .fireEveryFrames = 0, .bulletsPerVolley = 0,
        .bulletSpeed = 0.0f, .bulletLifetime = 5.0f, .bulletDamage = 1.0f, .bulletRadius = 0.2f,
        .pickupLifetime = 10.0f, .pickupRadius = 0.3f, .pickupValue = 1.0f,
        .collectRadius = 0.0f, .reachRadius = 0.0f,
    };
  }

  // ===========================================================================
  // 状態
  // ===========================================================================

  /**
   * @brief 1 フレームぶんの出来事
   *
   * @details
   *  **件数を段ごとに分けて持つ**(1-7 の「件数と費用を分けて出す」)。テストは
   *  これらの保存則(生まれた数 = 消えた数 + 生きている数)でループを確かめる。
   */
  struct SurvivorCounts {
    std::uint32_t enemiesSpawned      = 0;
    std::uint32_t bulletsFired        = 0;

    std::uint32_t hitsDelivered       = 0;   ///< 購読者に届いた `HitEvent`
    std::uint32_t hitsIgnored         = 0;   ///< 弾が使い切り済み / 敵が倒れ済み(値で素通り)
    std::uint32_t hitsStale           = 0;   ///< ハンドルがもう生きていない(世代で弾いた)
    std::uint32_t bulletsSpent        = 0;   ///< 命中して破棄を積んだ弾
    std::uint32_t kills               = 0;

    std::uint32_t pickupsCreated      = 0;   ///< 生成と成分の追加を積めた経験値
    std::uint32_t pickupsSkippedAtCap = 0;
    std::uint32_t pickupsLost         = 0;   ///< 倒したのに作れなかった / 積めなかった
    std::uint32_t pickupsCollected    = 0;
    std::uint32_t pickupsExpired      = 0;
    std::uint32_t bulletsExpired      = 0;
    std::uint32_t enemiesReached      = 0;

    std::uint32_t createFailures      = 0;   ///< `CreateEntity` が `Invalid()` を返した
    std::uint32_t buildFailures       = 0;   ///< 即時の `AddComponent` が失敗し、その場で破棄した
    std::uint32_t queueFailures       = 0;   ///< `CommandBuffer` が積めなかった
    std::uint32_t orphans             = 0;   ///< 作ったが、組み立ても破棄も積めなかった
    std::uint32_t gridMismatches      = 0;   ///< `Hit` の直前に構造版が食い違っていた

    void Accumulate(const SurvivorCounts& o) noexcept {
      enemiesSpawned      += o.enemiesSpawned;
      bulletsFired        += o.bulletsFired;
      hitsDelivered       += o.hitsDelivered;
      hitsIgnored         += o.hitsIgnored;
      hitsStale           += o.hitsStale;
      bulletsSpent        += o.bulletsSpent;
      kills               += o.kills;
      pickupsCreated      += o.pickupsCreated;
      pickupsSkippedAtCap += o.pickupsSkippedAtCap;
      pickupsLost         += o.pickupsLost;
      pickupsCollected    += o.pickupsCollected;
      pickupsExpired      += o.pickupsExpired;
      bulletsExpired      += o.bulletsExpired;
      enemiesReached      += o.enemiesReached;
      createFailures      += o.createFailures;
      buildFailures       += o.buildFailures;
      queueFailures       += o.queueFailures;
      orphans             += o.orphans;
      gridMismatches      += o.gridMismatches;
    }
  };

  /**
   * @brief ループの状態
   *
   * @warning **`DetachSurvivor` を呼ぶまで、`EventBus` より長く生きること。**
   *          `AttachSurvivor` が購読者にこのオブジェクトへの参照を渡す。
   *          1-8 では `EventBus` に購読解除が無く、シーンを抜けた時点で
   *          use-after-free になる欠陥だった (1-8 §1-A)。2-1 で解除を入れ、
   *          `SceneManager` が外し忘れを検出して安全網として外すようにした
   */
  struct SurvivorState {
    SurvivorParams      params{};
    ECS::Registry*      registry = nullptr;
    ECS::CommandBuffer* commands = nullptr;

    std::uint64_t frame      = 0;
    float         spawnAngle = 0.0f;
    float         fireAngle  = 0.0f;
    double        experience = 0.0;

    SurvivorCounts thisFrame{};
    SurvivorCounts total{};

    /// `AttachSurvivor` が登録した購読 (2-1)。**`DetachSurvivor` が外す**
    Events::SubscriptionId subscription{};
  };

  // ===========================================================================
  // 小道具
  // ===========================================================================

  namespace SurvivorDetail {
    /// **黄金角で回す。** 固定の 1 点や等間隔にしない。完全に同じ位置へ湧いた
    /// 敵同士は `CollisionSystem` のイプシロンで離れなくなる(§20.7)ので、
    /// 位置が一致しない列を使う
    inline constexpr float kGoldenAngle = 2.39996323f;
    inline constexpr float kTwoPi       = 6.28318531f;

    inline float NextAngle(float& angle) noexcept {
      const float out = angle;
      angle += kGoldenAngle;
      if (angle >= kTwoPi) { angle -= kTwoPi; }
      return out;
    }

    /// その成分を持つ数。単一型の `View` の基準範囲はプールの大きさそのもの
    template <class T>
    [[nodiscard]] std::size_t CountWith(ECS::Registry& registry) noexcept {
      return registry.View<T>().BaseSize();
    }

    /// 積めなかった破棄を次のフレームで再挑戦させるための「まだ正」
    // **`(... ::min)()` と括弧で包む。** ゲーム本体では <windows.h> の `min` マクロが
    // 先に定義されていて、そのままだと関数形式マクロとして展開され壊れる
    // (1-8 のソリューションのリビルドで発覚。テストのビルドでは出なかった)
    inline constexpr float kRetry = (std::numeric_limits<float>::min)();
  }

  // ===========================================================================
  // 生成(即時 API。**反復の外、`GridBuild` より前でだけ呼ぶ**)
  // ===========================================================================

  /**
   * @brief 敵を 1 体作る
   * @return 作れたエンティティ。**失敗したら `Invalid()`**(作りかけは残さない)
   * @note   上限は見ない(呼ぶ側のステップが見る)。テストが配置を手で決めるため
   */
  inline ECS::Entity SpawnEnemy(SurvivorState& s, float x, float y,
                                float vx, float vy) noexcept {
    ECS::Registry&  registry = *s.registry;
    SurvivorCounts& counts   = s.thisFrame;

    const ECS::Entity e = registry.CreateEntity();
    if (!e.IsValid()) {
      ++counts.createFailures;
      return ECS::Entity::Invalid();
    }

    const bool built =
           registry.AddComponent<Components::Position>(e, x, y, 0.0f, 0.0f) != nullptr
        && registry.AddComponent<Components::Velocity>(e, vx, vy, 0.0f, 0.0f) != nullptr
        && registry.AddComponent<Components::Health>(e, s.params.enemyHealth) != nullptr
        && registry.AddComponent<Components::Collider>(e, s.params.enemyRadius) != nullptr;
    if (!built) {
      // **作りかけを残さない**(`BoidDemoScene::OnEnter` と同じ形)
      registry.DestroyEntity(e);
      ++counts.buildFailures;
      return ECS::Entity::Invalid();
    }

    ++counts.enemiesSpawned;
    return e;
  }

  /// @brief 弾を 1 発作る。@copydetails SpawnEnemy
  inline ECS::Entity FireBullet(SurvivorState& s, float x, float y,
                                float vx, float vy) noexcept {
    ECS::Registry&  registry = *s.registry;
    SurvivorCounts& counts   = s.thisFrame;

    const ECS::Entity e = registry.CreateEntity();
    if (!e.IsValid()) {
      ++counts.createFailures;
      return ECS::Entity::Invalid();
    }

    const bool built =
           registry.AddComponent<Components::Position>(e, x, y, 0.0f, 0.0f) != nullptr
        && registry.AddComponent<Components::Velocity>(e, vx, vy, 0.0f, 0.0f) != nullptr
        && registry.AddComponent<Components::Damage>(e, s.params.bulletDamage) != nullptr
        && registry.AddComponent<Components::Lifetime>(e, s.params.bulletLifetime) != nullptr
        && registry.AddComponent<Components::Collider>(e, s.params.bulletRadius) != nullptr;
    if (!built) {
      registry.DestroyEntity(e);
      ++counts.buildFailures;
      return ECS::Entity::Invalid();
    }

    ++counts.bulletsFired;
    return e;
  }

  /**
   * @brief 敵が倒れた位置に経験値を作る(**遅延**)
   *
   * @details
   *  配信中(`DispatchEvents`)に呼ばれる。グリッドを読む処理は終わっているが、
   *  **成分はバッファ経由で積む**。T-ECS-22 が通したいのはこの経路である。
   *
   *  `CreateEntity` はその場でハンドルを返す (R-17) が、成分が付くのは
   *  `ApplyCommands` の後。**同じフレームの `Collect` からは見えない** —
   *  経験値を拾えるのは次のフレームになる(§5.3 の 1 フレーム遅れ)。
   *
   *  **`Lifetime` を最初に積む。** 途中の追加が適用時に失敗しても、
   *  期限で消えるので作りかけが永久に残らない。
   */
  inline void QueuePickup(SurvivorState& s, const Components::Position& at) noexcept {
    ECS::Registry&        registry = *s.registry;
    ECS::CommandBuffer&   commands = *s.commands;
    SurvivorCounts&       counts   = s.thisFrame;
    const SurvivorParams& p        = s.params;

    // **まだ適用されていない分も数える。** プールにはまだ入っていない
    if (SurvivorDetail::CountWith<Components::Pickup>(registry) + counts.pickupsCreated
        >= p.maxPickups) {
      ++counts.pickupsSkippedAtCap;
      return;
    }

    const ECS::Entity xp = registry.CreateEntity();
    if (!xp.IsValid()) {
      ++counts.createFailures;
      ++counts.pickupsLost;
      return;
    }

    const bool queued =
           commands.Add(xp, Components::Lifetime{ p.pickupLifetime })
        && commands.Add(xp, Components::Position{ at.x, at.y, at.z, 0.0f })
        && commands.Add(xp, Components::Collider{ p.pickupRadius, { 0.0f, 0.0f, 0.0f } })
        && commands.Add(xp, Components::Pickup{ p.pickupValue });
    if (!queued) {
      ++counts.queueFailures;
      ++counts.pickupsLost;
      // 積めた分は適用され、この Destroy で消える
      if (!commands.Destroy(xp)) { ++counts.orphans; }
      return;
    }
    ++counts.pickupsCreated;
  }

  // ===========================================================================
  // 命中の解決(購読者。メインスレッド)
  // ===========================================================================

  /**
   * @brief 1 件の `HitEvent` を解決する
   *
   * @details
   *  **1 発で 1 体、1 体に 1 回の破棄**を値で守る(論点3 の c)。
   *   - 弾の `remaining` が 0 以下なら使い切り済み。素通り
   *   - 敵の `current` が 0 以下なら倒れ済み。素通り
   *
   *  **ハンドルが死んでいれば `GetComponent` が `nullptr` を返す**(世代カウンタ。
   *  1-1)。同じ index に別のエンティティが入っていても、古いハンドルでは
   *  その新しい住人に触れない。
   */
  inline void ResolveHit(SurvivorState& s, const Events::HitEvent& hit) noexcept {
    ECS::Registry&      registry = *s.registry;
    ECS::CommandBuffer& commands = *s.commands;
    SurvivorCounts&     counts   = s.thisFrame;

    ++counts.hitsDelivered;

    Components::Lifetime* const       life  = registry.GetComponent<Components::Lifetime>(hit.bullet);
    const Components::Damage* const   dmg   = registry.GetComponent<Components::Damage>(hit.bullet);
    Components::Health* const         hp    = registry.GetComponent<Components::Health>(hit.enemy);
    const Components::Position* const where = registry.GetComponent<Components::Position>(hit.enemy);
    if (life == nullptr || dmg == nullptr || hp == nullptr || where == nullptr) {
      ++counts.hitsStale;
      return;
    }
    if (life->remaining <= 0.0f || hp->current <= 0.0f) {
      ++counts.hitsIgnored;
      return;
    }

    // --- 弾を使い切る ---------------------------------------------------------
    if (!commands.Destroy(hit.bullet)) {
      // 積めなかった。弾は生きたまま。次のフレームでまた当たれば再挑戦になる
      ++counts.queueFailures;
      return;
    }
    life->remaining = 0.0f;
    ++counts.bulletsSpent;

    // --- 敵に当てる -----------------------------------------------------------
    hp->current -= dmg->amount;
    if (hp->current > 0.0f) { return; }

    if (!commands.Destroy(hit.enemy)) {
      // 積めなかった。**倒れ済みにしない**(0 以下にすると二度と積まれず、
      // 生きたまま残る)。次の命中で再挑戦させる
      ++counts.queueFailures;
      hp->current = SurvivorDetail::kRetry;
      return;
    }
    hp->current = 0.0f;
    ++counts.kills;

    // 位置は**今読む**。適用後には敵の成分はもう無い
    QueuePickup(s, *where);
  }

  // ===========================================================================
  // ステップ
  // ===========================================================================

  inline Core::StepResult SpawnEnemiesStep(SurvivorState& s, GameContext&) noexcept {
    const SurvivorParams& p = s.params;
    if (p.spawnEveryFrames == 0u || (s.frame % p.spawnEveryFrames) != 0u) {
      return Core::StepResult::Ran;
    }

    std::size_t alive = SurvivorDetail::CountWith<Components::Health>(*s.registry);
    for (std::uint32_t i = 0; i < p.enemiesPerSpawn; ++i) {
      if (alive >= p.maxEnemies) { break; }   // 上限: 通常の運転。黙って飛ばす

      // 黄金角の列 [0, 2pi) を幅 spawnArc へ縮める。一様さはそのまま保たれる
      const float angle = SurvivorDetail::NextAngle(s.spawnAngle)
                        * (p.spawnArc / SurvivorDetail::kTwoPi);
      const float cx    = std::cos(angle);
      const float cy    = std::sin(angle);
      const ECS::Entity e = SpawnEnemy(s, cx * p.spawnRadius, cy * p.spawnRadius,
                                       -cx * p.enemySpeed, -cy * p.enemySpeed);
      if (!e.IsValid()) { break; }            // 失敗は数えてある。このフレームは打ち切る
      ++alive;
    }
    return Core::StepResult::Ran;
  }

  inline Core::StepResult FireBulletsStep(SurvivorState& s, GameContext&) noexcept {
    const SurvivorParams& p = s.params;
    if (p.fireEveryFrames == 0u || (s.frame % p.fireEveryFrames) != 0u) {
      return Core::StepResult::Ran;
    }

    std::size_t alive = SurvivorDetail::CountWith<Components::Damage>(*s.registry);
    for (std::uint32_t i = 0; i < p.bulletsPerVolley; ++i) {
      if (alive >= p.maxBullets) { break; }

      const float angle = SurvivorDetail::NextAngle(s.fireAngle);
      const ECS::Entity e = FireBullet(s, 0.0f, 0.0f,
                                       std::cos(angle) * p.bulletSpeed,
                                       std::sin(angle) * p.bulletSpeed);
      if (!e.IsValid()) { break; }
      ++alive;
    }
    return Core::StepResult::Ran;
  }

  inline Core::StepResult MovementStep(SurvivorState&, GameContext& ctx) noexcept {
    Systems::MovementSystem::Update(ctx);
    return Core::StepResult::Ran;
  }

  inline Core::StepResult GridBuildStep(SurvivorState&, GameContext& ctx) noexcept {
    Systems::HitSystem::BuildGrid(ctx);
    return (ctx.grid != nullptr) ? Core::StepResult::Ran : Core::StepResult::Failed;
  }

  inline Core::StepResult HitStep(SurvivorState& s, GameContext& ctx) noexcept {
    if (ctx.grid == nullptr) { return Core::StepResult::Skipped; }
    // **Release でも数える。** Debug は `Update` の中の assert で止まるが、
    // Release ではそれが消え、食い違ったまま古い添字で引くことになる
    if (!Systems::HitSystem::GridMatches(ctx)) { ++s.thisFrame.gridMismatches; }
    Systems::HitSystem::Update(ctx);
    return Core::StepResult::Ran;
  }

  inline Core::StepResult DispatchEventsStep(SurvivorState&, GameContext& ctx) noexcept {
    // **`ApplyCommands` の前で配る**(ファイル冒頭の「順序」)。エンジンの
    // `DispatchAll` は残っているが、ここで空にしてあるので空振りになる
    ctx.eventBus->DispatchAll();
    return Core::StepResult::Ran;
  }

  inline Core::StepResult LifetimeStep(SurvivorState& s, GameContext& ctx) noexcept {
    ECS::Registry&      registry = *s.registry;
    ECS::CommandBuffer& commands = *s.commands;
    SurvivorCounts&     counts   = s.thisFrame;

    for (auto entry : registry.View<Components::Lifetime>()) {
      const ECS::Entity     e    = std::get<0>(entry);
      Components::Lifetime& life = std::get<1>(entry);

      if (life.remaining <= 0.0f) { continue; }   // 既に積んである(期限切れ / 命中 / 回収)
      life.remaining -= ctx.dt;
      if (life.remaining > 0.0f) { continue; }

      if (!commands.Destroy(e)) {
        ++counts.queueFailures;
        life.remaining = SurvivorDetail::kRetry;  // 次のフレームで再び 0 をまたぐ
        continue;
      }
      life.remaining = 0.0f;
      if (registry.HasComponent<Components::Damage>(e)) { ++counts.bulletsExpired; }
      else                                            { ++counts.pickupsExpired; }
    }
    return Core::StepResult::Ran;
  }

  inline Core::StepResult CollectStep(SurvivorState& s, GameContext&) noexcept {
    ECS::Registry&        registry = *s.registry;
    ECS::CommandBuffer&   commands = *s.commands;
    SurvivorCounts&       counts   = s.thisFrame;
    const SurvivorParams& p        = s.params;

    for (auto entry : registry.View<Components::Position, Components::Pickup,
                                    Components::Lifetime, Components::Collider>()) {
      const ECS::Entity           e    = std::get<0>(entry);
      const Components::Position& pos  = std::get<1>(entry);
      const Components::Pickup&   pick = std::get<2>(entry);
      Components::Lifetime&       life = std::get<3>(entry);
      const float                 r    = p.collectRadius + std::get<4>(entry).radius;

      if (life.remaining <= 0.0f) { continue; }
      if (pos.x * pos.x + pos.y * pos.y > r * r) { continue; }

      if (!commands.Destroy(e)) {
        ++counts.queueFailures;                   // 値は触らない。次のフレームも範囲内にいる
        continue;
      }
      life.remaining = 0.0f;
      s.experience += pick.value;
      ++counts.pickupsCollected;
    }
    return Core::StepResult::Ran;
  }

  inline Core::StepResult ReachStep(SurvivorState& s, GameContext&) noexcept {
    ECS::Registry&        registry = *s.registry;
    ECS::CommandBuffer&   commands = *s.commands;
    SurvivorCounts&       counts   = s.thisFrame;
    const SurvivorParams& p        = s.params;

    for (auto entry : registry.View<Components::Position, Components::Health,
                                    Components::Collider>()) {
      const ECS::Entity           e   = std::get<0>(entry);
      const Components::Position& pos = std::get<1>(entry);
      Components::Health&         hp  = std::get<2>(entry);
      const float                 r   = p.reachRadius + std::get<3>(entry).radius;

      if (hp.current <= 0.0f) { continue; }       // このフレームで倒れ済み
      if (pos.x * pos.x + pos.y * pos.y > r * r) { continue; }

      if (!commands.Destroy(e)) {
        ++counts.queueFailures;
        continue;
      }
      hp.current = 0.0f;
      ++counts.enemiesReached;
    }
    return Core::StepResult::Ran;
  }

  inline Core::StepResult ApplyCommandsStep(SurvivorState&, GameContext& ctx) noexcept {
    ctx.registry->ApplyCommands(*ctx.commands);
    return Core::StepResult::Ran;
  }

  // ===========================================================================
  // 診断の判断(ログは出さない)
  // ===========================================================================

  /**
   * @brief 作ろうとして作れなかったことを、状態の変わり目で見る (1-8 論点4)
   *
   * @details
   *  **上限 (`maxEnemies` など) で飛ばすのは通常の運転なので数えない。**
   *  数えるのは作ろうとして作れなかったものだけ(`CreateEntity` が `Invalid()`、
   *  成分の追加の失敗、積み込みの失敗)。
   *
   *  レジストリが満杯のあいだは毎フレーム失敗し続ける。**出すのは始まりと終わりの
   *  2 回だけ**で、途中は黙る(R-46)。出力はシーン側が行う。
   */
  [[nodiscard]] inline Core::FailureGate::Change ObserveCreationFailures(
      const SurvivorCounts& counts, Core::FailureGate& gate) noexcept {
    const bool failing = (counts.createFailures | counts.buildFailures
                          | counts.queueFailures | counts.orphans) != 0u;
    return gate.Observe(failing);
  }

  // ===========================================================================
  // 入口
  // ===========================================================================

  /**
   * @brief ループをバスとレジストリへつなぐ。**入室時に 1 回だけ**呼ぶ
   *
   * @details
   *  - **プールを先に全部作る。** `QueuePickup` は配信中に数を数えるので、
   *    そこで初めてプールが作られるとフレームの途中で確保が起きる
   *  - 購読者は `s` を参照で掴む。`SurvivorState` の @warning を参照
   */
  [[nodiscard]] inline bool AttachSurvivor(SurvivorState& s, ECS::Registry& registry,
                                           ECS::CommandBuffer& commands,
                                           Events::EventBus& bus) {
    s.registry = &registry;
    s.commands = &commands;

    (void)registry.View<Components::Position, Components::Velocity, Components::Collider,
                        Components::Health, Components::Damage, Components::Lifetime,
                        Components::Pickup>();

    if (!bus.Register<Events::HitEvent>()) { return false; }

    // **捕捉を持たない関数ポインタ + 文脈** (2-1 / 論点1)。`std::function` は
    // 捕捉が大きいと黙って確保して投げる(2-2 の監査で実測)
    s.subscription = bus.Subscribe<Events::HitEvent>(
        &s, [](void* context, const Events::HitEvent& hit) {
          ResolveHit(*static_cast<SurvivorState*>(context), hit);
        });
    return s.subscription.IsValid();
  }

  /**
   * @brief `AttachSurvivor` の後始末 (ECS 2-1)
   *
   * @details
   *  **対で呼ぶこと。** `SurvivorScene::OnExit` もヘッドレスのテストも、
   *  同じこの関数を通る (§4.7)。外し忘れると、次の配信が解放済みの
   *  `SurvivorState` を触る。
   *
   *  @note 二重に呼んでも安全である(ハンドルは無効化され、`Unsubscribe` は
   *        見つからなければ false を返すだけ)
   */
  inline bool DetachSurvivor(SurvivorState& s, Events::EventBus& bus) noexcept {
    const bool removed = bus.Unsubscribe(s.subscription);
    s.subscription = Events::SubscriptionId{};
    return removed;
  }

  /**
   * @brief 1 フレーム進める
   * @note  `ApplyCommands` まで含む。適用の報告は `ctx.commands->Report()` に残る
   */
  /**
   * @brief **実行順序はここ 1 箇所にある** (R-26)
   *
   * @details
   *  `RunSurvivorFrame` と `EcsSurvivorBenchmark` が**同じ配列を回す**。ベンチは段ごとに
   *  時間を挟むために 1 段ずつ呼ぶが、表を写さないので、順序を変えればベンチにも出る。
   *  1-6 で `kUpdateOrder` がシーンの中にあり、外から見えなかった (§19.7) ことへの答え。
   */
  inline constexpr Core::SystemStep<SurvivorState, GameContext> kSurvivorOrder[] = {
    { "SpawnEnemies",   &SpawnEnemiesStep   },
    { "FireBullets",    &FireBulletsStep    },
    { "Movement",       &MovementStep       },
    { "GridBuild",      &GridBuildStep      },
    { "Hit",            &HitStep            },
    { "DispatchEvents", &DispatchEventsStep },
    { "Lifetime",       &LifetimeStep       },
    { "Collect",        &CollectStep        },
    { "Reach",          &ReachStep          },
    { "ApplyCommands",  &ApplyCommandsStep  },
  };

  /// フレームの頭。このフレームの件数を空にする
  inline void BeginSurvivorFrame(SurvivorState& s) noexcept {
    s.thisFrame = SurvivorCounts{};
  }

  /// フレームの終わり。件数を合計へ足し、フレームを進める
  inline void EndSurvivorFrame(SurvivorState& s) noexcept {
    s.total.Accumulate(s.thisFrame);
    ++s.frame;
  }

  inline void RunSurvivorFrame(SurvivorState& s, GameContext& ctx,
                               Core::FrameReport& report) noexcept {
    assert(s.registry == ctx.registry && s.commands == ctx.commands
           && "RunSurvivorFrame: call AttachSurvivor with the same registry and command "
              "buffer that the context carries");

    BeginSurvivorFrame(s);
    Core::RunSteps(kSurvivorOrder, s, ctx, report);
    EndSurvivorFrame(s);
  }

}
