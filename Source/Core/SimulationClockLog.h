#pragma once

/**
 * @file  SimulationClockLog.h
 * @brief 固定の刻みの診断の行 (ECS 2-8)
 *
 * @details
 *  数えるのは `SimulationClock.h`(出力しない)、行を組み立てて出すのはここ。
 *  構造は下層、出力は上層(開発手法 §6.4)。`Run` もテストも同じ関数を呼ぶ。
 *
 *  **毎フレーム・毎秒は出さない。** 出すのは次の 2 つだけ(R-46)。
 *   - 上限を超えて刻みを捨て始めたとき / 止んだとき(`FailureGate`)
 *   - 起動して落ち着いたところで 1 回、フレームの速さとシミュレーションの速さ
 *
 *  ```
 *  [WARN] simulation: falling behind real time. dropped 58 step(s) (0.93 s) this frame; at most 4 step(s) run per frame
 *     ... 捨て続けている間は沈黙 ...
 *  [INFO] simulation: keeping up again after 3 frame(s) (61 step(s) = 0.98 s dropped in that stretch, 61 in total)
 *  ```
 */

#include "FailureGate.h"
#include "Logger.h"
#include "SimulationClock.h"

#include <cstdint>
#include <cstdio>

namespace GLFD::Core {

  /// 刻みを捨てたことの診断の状態。**`Run` が持つ**(関数ローカルの `static` にしない。N-3)
  struct StepDropReport {
    FailureGate   gate;
    std::uint64_t droppedInStretch = 0;   ///< 今の(直前の)続きで捨てた刻み
    std::uint64_t droppedTotal     = 0;   ///< 起動してからの合計
  };

  /// フレームごとに呼ぶ。状態が変わったときだけ 1 行出す
  inline void ReportStepDrops(const StepPlan& plan, std::uint32_t maxStepsPerFrame, StepDropReport& report) {
    const bool dropping = (plan.droppedSteps != 0u);
    if (dropping) {
      report.droppedInStretch += plan.droppedSteps;
      report.droppedTotal     += plan.droppedSteps;
    }
    const FailureGate::Change change = report.gate.Observe(dropping);
    if (change == FailureGate::Change::Started) {
      LOG_WARN("simulation: falling behind real time. dropped %llu step(s) (%.2f s) this frame; "
               "at most %u step(s) run per frame",
               static_cast<unsigned long long>(plan.droppedSteps),
               static_cast<double>(plan.droppedSteps) * static_cast<double>(kSimulationStep),
               maxStepsPerFrame);
    }
    else if (change == FailureGate::Change::Recovered) {
      LOG_INFO("simulation: keeping up again after %u frame(s) (%llu step(s) = %.2f s dropped in that stretch, "
               "%llu in total)",
               report.gate.LastStreakLength(),
               static_cast<unsigned long long>(report.droppedInStretch),
               static_cast<double>(report.droppedInStretch) * static_cast<double>(kSimulationStep),
               static_cast<unsigned long long>(report.droppedTotal));
      report.droppedInStretch = 0;
    }
  }

  /**
   * @brief 起動して落ち着いたところで 1 回だけ出す
   * @param refreshHz 窓があるモニターのリフレッシュレート。分からなければ 0 以下
   */
  inline void ReportStartupRate(const StartupRateProbe::Result& measured, int refreshHz) {
    char monitor[48];
    if (refreshHz > 0) { std::snprintf(monitor, sizeof monitor, "monitor %d Hz", refreshHz); }
    else               { std::snprintf(monitor, sizeof monitor, "monitor refresh unknown"); }
    LOG_INFO("frame rate: %.1f fps over %.1f s (%s). simulation: %.1f step(s)/s of %.3f s = %.2f x real time",
             measured.framesPerSecond, measured.seconds, monitor,
             measured.stepsPerSecond, static_cast<double>(kSimulationStep), measured.simulatedPerReal);
  }

}
