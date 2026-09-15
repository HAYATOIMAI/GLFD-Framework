#pragma once

/**
 * @file  EcsDiagnosticsLog.h
 * @brief ECS の診断を**ログに出す層** (ECS 1-6 / R-28)
 *
 * @details
 *  ## なぜこのファイルがあるか
 *  1-4 の時点では整形が `BoidDemoScene.cpp` に直書きされていた(1 箇所)。
 *  1-6 で診断を足すと **5 箇所**になり、しかも `OnUpdate` と `OnRender` の
 *  2 つのメソッドに割れる:
 *
 *   1. `ApplyReport` の明細(1-4 から移設)
 *   2. 実行順序の各ステップの結果 (`FrameReport`)
 *   3. `RenderSystem` の頂点バッファ確保失敗
 *   4. `RenderSystem` の成分プール確保失敗
 *   5. `FailureGate` の開始 / 復帰の文言
 *
 *  ## 層の分け方(JSON と同じ分担)
 *
 *  | 層 | JSON | ECS |
 *  |---|---|---|
 *  | 構造(記録するだけ) | `ArchiveContext` | `ECS::ApplyReport` / `Core::FrameReport` / `RenderStatus` |
 *  | **出力**(`Logger` を呼ぶ) | `Core/GameConfigLog.h` | **このファイル** |
 *
 *  **`Source/ECS/` は `Logger` を include しない。** 1-6 の調査時点でそれは
 *  成立しており(ECS からのログは 0 件)、崩さない。
 *
 *  @note JSON にある「行を組み立てるだけの中間層」(`JsonDiagnostics.h`)は
 *        **作らない**。あれは `Core/Json` が `Logger` を知らないという純度要求と、
 *        パス/Issue の整形の複雑さに対する答えである。ECS の文言は `%s` と整数が
 *        数個で、中間層を挟むと char バッファへ手で組み立てる費用だけが増える。
 */

#include "../Core/FailureGate.h"
#include "../Core/Logger.h"
#include "CommandReportGate.h"
#include "SurvivorLoop.h"
#include "../Core/SystemSchedule.h"
#include "../ECS/CommandBuffer.h"
#include "../Events/EventBus.h"
#include "../Events/Events.h"
#include "../Graphics/RenderSystem.h"

#include <cstdint>

namespace GLFD::Game {

  // ---------------------------------------------------------------------------
  // 名前付け
  // ---------------------------------------------------------------------------

  [[nodiscard]] inline const char* ToText(Core::StepResult result) noexcept {
    switch (result) {
      case Core::StepResult::Ran:     return "ran";
      case Core::StepResult::Skipped: return "skipped";
      case Core::StepResult::Failed:  return "FAILED";
    }
    return "?";
  }

  [[nodiscard]] inline const char* ToText(ECS::CommandKind kind) noexcept {
    switch (kind) {
      case ECS::CommandKind::Destroy: return "destroy";
      case ECS::CommandKind::Add:     return "add";
      case ECS::CommandKind::Remove:  return "remove";
    }
    return "?";
  }

  [[nodiscard]] inline const char* ToText(ECS::DropReason reason) noexcept {
    switch (reason) {
      case ECS::DropReason::DeadEntity:       return "the entity was not alive";
      case ECS::DropReason::AlreadyDestroyed: return "already destroyed";
      case ECS::DropReason::AlreadyPresent:   return "the component was already there";
      case ECS::DropReason::AllocationFailed: return "allocation failed";
    }
    return "?";
  }

  [[nodiscard]] inline const char* ToText(Systems::RenderStatus::Outcome outcome) noexcept {
    switch (outcome) {
      case Systems::RenderStatus::Outcome::Drawn:
        return "drawn";
      case Systems::RenderStatus::Outcome::VertexBufferUnavailable:
        return "the vertex buffer could not be sized from frame memory";
      case Systems::RenderStatus::Outcome::ComponentsUnavailable:
        return "the component pool could not be allocated";
    }
    return "?";
  }

  // ---------------------------------------------------------------------------
  // 実行順序の報告
  // ---------------------------------------------------------------------------

