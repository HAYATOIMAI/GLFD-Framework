#pragma once

/**
 * @file  SceneManager.h
 * @brief シーンのスタックと遷移 (ECS 2-1)
 *
 * @details
 *  ## 出力はしない (§6.4)
 *  **`Logger` を知らない。** 何が起きたかは `TransitionReport` に数として
 *  残し、行を組み立てて出すのは `Engine` の仕事である。
 *  2-2 の監査で `SceneManager.cpp` が CLEAN になるのはこのためで、
 *  1 つでも例外依存を許すと、**その行は以後何も検出しなくなる**。
 *
 *  ## 確保は先に済ませる
 *  `PushScene` / `ChangeScene` は `[[nodiscard]] bool` を返す (N-2)。
 *  処理側も**古いシーンの `OnExit` を呼ぶ前に**スタックの容量を確保する。
 *  逆順にすると、確保に失敗したときシーンが 1 つも無い状態が残る。
 */

#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"
#include "IScene.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace GLFD {
  struct GameContext;
}

namespace GLFD::Scene {

  /**
   * @brief 遷移で何が起きたか (ECS 2-1 / R-28)
   *
   * @details
   *  **確保しない。** `ApplyReport`(1-4 / R-38)と同じ理由である。
   *  数え続け、`Engine` が状態の変わり目だけ出す (R-46)。
   */
  struct TransitionReport {
    std::uint32_t transitions        = 0;  ///< 実際に起きた遷移の回数
    std::uint32_t refused            = 0;  ///< 確保に失敗して取りやめた回数
    std::uint32_t leakedSubscriptions = 0; ///< OnExit が外し忘れ、安全網が外した数
    std::uint32_t leakedEntities     = 0;  ///< OnExit が残したエンティティの数
    std::uint32_t sweptEntities      = 0;  ///< そのうち安全網が破棄した数
    std::uint32_t discardedCommands  = 0;  ///< 遷移時に捨てた積み残しコマンドの数

    [[nodiscard]] bool HasProblem() const noexcept {
      return refused != 0 || leakedSubscriptions != 0 || leakedEntities != 0
          || discardedCommands != 0;
    }
  };

  class SceneManager {
  public:
    explicit SceneManager(Memory::IMemoryResource* resource);
    ~SceneManager();

    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;
    SceneManager(SceneManager&&) = delete;
    SceneManager& operator=(SceneManager&&) = delete;

    /// @return 要求を積めたら true。**確保に失敗したら false** (N-2)
    [[nodiscard]] bool PushScene(std::unique_ptr<IScene> scene);
    [[nodiscard]] bool PopScene();
    [[nodiscard]] bool ChangeScene(std::unique_ptr<IScene> scene);

    /**
     * @brief 積まれた遷移要求を処理する
     *
     * @note  **`OnEnter` / `OnExit` の中で出された要求は次の呼び出しで
     *        処理される。** 1 回の呼び出しで遷移が連鎖し続けないようにする
     *        ため、処理する前に配列を入れ替える。1-8 までは保留配列を
     *        直接なぞっており、`OnEnter` が `PushScene` を呼ぶと
     *        再確保で参照が宙に浮いた
     */
    void ProcessPendingTransitions(GameContext& ctx);

    void Update(GameContext& ctx);
    void Render(GameContext& ctx);

    [[nodiscard]] bool HasActiveScene() const;
    [[nodiscard]] std::size_t Depth() const noexcept { return m_sceneStack.GetSize(); }

    /// 遷移の観測 (§4.12: `assert` では確かめられないので数を公開する)
    [[nodiscard]] const TransitionReport& Report() const noexcept { return m_report; }
    void ResetReport() noexcept { m_report = TransitionReport{}; }

  private:
    enum class TransitionType { Push, Pop, Change };

    struct TransitionRequest {
      TransitionType type;
      std::unique_ptr<IScene> scene;
    };

    void ApplyPush(GameContext& ctx, std::unique_ptr<IScene> scene, bool replaceTop);
    void ApplyPop(GameContext& ctx);
    /// `OnExit` の後始末と照合。**契約を守らなかったシーンを数える**
    void LeaveTop(GameContext& ctx);

    DynamicArray<std::unique_ptr<IScene>> m_sceneStack;
    DynamicArray<TransitionRequest>       m_pendingTransitions;
    /// 処理中の要求。**保留配列と入れ替えて使う**(確保を増やさない)
    DynamicArray<TransitionRequest>       m_processing;
    Memory::IMemoryResource*              m_resource;

    TransitionReport m_report;

    /// 今いるシーンに入る直前の控え (論点5)
    std::uint32_t m_serialAtEnter = 0;
    std::uint32_t m_aliveAtEnter  = 0;
  };

}
