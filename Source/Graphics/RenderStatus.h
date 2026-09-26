#pragma once

/**
 * @file  RenderStatus.h
 * @brief 1 フレームの描画がどうなったか (ECS 1-6 / R-28)。ECS 2-3 で `RenderSystem.h` から移した
 *
 * @details
 *  **1-1 からの借りの返済。** それまでは確保に失敗すると黙って 1 フレーム描かずに
 *  戻っていた(コードにも「1-6 で観測点を用意すること」と書いてあった)。
 *
 *  **記録するだけで `Logger` を呼ばない。** 出力はシーンが
 *  `Game/EcsDiagnosticsLog.h` の `ReportRenderStep` と `FailureGate` の組で行う。
 *  JSON の `ArchiveContext` と同じ分担。
 *
 *  **ECS も DX11 も引き込まない。** Game 側で頂点を組む関数(`Game/SurvivorRender.h`)が
 *  この型を返し、テストがそれを直接見る。
 *
 *  @note 名前空間は `GLFD::Systems` のまま(`RenderSystem` と同じ)。置き場所の見直しは
 *        `RenderSystem.h` の負債と一緒に扱う(ECS 2-3 の記録)。
 */

#include <cstddef>
#include <cstdint>

namespace GLFD::Systems {

  struct RenderStatus {
    enum class Outcome : std::uint8_t {
      Drawn,                    ///< 描いた
      VertexBufferUnavailable,  ///< 頂点の一時配列を確保できなかった
      ComponentsUnavailable,    ///< 成分のプールを確保できていなかった (1-5 で増えた経路)
    };

    Outcome     outcome           = Outcome::Drawn;
    std::size_t requestedVertices = 0;   ///< 描いた数、または確保しようとした数
  };

}
