#pragma once

/**
 * @file  RenderHealthLog.h
 * @brief 描画の失敗の診断の行 (ECS 2-9)
 *
 * @details
 *  判断は `RenderHealth.h`(出力しない)、行を組み立てて出すのはここ。構造は下層、出力は上層
 *  (開発手法 §6.4)。`Run` もテストも同じ関数を呼ぶ。
 *
 *  **毎フレームは出さない。** 出すのは次の 2 つだけ(R-46)。
 *   - 描画を続けられなくなったとき 1 回(その後ゲームは終わる)
 *   - 頂点バッファを書けずにフレームを描かなかったことの、始まり / 止み(`FailureGate`)
 *
 *  ```
 *  [ERROR] render: the graphics device was lost at Present: DXGI_ERROR_DEVICE_REMOVED (0x887A0005),
 *          removed reason DXGI_ERROR_DEVICE_HUNG (0x887A0006). stopping the game
 *  [ERROR] render: Present failed: DXGI_ERROR_INVALID_CALL (0x887A0001). the device was not lost
 *          (removed reason S_OK). stopping the game
 *  [WARN]  render: not drawing this frame. the vertex buffer could not be mapped: E_OUTOFMEMORY (0x8007000E);
 *          the device was not lost
 *     ... 沈黙 ...
 *  [INFO]  render: drawing again after 3 frame(s) (3 frame(s) not drawn in total)
 *  ```
 *  失われていない失敗を「失われた」と書かない。呼び方の誤りを消失と読むと、原因を誤る。
 */

#include "../Core/FailureGate.h"
#include "../Core/Logger.h"
#include "RenderHealth.h"

#include <cstdint>
#include <cstdio>

namespace GLFD::Graphics {

  /// 名前と 16 進を 1 つの文字列にする("DXGI_ERROR_DEVICE_REMOVED (0x887A0005)"、表に無ければ "0x88760870")
  inline void DescribeHResult(char (&out)[64], HRESULT hr) noexcept {
    const char* name = HResultName(hr);
    if (name != nullptr) {
      std::snprintf(out, sizeof out, "%s (0x%08lX)", name, static_cast<unsigned long>(hr));
    }
    else {
      std::snprintf(out, sizeof out, "0x%08lX", static_cast<unsigned long>(hr));
    }
  }

  /// 描画を続けられなくなったときに 1 回だけ呼ぶ(`Run` は呼んだ後に抜ける)
  inline void ReportRenderFailure(const RenderFailure& failure) {
    char returned[64];
    char reason[64];
    DescribeHResult(returned, failure.returned);
    DescribeHResult(reason, failure.removedReason);
    const char* where = (failure.where != nullptr) ? failure.where : "?";
    if (IsDeviceRemoved(failure)) {
      LOG_ERROR("render: the graphics device was lost at %s: %s, removed reason %s. stopping the game",
                where, returned, reason);
    }
    else {
      LOG_ERROR("render: %s failed: %s. the device was not lost (removed reason %s). stopping the game",
                where, returned, reason);
    }
  }

  /// 頂点バッファを書けずに描かなかったことの診断の状態。**`Run` が持つ**
  struct SkippedDrawReport {
    Core::FailureGate gate;
    std::uint64_t     skippedTotal = 0;
  };

  /**
   * @brief フレームごとに呼ぶ。描かなかったフレームの始まり / 止みだけ 1 行出す
   * @param skipped     このフレームで、頂点バッファを書けずに描かなかったか
   * @param mapReturned そのときの `Map` の戻り値(始まりの行にだけ載る)
   */
  inline void ReportSkippedDraws(bool skipped, HRESULT mapReturned, SkippedDrawReport& report) {
    if (skipped) { ++report.skippedTotal; }
    const Core::FailureGate::Change change = report.gate.Observe(skipped);
    if (change == Core::FailureGate::Change::Started) {
      char returned[64];
      DescribeHResult(returned, mapReturned);
      LOG_WARN("render: not drawing this frame. the vertex buffer could not be mapped: %s; the device was not lost",
               returned);
    }
    else if (change == Core::FailureGate::Change::Recovered) {
      LOG_INFO("render: drawing again after %u frame(s) (%llu frame(s) not drawn in total)",
               report.gate.LastStreakLength(), static_cast<unsigned long long>(report.skippedTotal));
    }
  }

}
