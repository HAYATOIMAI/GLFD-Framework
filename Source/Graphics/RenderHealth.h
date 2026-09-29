#pragma once

/**
 * @file  RenderHealth.h
 * @brief 描画の呼び出しの戻り値を分ける。デバイスが失われたら、正しく終わる (ECS 2-9)
 *
 * @details
 *  ## なぜ要るか
 *  2-8 までは `Present` の戻り値のうち `DXGI_STATUS_OCCLUDED` だけを見ていた。2-9 の手順1 で
 *  `Present` に `DXGI_ERROR_DEVICE_REMOVED` を返させると、**画面が止まり、ループが 1 秒に約 11 万回
 *  空回りし(1.25 コア)、ログに 1 行も出なかった**。頂点バッファの `Map` の失敗も、古い頂点を
 *  黙って描き続けていた。
 *
 *  ## 決めたこと(2-9 の判断 1: A)
 *  失われたら理由を 1 回だけログに残し、通常の終了の経路(join → ログ → Engine Shutdown)を
 *  通ってから知らせ、0 以外の終了コードで終わる。作り直して続ける形は採っていない(負債に記録)。
 *
 *  ## 見方: `FAILED(hr)`
 *  `DXGI_STATUS_OCCLUDED`(0x087A0001)は**成功の値**なので、`FAILED` の側に入らない。
 *  `FAILED` で見れば、`DEVICE_REMOVED` / `RESET` / `HUNG` / `DRIVER_INTERNAL_ERROR` と、
 *  値を確かめられなかった `D3DDDIERR_DEVICEREMOVED`(Present の文書にある)をまとめて拾う。
 *  **本物の消失で何が返るかは、この機械では確かめられていない**(dxcap -forcetdr では
 *  デバイスが失われなかった)。拾えるのは「返った値が失敗なら」まで。
 *
 *  ## TDR のときの順番(2-9 で本物を観察)
 *  `dxcap -forcetdr` を流すと、`Present` は**消失より先に OCCLUDED を返した**(0.9 秒止まって戻った)。
 *  止まっている間の確かめ直し(`DXGI_PRESENT_TEST`)の値も同じ判定に通す。そうしないと、
 *  確かめ直しで消失の値が返っても「見えた」と読んで、黙って描画に戻る。
 *
 *  **この層は出力しない。** DX11 も `Logger` も引き込まない(`<winerror.h>` だけ)。
 *  本番の `DX11Renderer` とテスト (T-ECS-43) が同じ関数を呼ぶ。行は `RenderHealthLog.h`。
 */

