#pragma once

/**
 * @file  SimulationClock.h
 * @brief シミュレーションを**固定の刻み**で、**実時間に合わせて**進める (ECS 2-8)
 *
 * @details
 *  ## なぜ要るか
 *  2-7 までは `Run` が毎フレーム `Update(0.016f)` を 1 回呼んでいた。フレームの数は
 *  モニターと窓の状態で決まるので、**ゲームの速さもそれで変わっていた**。
 *  2-8 の手順1 で測ると、144 Hz のモニターの前面で実時間の 2.30 倍、最小化すると
 *  `Present` が待たなくなり 316〜336 倍(Survivor)だった。
 *
 *  ## 形: 固定の刻み + 貯め込み
 *  実時間を貯め、1 刻みぶん貯まるたびに `Update` を 1 回呼ぶ。1 フレームに 0 回のことも
 *  複数回のこともある。**各システムに渡る `dt` は `kSimulationStep` のまま**なので、
 *  `dt` を掛けるもの・フレームの数で数えるもの(湧く・撃つ)・`dt` を足すもの(寿命)・
 *  フレームごとの押し返し(衝突)が、**すべて「1 刻みあたり」の量のまま実時間にそろう**。
 *  可変の `dt` はそれだけでは直らず(フレームの数で動くものが残る)、60 に抑える形は
 *  144 Hz の垂直同期では作れないので採らなかった(手順1 §1-F)。
 *
 *  ## 数え方: ナノ秒の整数
 *  1 刻み = 16,000,000 ns ちょうど。貯めるのも引くのも整数なので、**丸めの誤差が溜まらない**。
 *  `float` の秒で貯めると、長く回すうちにシミュレーションの時間が実時間から離れていく。
 *
 *  ## 上限
 *  1 フレームで進める刻みは `kMaxStepsPerFrame` まで。超えた分は**捨てる**(追いつこうと
 *  して 1 フレームが長くなり、さらに遅れる形を断つ)。1 刻みに満たない端数は残す。
 *  捨てたことは戻り値で返し、出力は上の層が状態の変わり目だけ行う(R-46)。
 *
 *  **この層は出力しない。** Win32 も Logger も引き込まない。時刻は呼ぶ側が渡すので、
 *  テストが実時間の列を差し込んで決定的に確かめられる (T-ECS-41)。
 */

#include <cstdint>

namespace GLFD::Core {

  /// 1 刻みの長さ(マイクロ秒)。**刻みの値はここ 1 箇所**。テストとベンチも参照する
  inline constexpr std::uint32_t kSimulationStepMicroseconds = 16000;

  /// 1 刻みの長さ(ナノ秒)。貯め込みはこの単位の整数で行う
  inline constexpr std::int64_t kSimulationStepNanoseconds =
      static_cast<std::int64_t>(kSimulationStepMicroseconds) * 1000;

  /// 各システムに渡す `dt`(秒)
  inline constexpr float kSimulationStep = static_cast<float>(kSimulationStepMicroseconds) / 1000000.0f;

  static_assert(kSimulationStep == 0.016f,
                "kSimulationStep: the tests, the benchmarks and every recorded number so far ran with "
                "dt = 0.016. Changing the step changes every per-step quantity at once (spawn and fire "
                "intervals, the collision push, lifetimes); see SimulationClock.h before touching it");

  /**
   * @brief 1 フレームで進める刻みの数の上限
   * @details 追いつこうとしているフレームでも、刻みに使う時間を 1 刻み(16 ms)の半分以下に
   *          抑える。そうすれば、そのフレームは費やした時間より多くの実時間ぶん進み、追いつく
   *          方向に働く。2-8 で測った 1 刻みの費用の最大は Boid の長く回した区間の p95
   *          1.71 ms(`EcsBenchmark 3300 3000`)。8 ms / 1.71 ms = 4.7 → 4。
   *          1 フレームで最大 64 ms 進む。15.6 fps を下回ると捨て始める。
   */
  inline constexpr std::uint32_t kMaxStepsPerFrame = 4;

  /// `FixedStepAccumulator::Advance` の結果
  struct StepPlan {
    std::uint32_t steps        = 0;   ///< このフレームで進める刻みの数(0〜上限)
    std::uint64_t droppedSteps = 0;   ///< 上限を超えて捨てた刻みの数
  };

  /**
   * @brief 実時間を貯め、1 刻みぶん貯まるたびに 1 回進めさせる
   * @note  時刻の出どころを知らない。呼ぶ側が前のフレームからの経過(ナノ秒)を渡す
   */
  class FixedStepAccumulator {
  public:
    explicit FixedStepAccumulator(std::uint32_t maxStepsPerFrame = kMaxStepsPerFrame) noexcept
        : m_maxSteps(maxStepsPerFrame == 0u ? 1u : maxStepsPerFrame) {}

