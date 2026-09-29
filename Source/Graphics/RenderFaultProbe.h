#pragma once

/**
 * @file  RenderFaultProbe.h
 * @brief 描画の呼び出しの戻り値を、確認用のビルドでだけ差し替える口 (ECS 2-9)
 *
 * @details
 *  デバイスの消失は普段の実行では起きず、この機械では本物を起こせなかった(`dxcap -forcetdr` では
 *  失われなかった)。消失の経路を決定的に通すため、戻り値を差し替える。
 *
 *  **2-4 の `JobSystemProbe.h` と同じ形。**
 *   - `GLFD_RENDER_FAULT_PROBE` を定義したビルドでだけ、`Inject` を呼ぶ。定義の無いビルドでは
 *     `Call` は `real()` を呼ぶだけで、差し込みの呼び出しごと消える
 *   - `Inject` は宣言だけ。定義は確認用のビルドにだけ入る `Tests/RenderFaultProbe.cpp` にある。
 *     本番のビルドに `GLFD_RENDER_FAULT_PROBE` が紛れ込めば、定義が無いのでリンクエラーになる
 *   - 確認用のビルドは別の出力に建てる(vcxproj の `GLFDProbeDefines`)。本番のソースを道具が
 *     書き換えない(開発手法 §4.14b)
 *  差し替えるときは、**本物の呼び出しをしない**(手順1 の差し込みと同じ。失われたデバイスの
 *  `Present` は待たずに戻ると見て、その形を再現する)。
 */

#include <winerror.h>

#include <cstdint>

namespace GLFD::Graphics::RenderFaultProbe {

  /// 差し込む点
  enum class Point : std::uint8_t {
    Present,         ///< `EndFrame` の `Present(1, 0)`
    PresentTest,     ///< `IsOccluded` の `Present(0, DXGI_PRESENT_TEST)`
    Map,             ///< `DrawPoints` の頂点バッファの `Map`
    RemovedReason,   ///< `GetDeviceRemovedReason()`
  };

#if defined(GLFD_RENDER_FAULT_PROBE)
  /// 差し替えるなら `replaced` に値を入れて true。宣言だけ(定義は Tests/RenderFaultProbe.cpp)
  bool Inject(Point point, HRESULT& replaced) noexcept;
#endif

  /// 本番: `real()` を呼ぶだけ。確認用: 差し替えがあれば、本物を呼ばずにその値を返す
  template <class Real>
  [[nodiscard]] inline HRESULT Call([[maybe_unused]] Point point, Real&& real) {
#if defined(GLFD_RENDER_FAULT_PROBE)
    HRESULT replaced = S_OK;
    if (Inject(point, replaced)) { return replaced; }
#endif
    return real();
  }

}
