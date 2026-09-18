#pragma once

/**
 * @file  IScene.h
 * @brief シーンのインタフェースと**契約** (ECS 2-1 / 論点5)
 *
 * @details
 *  **新しいシーンを書く人が最初に読む場所である。** 1-8 まで、ここには
 *  4 つの純粋仮想関数だけが並んでいて契約が 1 行も書かれておらず、
 *  実際に両方のシーンが `OnExit` にログ 1 行しか書いていなかった。
 *
 *  # 契約
 *
 *  ## 一言でいうと
 *  **`OnExit` を抜けた時点で、`EventBus` と `Registry` を `OnEnter` の
 *  直前の状態に戻すこと。**
 *
 *  ## `OnEnter`
 *   - 下にシーンが無ければ、**空の世界を前提にしてよい**。`SceneManager` が
 *     `AliveCount() == 0` と購読 0 件を保証する
 *   - 遷移を要求してよい。ただし**処理されるのは次のフレーム**である
 *     (`SceneManager::ProcessPendingTransitions` の @note)
 *
 *  ## `OnExit` がやること
 *   1. **自分が登録した購読をすべて外す。** `Subscribe` が返した
 *      `SubscriptionId` を控えておき、`ctx.eventBus->Unsubscribe(id)` を呼ぶ
 *   2. **自分が作ったエンティティをすべて破棄する。** 一番下のシーンなら
 *      `ctx.registry->DestroyAll()` で足りる
 *   3. 即時 API を使ってよい。**コマンドバッファには積まない**
 *      (遷移時に捨てられる。論点3)
 *
 *  ## 守らなかったらどうなるか
 *  `SceneManager` が `OnEnter` の直前に控えた値と突き合わせ、
 *  `TransitionReport` に件数を残す。**購読の外し忘れは安全網として外す** —
 *  報告だけにすると、次の配信が解放済みのシーンを呼ぶためである
 *  (use-after-free)。エンティティの残りは、一番下のシーンなら
 *  `DestroyAll()` する。**上に積んだシーンでは報告だけ**で、どれが
 *  自分のものか区別できないため直さない。
 *
 *  @note **エンティティの照合は件数だけである。** 上に積んだシーンが N 体
 *        作り、下のシーンの N 体を消すと、件数は一致して見逃す。閉じるには
 *        持ち主の記録が要り、深さ 1 でしか使っていない現状では作っていない
 *
 *  ## `OnUpdate` / `OnRender`
 *   - **遷移したフレームでは、新しいシーンの `OnRender` が
 *     その `OnUpdate` より先に 1 回走る**(`Engine::Update` の末尾で遷移し、
 *     その後 `Engine::Render` が呼ばれるため)。`OnRender` は `OnUpdate` が
 *     一度も走っていない状態で描けること
 */

namespace GLFD { struct GameContext; }

namespace GLFD::Scene {
  class IScene {
  public:
    virtual ~IScene() = default;

    virtual void OnEnter(GameContext& ctx) = 0;
    virtual void OnUpdate(GameContext& ctx) = 0;
    virtual void OnRender(GameContext& ctx) = 0;

    /// @see ファイル冒頭の契約。**購読を外し、自分のエンティティを破棄する**
    virtual void OnExit(GameContext& ctx) = 0;
  };
}