#include <winerror.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace GLFD::Graphics {

  /// `Present` / `Present(TEST)` の結果
  enum class PresentOutcome : std::uint8_t {
    Shown,      ///< 成功(OCCLUDED 以外の成功の値も含む)
    Occluded,   ///< 窓が見えない。止めて眠る (2-8)
    Failed,     ///< 失敗。デバイスが失われたかは `GetDeviceRemovedReason` で分ける
  };

  [[nodiscard]] constexpr PresentOutcome ClassifyPresent(HRESULT hr) noexcept {
    if (FAILED(hr)) { return PresentOutcome::Failed; }
    if (hr == DXGI_STATUS_OCCLUDED) { return PresentOutcome::Occluded; }
    return PresentOutcome::Shown;
  }

  /// 頂点バッファの `Map` の結果
  enum class MapOutcome : std::uint8_t {
    Written,      ///< 書けた
    SkipFrame,    ///< 失敗したが、デバイスは失われていない。**このフレームは描かない**(古い頂点を描かない)
    DeviceLost,   ///< 失敗し、デバイスが失われていた。`Present` の失敗と同じく終わる
  };

  /**
   * @param map           `Map` の戻り値
   * @param removedReason `Map` が失敗したときに読んだ `GetDeviceRemovedReason()`(成功なら見ない)
   * @note  書き込みだけの `Map`(`WRITE_DISCARD`)が消失で何を返すかは、文書に書かれていない
   *        (文書にあるのは CPU から読む Map だけ)。失敗なら理由で分ける
   */
  [[nodiscard]] constexpr MapOutcome ClassifyMap(HRESULT map, HRESULT removedReason) noexcept {
    if (SUCCEEDED(map)) { return MapOutcome::Written; }
    return (removedReason == S_OK) ? MapOutcome::SkipFrame : MapOutcome::DeviceLost;
  }

  /// 描画を続けられなくなった最初の 1 回。**後から来たもので上書きしない**
  struct RenderFailure {
    bool        failed        = false;
    const char* where         = nullptr;   ///< "Present" / "Present(TEST)" / "Map"(文字列リテラル。確保しない)
    HRESULT     returned      = S_OK;      ///< その呼び出しが返した値
    HRESULT     removedReason = S_OK;      ///< `GetDeviceRemovedReason()`。S_OK ならデバイスは失われていない
  };

  /// まだ記録が無ければ記録して true。記録済みなら何もせず false
  inline bool RecordFailure(RenderFailure& failure, const char* where, HRESULT returned,
                            HRESULT removedReason) noexcept {
    if (failure.failed) { return false; }
    failure.failed        = true;
    failure.where         = where;
    failure.returned      = returned;
    failure.removedReason = removedReason;
    return true;
  }

  /**
   * @brief `Present` の結果の状態。`DX11Renderer` が持ち、**本番もテストも下の 3 つの関数で動かす**
   *        (手順を写さない。開発手法 §4.7)
   */
  struct PresentState {
    bool          occluded = false;   ///< 前の Present が OCCLUDED だった(止めて眠る。2-8)
    RenderFailure failure;            ///< 描画を続けられなくなった最初の 1 回
  };

  /**
   * @brief `Present(1, 0)` の後
   * @param reason 失敗したときだけ呼ぶ `GetDeviceRemovedReason`(成功のたびに呼ばない)
   */
  template <class ReasonFn>
  void AfterPresent(PresentState& state, HRESULT hr, ReasonFn&& reason) {
    const PresentOutcome outcome = ClassifyPresent(hr);
    if (outcome == PresentOutcome::Failed) {
      RecordFailure(state.failure, "Present", hr, reason());
    }
    state.occluded = (outcome == PresentOutcome::Occluded);
  }

  /**
   * @brief 止まっている間の確かめ直し `Present(0, DXGI_PRESENT_TEST)` の後
   * @return まだ止まっているか。**失敗なら記録して true**(止まったまま。`Run` が失敗を見て終わる)
   * @note  TDR では Present が消失より先に OCCLUDED を返した(2-9 で本物を観察)。ここで
   *        「OCCLUDED でなければ見えた」と読むと、消失の値が返っても黙って描画に戻る
   */
  template <class ReasonFn>
  [[nodiscard]] bool AfterPresentTest(PresentState& state, HRESULT hr, ReasonFn&& reason) {
    const PresentOutcome outcome = ClassifyPresent(hr);
    if (outcome == PresentOutcome::Failed) {
      RecordFailure(state.failure, "Present(TEST)", hr, reason());
      return true;
    }
    state.occluded = (outcome == PresentOutcome::Occluded);
    return state.occluded;
  }

  /**
   * @brief 頂点バッファの `Map` の後
   * @return `Written` なら書いてよい。それ以外は**このフレームは描かない**(`DeviceLost` は記録済み)
   */
  template <class ReasonFn>
  [[nodiscard]] MapOutcome AfterMap(RenderFailure& failure, HRESULT hr, ReasonFn&& reason) {
    if (SUCCEEDED(hr)) { return MapOutcome::Written; }
    const HRESULT removed = reason();
    const MapOutcome outcome = ClassifyMap(hr, removed);
    if (outcome == MapOutcome::DeviceLost) {
      RecordFailure(failure, "Map", hr, removed);
    }
    return outcome;
  }

  /// デバイスが失われていたか。失われていない失敗(呼び方の誤りなど)を「失われた」と書かないため
  [[nodiscard]] constexpr bool IsDeviceRemoved(const RenderFailure& failure) noexcept {
    return failure.failed && failure.removedReason != S_OK;
  }

  /// 値の名前。表に無ければ nullptr(行の側で 16 進だけにする)
  [[nodiscard]] inline const char* HResultName(HRESULT hr) noexcept {
    switch (hr) {
      case S_OK:                             return "S_OK";
      case DXGI_STATUS_OCCLUDED:             return "DXGI_STATUS_OCCLUDED";
      case DXGI_ERROR_DEVICE_REMOVED:        return "DXGI_ERROR_DEVICE_REMOVED";
      case DXGI_ERROR_DEVICE_HUNG:           return "DXGI_ERROR_DEVICE_HUNG";
      case DXGI_ERROR_DEVICE_RESET:          return "DXGI_ERROR_DEVICE_RESET";
      case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
      case DXGI_ERROR_INVALID_CALL:          return "DXGI_ERROR_INVALID_CALL";
      case DXGI_ERROR_WAS_STILL_DRAWING:     return "DXGI_ERROR_WAS_STILL_DRAWING";
      case E_OUTOFMEMORY:                    return "E_OUTOFMEMORY";
      case E_INVALIDARG:                     return "E_INVALIDARG";
      case E_FAIL:                           return "E_FAIL";
      default:                               return nullptr;
    }
  }

  /// ゲームの終わり方。`main` が終了コードと知らせの文を決める
  enum class ExitReason : std::uint8_t {
    Normal,          ///< × で閉じた、など
    StartupFailed,   ///< 起動できなかった(DX11 の初期化、開始のシーン)
    RenderFailed,    ///< 実行中に描画を続けられなくなった(デバイスの消失を含む)
  };

  /// 終了コード。確認の道具は「0 以外は失敗」とだけ読む(値に意味を持たせない。開発手法 §4.14a)
  [[nodiscard]] constexpr int ExitCodeFor(ExitReason reason) noexcept {
    switch (reason) {
      case ExitReason::Normal:        return 0;
      case ExitReason::StartupFailed: return 1;
      case ExitReason::RenderFailed:  return 2;
    }
    return 3;
  }

  /// 知らせの文の長さ(`wchar_t`、終端を含む)
  inline constexpr std::size_t kExitMessageLength = 1024;

  /**
   * @brief 利用者への知らせの文(メッセージボックス用)を組み立てる
   * @param what      起動の失敗なら何ができなかったか(ASCII)。描画の失敗なら使わない
   * @param workDir   作業フォルダ(起動の失敗の多くは、ここを取り違えて起きる)
   * @return 書いた文字数。`Normal` なら空文字列を書いて 0
   */
  inline int FormatExitMessage(wchar_t (&out)[kExitMessageLength], ExitReason reason, const RenderFailure& failure,
                               const char* what, const wchar_t* workDir) noexcept {
    out[0] = L'\0';
    const wchar_t* folder = (workDir != nullptr) ? workDir : L"(unknown)";
    if (reason == ExitReason::StartupFailed) {
      return std::swprintf(out, kExitMessageLength,
                           L"The game could not start: %hs.\n\n"
                           L"Working folder: %ls\n"
                           L"The game reads Resource\\ and Source\\Shaders\\ from this folder. "
                           L"Start it from the repository root (or from Visual Studio).\n\n"
                           L"Log: %ls\\Game.log",
                           (what != nullptr) ? what : "unknown", folder, folder);
    }
    if (reason == ExitReason::RenderFailed) {
      const char* returned = HResultName(failure.returned);
      const char* reasonName = HResultName(failure.removedReason);
      const wchar_t* head = IsDeviceRemoved(failure)
                                ? L"The graphics device was lost"
                                : L"Drawing failed (the graphics device was not lost)";
      return std::swprintf(out, kExitMessageLength,
                           L"%ls, so the game has stopped.\n\n"
                           L"%hs returned %hs (0x%08lX). removed reason: %hs (0x%08lX).\n\n"
                           L"Log: %ls\\Game.log",
                           head, (failure.where != nullptr) ? failure.where : "?",
                           (returned != nullptr) ? returned : "unknown",
                           static_cast<unsigned long>(failure.returned),
                           (reasonName != nullptr) ? reasonName : "unknown",
                           static_cast<unsigned long>(failure.removedReason), folder);
    }
    return 0;
  }

}