  /**
   * @brief 1 フレームのステップ結果を、**状態が変わったときだけ**出す
   *
   * @details
   *  平常時は全ステップが `Ran` なので**1 行も出ない**。どれかが `Failed` /
   *  `Skipped` になった最初のフレームで内訳を出し、以降は直るまで黙る。
   *
   *  **沈黙が「状態が変わっていない」を意味する。** 直ったときに、何フレーム
   *  続いたかを添えて 1 行出す。
   */
  inline void ReportFrameSteps(const Core::FrameReport& report, Core::FailureGate& gate) {
    const Core::FailureGate::Change change = gate.Observe(!report.AllRan());

    if (change == Core::FailureGate::Change::Recovered) {
      LOG_INFO("frame steps: all systems ran again after %u frame(s) (%u bad frame(s) total)",
               gate.LastStreakLength(), gate.TotalFailures());
      return;
    }
    if (change != Core::FailureGate::Change::Started) {
      return;                       // 平常、または同じ状態が続いている
    }

    LOG_ERROR("frame steps: %u failed, %u skipped of %u",
              report.CountOf(Core::StepResult::Failed),
              report.CountOf(Core::StepResult::Skipped),
              report.StepCount());

    for (std::uint32_t i = 0; i < report.StepCount(); ++i) {
      const Core::StepResult result = report.ResultAt(i);
      if (result == Core::StepResult::Ran) {
        continue;                   // 走ったものは並べない(内訳が読めなくなる)
      }
      LOG_ERROR("  [%u] %s: %s", i, report.NameAt(i), ToText(result));
    }
    if (report.Truncated()) {
      LOG_ERROR("  (the step report ran out of room; raise FrameReport::kMaxSteps)");
    }
  }

  // ---------------------------------------------------------------------------
  // 描画の報告
  // ---------------------------------------------------------------------------

  /// @copydoc ReportFrameSteps
  inline void ReportRenderStep(const Systems::RenderStatus& status, Core::FailureGate& gate) {
    const bool failing = (status.outcome != Systems::RenderStatus::Outcome::Drawn);
    const Core::FailureGate::Change change = gate.Observe(failing);

    if (change == Core::FailureGate::Change::Started) {
      LOG_ERROR("RenderSystem: not drawing this frame. %s (%zu vertices requested)",
                ToText(status.outcome), status.requestedVertices);
    }
    else if (change == Core::FailureGate::Change::Recovered) {
      LOG_INFO("RenderSystem: drawing again after %u frame(s) (%u dropped frame(s) total)",
               gate.LastStreakLength(), gate.TotalFailures());
    }
  }

  // ---------------------------------------------------------------------------
  // 衝突の観測 (1-7 / R-27)
  // ---------------------------------------------------------------------------

  /**
   * @brief 衝突が実際に動いていることの観測点 (1-7)
   *
   * @details
   *  **ECS-0 で「衝突が 1 件も検出されていない」ことに誰も気づかなかったのは、
   *  購読者が 0 件で観測点が存在しなかったため**である。同じ状態のまま
   *  修復すると、直ったかどうかも分からない。
   *
   *  出し方は 1-4 / 1-6 と同じ考え方:
   *   - **初めて衝突が届いたフレームだけ** 1 行出す(動いたことの証拠)
   *   - **キューの取りこぼし**は `FailureGate` で状態の変わり目だけ出す
   *
   *  毎フレームの件数は出さない。衝突数は毎フレーム変わるので、出すと
   *  60 行/秒になる。
   *
   *  @param published / dropped この 1 フレームで発行された数と捨てられた数
   *  @param delivered  購読者に届いた数(**1 フレーム前のぶん**。発行は
   *                    `OnUpdate` 中、配信は `DispatchAll` なので 1 フレームずれる)
   */
  inline void ReportCollisionObservation(std::uint32_t published, std::uint32_t dropped,
                                         std::uint32_t delivered, bool& loggedFirstHit,
                                         Core::FailureGate& overflowGate) {
    if (!loggedFirstHit && delivered != 0u) {
      loggedFirstHit = true;
      LOG_INFO("collision: first frame with hits. published=%u delivered=%u dropped=%u",
               published, delivered, dropped);
    }

    const Core::FailureGate::Change change = overflowGate.Observe(dropped != 0u);
    if (change == Core::FailureGate::Change::Started) {
      // **黙って捨てない** (R-28)。ここは 1-7 まで printf がコメントアウト
      // されていた場所である
      LOG_ERROR("event queue overflow: dropped %u of %u published this frame. "
                "the channel holds %zu",
                dropped, published,
                Events::EventChannel<Events::CollisionEvent>::QUEUE_CAPACITY);
    }
    else if (change == Core::FailureGate::Change::Recovered) {
      LOG_INFO("event queue: no longer overflowing after %u frame(s) "
               "(%u bad frame(s) total)",
               overflowGate.LastStreakLength(), overflowGate.TotalFailures());
    }
  }

