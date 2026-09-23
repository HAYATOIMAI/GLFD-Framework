#pragma once
/**
 * @file  ParallelFor.h
 * @brief 仕事を塊に分けて並列に回す。**分け方の規則はここ 1 か所**(ECS 2-6)
 *
 * @details
 *  ## なぜ 1 か所にまとめたか
 *  2-5 までは 7 つのシステムが `batchSize = count / WorkerThreadCount()` を同じ形で
 *  書いていた。規則を変えるときに個別に直すと、1 か所だけ違う規則が残る
 *  (開発手法 §7.8)。
 *
 *  ## 規則
 *   - 本数 = min(maxChunks, ceil(count / grain))。count が 0 なら何もしない
 *   - **1 本ならその場で呼ぶ。** 積まず、起こさず、待たない
 *   - 2 本以上なら積んで `WaitFor` する。範囲は均等に分ける(`count * i / chunks`)
 *     ので**空の塊は出ない**。以前は count < 20 のとき先頭 19 本が空だった
 *
 *  ## grain(粒度)= 1 本あたりの最小の対象数
 *  **1 本起こす費用は約 1.5 µs で、起こす側(呼んだスレッド)が払う**
 *  (2-6 手順1。`notify_one` が眠っているワーカーを起こす費用で、呼んだ回数ではなく
 *  起こしたスレッドの数で決まる)。1 本あたりの仕事がそれより十分大きくなければ、
 *  分けるほど遅くなる。grain は段ごとに、測った 1 体あたりの費用から決める。
 *  grain = 1 なら今までと同じ本数(maxChunks)になる。
 *
 *  ## 状態機械は変えない(2-4 の停止の正しさ)
 *  `KickJob` / `WaitFor` / ワーカーの主ループ / `Stop` には触れない。
 *   - 1 本の経路は `JobSystem` に触れない。積まないので世代も件数も動かず、
 *     **取り残され得るジョブがそもそも存在しない**
 *   - 2 本以上の経路は、以前と同じ `KickJob` と `WaitFor` の並びで本数が変わるだけ
 *  どちらも 2-4 の状態機械に遷移を足さない。呼んでよいのは `KickJob` と同じく
 *  所有スレッドだけ(`Stop` と並行しない)。
 *
 *  ## body は参照で持つ
 *  ジョブは `body` を**参照で**持つ。この関数は `WaitFor` の前には戻らないので、
 *  参照は実行の間ずっと有効である。取り込みが小さくなり(参照 1 つと範囲 2 つ)、
 *  `std::function` の小さな領域に収まる。
 *  **body は複数のワーカーから同時に呼ばれる。** body が触る共有の値は、読むだけに
 *  するか、塊ごとに別の場所へ書くこと(以前の各システムの約束と同じ)。
 */

#include <cstddef>

#include "JobSystem.h"
#include "../Core/HardwareConstants.h"

namespace GLFD::Thread {

  /// 分け方の結果。塊 i は [ChunkBegin(count, chunks, i), ChunkBegin(count, chunks, i + 1))
  struct ChunkPlan {
    std::size_t chunks = 0;
  };

  /**
   * @brief 本数を決める。**規則はここだけ**
   * @param grain 1 本あたりの最小の対象数。0 は 1 として扱う
   * @param maxChunks 本数の上限。0 は 1 として扱う
   */
  [[nodiscard]] constexpr ChunkPlan PlanChunks(std::size_t count, std::size_t grain,
                                               std::size_t maxChunks) noexcept {
    if (count == 0) { return ChunkPlan{ 0 }; }
    if (grain == 0) { grain = 1; }
    if (maxChunks == 0) { maxChunks = 1; }
    // ceil(count / grain)。count + grain - 1 は grain が大きいと溢れるので割り算で書く
    const std::size_t byGrain = count / grain + ((count % grain != 0) ? 1u : 0u);
    return ChunkPlan{ (byGrain < maxChunks) ? byGrain : maxChunks };
  }