    /**
     * @brief 前のフレームからの経過を足し、このフレームで進める刻みの数を返す
     * @param elapsedNanoseconds 0 以下なら何も足さない(時計が戻った・同じ時刻)
     * @note  経過に上限は設けない。貯め込みは 64 ビットのナノ秒で、桁があふれるのは約 292 年ぶんの
     *        経過から。1 時間で打ち切る守りを置いたが、外しても何も変わらないことを変異 (N6) で
     *        確かめたので外した(何も変えない守りは置かない)
     */
    [[nodiscard]] StepPlan Advance(std::int64_t elapsedNanoseconds) noexcept {
      StepPlan plan;
      if (elapsedNanoseconds > 0) {
        m_pending += elapsedNanoseconds;
      }

      const std::uint64_t due = static_cast<std::uint64_t>(m_pending / kSimulationStepNanoseconds);
      if (due > m_maxSteps) {
        plan.steps         = m_maxSteps;
        plan.droppedSteps  = due - m_maxSteps;
        m_pending         %= kSimulationStepNanoseconds;   // 捨てるのは刻みの単位まで。端数は残す
      }
      else {
        plan.steps  = static_cast<std::uint32_t>(due);
        m_pending  -= static_cast<std::int64_t>(due) * kSimulationStepNanoseconds;
      }
      return plan;
    }

    /// 貯まっていて、まだ刻みにしていない時間(ナノ秒)。常に 0 以上 1 刻み未満
    [[nodiscard]] std::int64_t PendingNanoseconds() const noexcept { return m_pending; }

    [[nodiscard]] std::uint32_t MaxStepsPerFrame() const noexcept { return m_maxSteps; }

  private:
    std::int64_t  m_pending = 0;
    std::uint32_t m_maxSteps;
  };

  /**
   * @brief 1 フレームぶん進める。**`Run` もテストもこれを呼ぶ**(手順を写さない。開発手法 §4.7)
   *
   * @param input 刻みの頭で `BeginStep()` を呼ぶ相手(`InputSystem` / `KeyEdgeLatch`)。
   *              押された瞬間は**ちょうど 1 回の刻みに**届く。刻みが 0 回のフレームでは
   *              届かず、次のフレームの最初の刻みへ持ち越される
   * @param step  1 刻みぶん進める処理
   */
  template <class Input, class StepFn>
  StepPlan RunFixedSteps(FixedStepAccumulator& clock, Input& input, std::int64_t elapsedNanoseconds,
                         StepFn&& step) {
    const StepPlan plan = clock.Advance(elapsedNanoseconds);
    for (std::uint32_t i = 0; i < plan.steps; ++i) {
      input.BeginStep();
      step();
    }
    return plan;
  }

  /**
   * @brief 起動して落ち着いたところで 1 回だけ、フレームの速さとシミュレーションの速さを測る
   *
   * @details 2-7 までは FPS を数えて捨てていたので、**ゲームが 2.3 倍で進んでいても誰も
   *          気づかなかった**(開発手法 §6.6)。起動から `kSettleNanoseconds` 待ち、そこから
   *          `kWindowNanoseconds` の間のフレームと刻みを数える。毎フレーム・毎秒は出さない。
   */
  class StartupRateProbe {
  public:
    static constexpr std::int64_t kSettleNanoseconds = 1000000000LL;   ///< 起動直後の 1 秒は捨てる
    static constexpr std::int64_t kWindowNanoseconds = 2000000000LL;   ///< そこから 2 秒を数える

    struct Result {
      double framesPerSecond = 0.0;
      double stepsPerSecond  = 0.0;
      double simulatedPerReal = 0.0;   ///< シミュレーションの時間 / 実時間
      double seconds          = 0.0;   ///< 数えた区間の長さ
    };

    explicit StartupRateProbe(std::int64_t startNanoseconds) noexcept : m_start(startNanoseconds) {}

    /**
     * @brief フレームの終わりに呼ぶ
     * @return 測り終えた**その 1 回だけ** true。以後は `Result()` を読む
     */
    [[nodiscard]] bool OnFrame(std::int64_t nowNanoseconds, std::uint32_t steps) noexcept {
      if (m_done) { return false; }
      if (!m_counting) {
        if (nowNanoseconds - m_start < kSettleNanoseconds) { return false; }
        m_counting    = true;
        m_windowStart = nowNanoseconds;   // このフレームの終わりから数え始める
        return false;
      }
      ++m_frames;
      m_steps += steps;
      const std::int64_t span = nowNanoseconds - m_windowStart;
      if (span < kWindowNanoseconds) { return false; }
      const double seconds     = static_cast<double>(span) / 1e9;
      m_result.seconds          = seconds;
      m_result.framesPerSecond  = static_cast<double>(m_frames) / seconds;
      m_result.stepsPerSecond   = static_cast<double>(m_steps) / seconds;
      m_result.simulatedPerReal = static_cast<double>(m_steps) * static_cast<double>(kSimulationStepNanoseconds)
                                / static_cast<double>(span);
      m_done = true;
      return true;
    }

    [[nodiscard]] const Result& Measured() const noexcept { return m_result; }

  private:
    std::int64_t  m_start;
    std::int64_t  m_windowStart = 0;
    std::uint64_t m_frames      = 0;
    std::uint64_t m_steps       = 0;
    bool          m_counting    = false;
    bool          m_done        = false;
    Result        m_result{};
  };

}