  // ---------------------------------------------------------------------------
  // 1-8 のループの観測 (SurvivorScene)
  // ---------------------------------------------------------------------------

  /// 各段が**初めて起きたフレーム**を出したか。**呼び出し側が所有する**(N-3)
  struct SurvivorLapLog {
    bool spawned   = false;
    bool fired     = false;
    bool hit       = false;
    bool killed    = false;
    bool dropped   = false;
    bool collected = false;
    bool expired   = false;
    bool reached   = false;
    bool complete  = false;
  };

  /**
   * @brief ループの各段が実際に動いたことを、**段ごとに 1 回だけ**出す (1-8)
   *
   * @details
   *  1-7 の「初めて衝突が届いたフレームだけ 1 行」と同じ考え方。**「クラッシュしない」を
   *  「動いた」と読まない**ための観測点で、全段がそろった時点でその旨を 1 行出す。
   *  毎フレームの件数は出さない。
   */
  inline void ReportSurvivorFirstLap(const SurvivorState& s, SurvivorLapLog& log) {
    const SurvivorCounts&    f     = s.thisFrame;
    // `RunSurvivorFrame` がフレームを進めた後に呼ばれるので 1 つ戻す
    const unsigned long long frame = (s.frame == 0u) ? 0ull
                                                     : static_cast<unsigned long long>(s.frame - 1u);

    const auto once = [frame](bool& done, std::uint32_t count, const char* what) {
      if (done || count == 0u) { return; }
      done = true;
      LOG_INFO("survivor: first %s at frame %llu (%u in that frame)", what, frame, count);
    };
    once(log.spawned,   f.enemiesSpawned,                     "enemy spawned");
    once(log.fired,     f.bulletsFired,                       "bullet fired");
    once(log.hit,       f.hitsDelivered,                      "hit delivered");
    once(log.killed,    f.kills,                              "kill");
    once(log.dropped,   f.pickupsCreated,                     "experience dropped");
    once(log.collected, f.pickupsCollected,                   "experience collected");
    once(log.expired,   f.pickupsExpired + f.bulletsExpired,  "expiry");
    once(log.reached,   f.enemiesReached,                     "enemy reaching the player");

    if (!log.complete && log.spawned && log.fired && log.hit && log.killed && log.dropped
        && log.collected && log.expired && log.reached) {
      log.complete = true;
      LOG_INFO("survivor: every stage of the loop has run at least once (by frame %llu)", frame);
    }
  }

  /// 作ろうとして作れなかったことを、**始まりと終わりだけ**出す (1-8 論点4 / R-46)
  inline void ReportSurvivorCreation(const SurvivorCounts& f, Core::FailureGate::Change change,
                                     const Core::FailureGate& gate) {
    if (change == Core::FailureGate::Change::Started) {
      LOG_ERROR("survivor: could not create entities this frame (create %u, build %u, "
                "queue %u, orphans %u). ECS::MaxEntities = %zu",
                f.createFailures, f.buildFailures, f.queueFailures, f.orphans,
                ECS::MaxEntities);
    }
    else if (change == Core::FailureGate::Change::Recovered) {
      LOG_INFO("survivor: creating entities again after %u frame(s) (%u bad frame(s) total)",
               gate.LastStreakLength(), gate.TotalFailures());
    }
  }

  /**
   * @brief イベントキューの取りこぼしを、状態の変わり目だけ出す (R-28)
   * @note  1-8 では**取りこぼした `HitEvent` はそのまま命中の見逃し**になる
   */
  inline void ReportEventQueue(const Events::BusCounters& counters, Core::FailureGate& gate) {
    const Core::FailureGate::Change change = gate.Observe(counters.dropped != 0u);
    if (change == Core::FailureGate::Change::Started) {
      LOG_ERROR("event queue overflow: dropped %u of %u published this frame. "
                "each channel holds %zu",
                counters.dropped, counters.published,
                Events::EventChannel<Events::HitEvent>::QUEUE_CAPACITY);
    }
    else if (change == Core::FailureGate::Change::Recovered) {
      LOG_INFO("event queue: no longer overflowing after %u frame(s) (%u bad frame(s) total)",
               gate.LastStreakLength(), gate.TotalFailures());
    }
  }

