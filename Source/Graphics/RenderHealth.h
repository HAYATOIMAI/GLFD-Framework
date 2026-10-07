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
 *  【訂正】18:53 の `dxcap -forcetdr` で本物の消失を捉えた。`Present` は DXGI_ERROR_DEVICE_REMOVED を
 *  返し、理由は DXGI_ERROR_DEVICE_RESET だった(差し込みと同じ振る舞いで終わった)。`Map` が何を
 *  返すかは確かめていない(Present で先に見つかった)
 *  【訂正 2】「Present で先に見つかった」は誤り。Boid のフレームは `DrawPoints`(Map)を `EndFrame`(Present)
 *  より先に呼ぶ。18:53 のログには `at Map` の行も `not drawing this frame` の行も無いので、失敗した
 *  Present の直前の Map は失敗を返していない。そのときデバイスがすでに失われていたかは分からない。
 *  書き込みだけの Map が失われたデバイスで成功を返すなら、消失を確実に見つけられるのは Present だけになる
 *
 *  ## `dxcap -forcetdr` を流したときの順番(2-9 で 2 回。順番は 1 通りではない)
 *   - 17:14: `Present` が OCCLUDED を返し、0.9 秒止まって戻った。描画は続いた。ドライバと
 *     Windows Error Reporting の記録は無い。**本物の TDR だったかは分からない**
 *     (【訂正】以前は「デバイスは TDR で生き残った」と書いていた)
 *   - 18:53: 約 6.9 秒止まった後(どの呼び出しで止まったかは分からない)、OCCLUDED を経ずに
 *     `Present` が DXGI_ERROR_DEVICE_REMOVED を返した。ドライバの記録と LKD_0x117 (TDR) がある
 *  止まっている間の確かめ直し(`DXGI_PRESENT_TEST`)の値も同じ判定に通す。**OCCLUDED の後に確かめ
 *  直しで失敗が返る順番は観察していない**が、起こり得る経路として塞ぐ。見ないと「見えた」と読んで、
 *  黙って描画に戻る。
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
   * @note  OCCLUDED の後に確かめ直しで失敗が返る順番は、観察していない(17:14 は OCCLUDED の後に戻り、
   *        18:53 は OCCLUDED を経ずに失敗した)。起こり得る経路として塞ぐ。ここで「OCCLUDED でなければ
   *        見えた」と読むと、失敗の値が返っても黙って描画に戻る
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
   * @brief 知らせの 1 行目: 何が起きたかを、理由(`GetDeviceRemovedReason`)ごとに平易に書く
   * @note  2-9 で本物の消失を捉えたとき (18:53)、英語の "The graphics device was lost" はユーザーに
   *        「デバイスが見つからない」と読まれた。起きたのは Windows による GPU のリセット (TDR。理由
   *        DXGI_ERROR_DEVICE_RESET)。見当違いの対処(GPU を探す、挿し直す)を招かない文にする。
   *        RESET の文は文書の説明(「誤った命令」)ではなく、観察した出来事(外からのリセット)で書く
   */
  [[nodiscard]] inline const wchar_t* ExitHeadline(const RenderFailure& failure) noexcept {
    if (!IsDeviceRemoved(failure)) { return L"描画の呼び出しが失敗しました。GPU は使える状態のままです (デバイスは失われていません)。"; }
    switch (failure.removedReason) {
      case DXGI_ERROR_DEVICE_RESET:          return L"Windows がグラフィックス デバイス (GPU) をリセットしたため、描画を続けられなくなりました。";
      case DXGI_ERROR_DEVICE_HUNG:           return L"GPU が応答しなくなり、Windows がリセットしたため、描画を続けられなくなりました。";
      case DXGI_ERROR_DEVICE_REMOVED:        return L"グラフィックス ドライバが更新・再起動されたか、GPU が取り外されたため、描画を続けられなくなりました。";
      case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return L"グラフィックス ドライバの内部エラーで GPU がリセットされたため、描画を続けられなくなりました。";
      case DXGI_ERROR_INVALID_CALL:          return L"ゲームが描画に誤った呼び出しをしたため、描画を続けられなくなりました (ゲームの不具合です)。";
      default:                               return L"GPU が使えない状態になったため、描画を続けられなくなりました。";
    }
  }

  /// 知らせの 2 段目: どうすればよいか。
  /// `logWritten` が false (Game.log を開けなかった) なら、Game.log を送るよう頼まない (ECS 2-11)
  [[nodiscard]] inline const wchar_t* ExitAdvice(const RenderFailure& failure, bool logWritten = true) noexcept {
    if (!IsDeviceRemoved(failure) || failure.removedReason == DXGI_ERROR_INVALID_CALL) {
      return logWritten ? L"ゲームの不具合の可能性が高いので、Game.log を送ってください。" : L"ゲームの不具合の可能性が高いです。";
    }
    switch (failure.removedReason) {
      case DXGI_ERROR_DEVICE_RESET:
      case DXGI_ERROR_DEVICE_HUNG:
      case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return logWritten ? L"GPU が無くなったわけではありません。ゲームをもう一度起動してください。\n何度も起きる場合は、グラフィックス ドライバを更新し、Game.log を送ってください。"
                          : L"GPU が無くなったわけではありません。ゲームをもう一度起動してください。\n何度も起きる場合は、グラフィックス ドライバを更新してください。";
      default:
        return logWritten ? L"ゲームをもう一度起動してください。\n何度も起きる場合は、グラフィックス ドライバを更新し、Game.log を送ってください。"
                          : L"ゲームをもう一度起動してください。\n何度も起きる場合は、グラフィックス ドライバを更新してください。";
    }
  }

  /// 知らせの最後の行の頭と終わり: ログの場所か、書けなかったこと (ECS 2-11)
  [[nodiscard]] inline const wchar_t* ExitLogLabel(bool logWritten) noexcept {
    return logWritten ? L"ログ: " : L"Game.log を書けませんでした (";
  }
  [[nodiscard]] inline const wchar_t* ExitLogTail(bool logWritten) noexcept { return logWritten ? L"" : L")"; }

  /**
   * @brief 利用者への知らせの文(メッセージボックス用)を組み立てる
   * @details 何が起きたか → どうすればよいか → 値の名前(最後の行)→ ログの場所、の順。
   *          日本語の文はこのヘッダ(UTF-8 BOM)のワイド文字列だけに置く。テストは \\u で照らす
   * @param what      起動の失敗なら何ができなかったか(ASCII)。描画の失敗なら使わない
   * @param workDir   作業フォルダ(起動の失敗の多くは、ここを取り違えて起きる)
   * @param logWritten Game.log に書けているか。false なら「送ってください」の代わりに「書けませんでした」(ECS 2-11)
   * @return 書いた文字数。`Normal` なら空文字列を書いて 0
   */
  inline int FormatExitMessage(wchar_t (&out)[kExitMessageLength], ExitReason reason, const RenderFailure& failure,
                               const char* what, const wchar_t* workDir, bool logWritten = true) noexcept {
    out[0] = L'\0';
    const wchar_t* folder = (workDir != nullptr) ? workDir : L"(unknown)";
    if (reason == ExitReason::StartupFailed) {
      return std::swprintf(out, kExitMessageLength,
                           L"ゲームを起動できませんでした。\n原因: %hs\n\n作業フォルダ: %ls\nゲームはこのフォルダから Resource\\ と Source\\Shaders\\ を読みます。リポジトリの直下 (または Visual Studio) から起動してください。\n\n%ls%ls\\Game.log%ls",
                           (what != nullptr) ? what : "unknown", folder,
                           ExitLogLabel(logWritten), folder, ExitLogTail(logWritten));
    }
    if (reason == ExitReason::RenderFailed) {
      const char* returned = HResultName(failure.returned);
      const char* reasonName = HResultName(failure.removedReason);
      return std::swprintf(out, kExitMessageLength,
                           L"%ls\n\n%ls\n\n"
                           L"%hs: %hs (0x%08lX) / removed reason: %hs (0x%08lX)\n"
                           L"%ls%ls\\Game.log%ls",
                           ExitHeadline(failure), ExitAdvice(failure, logWritten),
                           (failure.where != nullptr) ? failure.where : "?",
                           (returned != nullptr) ? returned : "unknown",
                           static_cast<unsigned long>(failure.returned),
                           (reasonName != nullptr) ? reasonName : "unknown",
                           static_cast<unsigned long>(failure.removedReason),
                           ExitLogLabel(logWritten), folder, ExitLogTail(logWritten));
    }
    return 0;
  }

}
