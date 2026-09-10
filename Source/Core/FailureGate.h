#pragma once

#include <cstdint>

namespace GLFD::Core {

  /**
   * @brief 同じ失敗が続く間は黙り、**状態が変わったときだけ**通す門 (ECS 1-6 / R-28)
   *
   * @details
   *  毎フレーム起き得る失敗(フレームメモリの枯渇など)を素朴にログへ出すと
   *  **60 行/秒**になる。かといって黙ると R-28(部分的な失敗を静かに通さない)に
   *  反する。
   *
   *  ## なぜ「初回だけ」ではないか
   *  初回だけ出す形は**復帰を落とす**。ログを見た人は「まだ失敗しているのか、
   *  直ったのか」を判断できない。「N 回ごと」はゆっくり溢れるうえ、
   *  N の根拠が無い。
   *
   *  **状態の変わり目だけを出す。**
   *
   *  ```
   *  [ERROR] RenderSystem: could not size the vertex buffer (20000 verts)
   *     ... 300 フレーム沈黙 ...
   *  [INFO]  RenderSystem: recovered after 300 frame(s) (312 failures total)
   *  ```
   *
   *  **沈黙が「状態が変わっていない」を意味する。** 1-4 で `applied=0` を 1 行だけ
   *  出して以降黙らせた設計(沈黙を証拠にする)の一般化である。
   *  復帰の行に**続いた長さ**を載せるので、沈黙の中身も後から分かる。
   *
   *  @note **状態を持つので、誰かが所有しなければならない。** 関数ローカルの
   *        `static` にすると N-3(グローバル可変状態)に当たる。
   *        `RenderSystem` のような `static` クラスからは使えないので、
   *        **シーンが持ち、システムは結果を返すだけ**という分担にしてある。
   */
  class FailureGate {
  public:
    enum class Change : std::uint8_t {
      None,        ///< 状態が変わっていない。**何も出さないこと**
      Started,     ///< 失敗し始めた
      Recovered,   ///< 直った
    };

    /**
     * @brief このフレームの結果を渡す
     * @return 状態が変わったときだけ `Started` / `Recovered`
     */
    [[nodiscard]] Change Observe(bool failing) noexcept {
      if (failing) {
        Bump(m_totalFailures);
        if (m_streak == 0u) {
          m_streak = 1u;
          return Change::Started;
        }
        Bump(m_streak);
        return Change::None;
      }

      if (m_streak != 0u) {
        m_lastStreak = m_streak;
        m_streak     = 0u;
        return Change::Recovered;
      }
      return Change::None;
    }

    /// いま何フレーム続いているか(失敗中でなければ 0)
    [[nodiscard]] std::uint32_t StreakLength() const noexcept { return m_streak; }

    /// 直前に何フレーム続いたか。**`Recovered` の行に載せる値**
    [[nodiscard]] std::uint32_t LastStreakLength() const noexcept { return m_lastStreak; }

    [[nodiscard]] std::uint32_t TotalFailures() const noexcept { return m_totalFailures; }

    [[nodiscard]] bool IsFailing() const noexcept { return m_streak != 0u; }

  private:
    /**
     * @note **飽和させる。** 一周して 0 に戻ると `Observe` が「失敗し始めた」と
     *       誤って言う。60fps で 42 億フレームは 2 年以上先だが、
     *       **数を数えるだけの型が嘘をつく形にしておく理由が無い**
     */
    static void Bump(std::uint32_t& counter) noexcept {
      if (counter != 0xFFFFFFFFu) { ++counter; }
    }

    std::uint32_t m_streak        = 0;
    std::uint32_t m_lastStreak    = 0;
    std::uint32_t m_totalFailures = 0;
  };

}