  /// 経過の要約。**呼び出し側が間隔を決める**(毎フレームは呼ばない)
  inline void ReportSurvivorSummary(const SurvivorState& s, std::size_t enemies,
                                    std::size_t bullets, std::size_t pickups,
                                    std::uint32_t alive) {
    const SurvivorCounts& t = s.total;
    LOG_INFO("survivor: frame %llu alive %u (enemies %zu, bullets %zu, experience %zu). "
             "so far: kills %u, collected %u, reached %u, experience %.0f",
             static_cast<unsigned long long>(s.frame), alive, enemies, bullets, pickups,
             t.kills, t.pickupsCollected, t.enemiesReached, s.experience);
  }

  // ---------------------------------------------------------------------------
  // コマンドバッファの報告 (1-4 から移設)
  // ---------------------------------------------------------------------------

  /**
   * @brief 適用結果を出す (1-4 / 1-8 で R-46 を適用)
   *
   * @details
   *  **何を出すかは `DecideAppliedCommands`(`CommandReportGate.h`)が決める。**
   *  ここは決まったものを整形するだけ。1-8 までは判断もここにあり、
   *  何か適用されたフレームに毎回 1 行出していた(「コマンドが 0 件」の前提で
   *  作ったため)。破棄が毎フレーム起きる 1-8 でその前提が崩れた。
   *
   * @param loggedFirstApply 1 回目だけは空でも出すためのフラグ。**呼び出し側が
   *        所有する**(N-3: ここで `static` を持たない)
   * @param dropGate         取りこぼしの門。**呼び出し側が所有する**
   *
   * @note **1 回目だけは空でも出す。** 「デモが動いた」ではなく
   *       **「デモはコマンドを 1 つも積まないので動いた」**を区別できるように
   *       するため。積み始めてから壊れたときに、原因を誤らないための記録である。
   */
  inline void ReportAppliedCommands(const ECS::ApplyReport& report, bool& loggedFirstApply,
                                    Core::FailureGate& dropGate) {
    const AppliedCommandsNotice notice = DecideAppliedCommands(report, loggedFirstApply, dropGate);

    if (notice.firstFlush) {
      LOG_INFO("command buffer: first flush. applied=%u dropped=%u",
               report.Applied(), report.Dropped());
    }

    if (notice.drops == Core::FailureGate::Change::Recovered) {
      LOG_INFO("command buffer: no longer dropping after %u frame(s) (%u bad frame(s) total)",
               dropGate.LastStreakLength(), dropGate.TotalFailures());
      return;
    }
    if (notice.drops != Core::FailureGate::Change::Started) {
      return;                       // 平常、または同じ状態が続いている
    }

    // **黙って捨てない** (R-20 / R-28)。件数は正確で、明細だけが上限で切れる
    if (notice.onlyDoubleDestroys) {
      // 二重破棄は VS 型では起こり得るので**異常として出さない**
      LOG_WARN("command buffer: %u command(s) dropped, all double destroys (%u recorded)",
               report.Dropped(), report.RecordedCount());
    }
    else {
      LOG_ERROR("command buffer: %u command(s) dropped (%u recorded%s)",
                report.Dropped(), report.RecordedCount(),
                report.Truncated() ? ", detail truncated" : "");
    }

    for (std::uint32_t i = 0; i < report.RecordedCount(); ++i) {
      const ECS::DroppedCommand& dropped = report.Recorded(i);
      if (dropped.reason == ECS::DropReason::AlreadyDestroyed) {
        LOG_WARN("  dropped %s: %s (entity index %u, generation %u)",
                 ToText(dropped.kind), ToText(dropped.reason),
                 dropped.entity.Index(), dropped.entity.Generation());
      }
      else {
        LOG_ERROR("  dropped %s: %s (entity index %u, generation %u)",
                  ToText(dropped.kind), ToText(dropped.reason),
                  dropped.entity.Index(), dropped.entity.Generation());
      }
    }
  }

}