  /// 上限をワーカーの本数にした PlanChunks
  [[nodiscard]] inline ChunkPlan PlanChunks(std::size_t count, std::size_t grain) noexcept {
    return PlanChunks(count, grain, System::WorkerThreadCount());
  }

  /**
   * @brief 塊 i の始まり。均等に分けるので、chunks <= count なら**空の塊は出ない**
   * @note count * i は count が 2^59 を超えると溢れる(64 ビット、i <= 32 として)。
   *       実体の数はそこまで届かない
   */
  [[nodiscard]] constexpr std::size_t ChunkBegin(std::size_t count, std::size_t chunks,
                                                 std::size_t i) noexcept {
    return (chunks == 0) ? 0 : (count * i) / chunks;
  }

  /**
   * @brief 1 本あたりの目標の仕事 [us](ECS 2-6 手順3b)。**全段で 1 つ**
   * @details 1 本起こす費用は約 1.5 us で起こす側が払い、積んでからワーカーで走り始めるまで
   *  約 4.3 us かかる(2-6 手順1)。1 本の仕事がこれより小さくなる分け方はしない。
   *
   *  **8 us は 4 / 8 / 16 / 32 us を振って決めた**(手順3b、3a と交互に各 30 回):
   *   - Survivor small は 4〜32 のどれでも全段が 1 本になり、差が無い(中央値 12.0〜12.1 us)
   *   - Survivor large の中央値: 4 = 79.3、8 = 66.9、16 = 67.3、32 = 72.6 us。
   *     8 は 4 と 32 より明確に良い(ブロックの範囲が重ならない)。8 と 16 は見分けられない
   *     (中央値も p95 も範囲が重なる)ので、**本数の変化が少ない方(8)**を選んだ
   *   - Boid は BoidSystem / CollisionSystem が 20 本のまま。Movement / GridBuild だけ本数が減る
   *  1 体あたりの費用と同じく、**機械が変わったら振り直す値**
   */
  inline constexpr double kTargetChunkUs = 8.0;

  /**
   * @brief 粒度 = ceil(targetChunkUs / 1 体あたりの費用)。**1 未満にはしない**
   * @param costPerEntityUs 段ごとに測った 1 体あたりの費用 [us]。0 以下なら 1 を返す
   */
  [[nodiscard]] constexpr std::size_t GrainFor(double costPerEntityUs,
                                              double targetChunkUs = kTargetChunkUs) noexcept {
    if (!(costPerEntityUs > 0.0) || !(targetChunkUs > 0.0)) { return 1; }
    const double q = targetChunkUs / costPerEntityUs;
    if (!(q > 1.0)) { return 1; }
    if (q >= 1.0e15) { return static_cast<std::size_t>(1.0e15); }   // 実体の数より十分大きい
    const std::size_t n = static_cast<std::size_t>(q);
    return (static_cast<double>(n) < q) ? n + 1 : n;
  }

  /**
   * @brief body(start, end) を [0, count) の上で呼ぶ
   * @details 1 本ならその場で呼ぶ(何も起こさない)。2 本以上なら積んで WaitFor する。
   *          **所有スレッドから呼ぶこと**(KickJob と同じ前提。Debug で assert される)
   */
  template <class Body>
  void ParallelForChunks(JobSystem& jobSystem, std::size_t count, std::size_t grain, Body&& body) {
    const ChunkPlan plan = PlanChunks(count, grain);
    if (plan.chunks == 0) { return; }
    if (plan.chunks == 1) {
      body(std::size_t{ 0 }, count);
      return;
    }

    JobCounter counter;
    JobHandle handle = jobSystem.CreateHandle(counter);
    for (std::size_t i = 0; i < plan.chunks; ++i) {
      const std::size_t start = ChunkBegin(count, plan.chunks, i);
      const std::size_t end   = ChunkBegin(count, plan.chunks, i + 1);
      jobSystem.KickJob([&body, start, end]() { body(start, end); }, &handle);
    }
    jobSystem.WaitFor(handle);
  }

}
