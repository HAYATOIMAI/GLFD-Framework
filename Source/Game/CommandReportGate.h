#pragma once

/**
 * @file  CommandReportGate.h
 * @brief ECS 1-8: 適用結果の「何を出すか」を決める(**ログは出さない**)
 *
 * @details
 *  ## 1-4 の前提が崩れた
 *  `ReportAppliedCommands` は 1-4 で「デモはコマンドを 1 つも積まない」前提で作った。
 *  何か適用されたフレームには毎回 INFO を 1 行、二重破棄には 1 件ずつ WARN を出していた。
 *  1-8 では弾の寿命で**毎フレーム破棄が起きる**ので、そのままだと毎秒 60 行以上流れる。
 *  1-6 の R-46(状態の変わり目だけ出す)を適用する。1-4 からの借りの返済である。
 *
 *  ## 判断と出力を分ける
 *  **1-4 から今まで、この判断にはテストが 1 件も無かった。** 判断が
 *  `EcsDiagnosticsLog.h` の中にあり、そのヘッダは `Logger` と `RenderSystem`(DX11)を
 *  引き込むのでテストから呼べなかった。判断だけをこの軽いヘッダへ出し、
 *  `Logger` を呼ぶのは従来どおり `EcsDiagnosticsLog.h` にする。
 *
 *  ## 何を出すか
 *   - **最初の適用だけは空でも出す**(1-4 の目的: 「動いた」と
 *     「コマンドを 1 つも積まないので動いた」を区別する)
 *   - **適用件数は出さない。** 積んで適用するのは通常の運転である
 *   - **取りこぼしは状態の変わり目だけ。** 始まったフレームで件数と明細を 1 回、
 *     戻ったフレームで続いたフレーム数を 1 回
 *   - 始まったフレームの取りこぼしが**すべて二重破棄**なら WARN。1 件でも別の理由が
 *     あるか、明細が上限で切れて理由が分からなければ ERROR
 */

#include "../Core/FailureGate.h"
#include "../ECS/CommandBuffer.h"

#include <cstdint>

namespace GLFD::Game {

  /// 1 フレームの適用について、出すべきもの
  struct AppliedCommandsNotice {
    /// 最初の適用。**空でも出す**
    bool firstFlush = false;
    /// 取りこぼしの状態の変わり目。`None` なら何も出さない
    Core::FailureGate::Change drops = Core::FailureGate::Change::None;
    /// `drops == Started` のとき、取りこぼしがすべて二重破棄だったか
    bool onlyDoubleDestroys = false;
  };

  /**
   * @brief 適用結果から、出すべきものを決める
   * @param loggedFirstApply 最初の適用を出したか。**呼び出し側が所有する**(N-3)
   * @param dropGate         取りこぼしの門。**呼び出し側が所有する**
   */
  [[nodiscard]] inline AppliedCommandsNotice DecideAppliedCommands(const ECS::ApplyReport& report,
                                                                   bool& loggedFirstApply,
                                                                   Core::FailureGate& dropGate) noexcept {
    AppliedCommandsNotice notice;

    if (!loggedFirstApply) {
      loggedFirstApply  = true;
      notice.firstFlush = true;
    }

    notice.drops = dropGate.Observe(report.Dropped() != 0u);

    if (notice.drops == Core::FailureGate::Change::Started) {
      // 明細が件数に届いていなければ、理由の分からない取りこぼしがある
      bool only = (report.RecordedCount() == report.Dropped());
      for (std::uint32_t i = 0; i < report.RecordedCount() && only; ++i) {
        only = (report.Recorded(i).reason == ECS::DropReason::AlreadyDestroyed);
      }
      notice.onlyDoubleDestroys = only;
    }
    return notice;
  }

}
