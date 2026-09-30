#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace GLFD::Core {

  /**
   * @brief 1 つの段が何をしたか (ECS 1-6 / R-26 / R-28)
   *
   * @note **`Skipped` と `Failed` を分ける。** 前者は「前提条件が揃わないので
   *       今回は走らない」で、後者は「走ろうとして駄目だった」である。
   *       グリッドが作れなかったフレームでは `GridBuild` が `Failed`、
   *       それに依存する `Boid` と `Collision` が `Skipped` になる。
   *       **1 つの原因と 2 つの結果**が区別できないと、報告を読んだ人が
   *       「3 つ壊れた」と誤解する。
   */
  enum class StageResult : std::uint8_t {
    Ran,       ///< 走った
    Skipped,   ///< 前提条件が揃わないので走らせなかった
    Failed,    ///< 走ったが目的を果たせなかった
  };

  /**
   * @brief 実行順序の 1 行 (R-26)
   *
   * @details
   *  **`run` は捕捉なしラムダから作る。** 捕捉しなければ関数ポインタへ落ちるので、
   *  `constexpr` の配列に並べられる。呼び出し側のシグネチャが揃っていなくても、
   *  ラムダの中で開いて渡せる(GLFD のシステムは `Update(ctx)` /
   *  `Update(ctx, maxSpeed)` / 7 引数 / シーンのメンバ関数と 4 種類に割れている)。
   *
   *  **`Host` と `Ctx` の両方をテンプレートにしてある。** テストが実物の
   *  `GameContext` を持ち込まずに順序を検証できるようにするため
   *  (`GameContext.h` は `d3d11.h` まで引き込む)。
   *
   *  @note **前提条件はラムダの中に書く。** 「依存を宣言して順序を自動で決める」
   *        ところまでは行かない(過剰)。**順序は人が書き、前提条件は同じ場所に
   *        見えている**、という状態を要件とする (R-26)。
   */
  template <class Host, class Ctx>
  struct SystemStage {
    /// 報告に出る名前。**`StageReport` が読む**(飾りではない)
    const char* name;
    StageResult (*run)(Host&, Ctx&);
  };

  /**
   * @brief 1 フレームで各段がどうなったか (R-28)
   *
   * @details
   *  **確保しない。** `ApplyReport`(1-4 / R-38)と同じ理由である。この報告は
   *  確保に失敗したフレームでこそ書かれるので、書くために確保してはならない。
   *
   *  出力はしない。**`Logger` を呼ぶのは上層**(`Game/EcsDiagnosticsLog.h`)で、
   *  ここは記録するだけ。JSON の `ArchiveContext` と同じ分担。
   */
  class StageReport {
  public:
    /**
     * @brief 記録できる段数の上限
     *
     * @note **本当の防御は `RunStages` の `static_assert` である**(表の大きさは
     *       コンパイル時に分かる)。この上限と `Truncated()` が残っているのは、
     *       `Record` が公開されていてテストが直接呼べるため。
     *       **実行順序の表がここを越えたらビルドが通らない。**
     */
    static constexpr std::uint32_t kMaxStages = 16;

    void Reset() noexcept {
      m_count     = 0;
      m_truncated = false;
    }

    void Record(const char* name, StageResult result) noexcept {
      if (m_count >= kMaxStages) {
        m_truncated = true;
        return;
      }
      m_names[m_count]   = name;
      m_results[m_count] = result;
      ++m_count;
    }

    [[nodiscard]] std::uint32_t StageCount() const noexcept { return m_count; }

    [[nodiscard]] const char* NameAt(std::uint32_t i) const noexcept {
      assert(i < m_count && "StageReport::NameAt: index out of range");
      return m_names[i];
    }

    [[nodiscard]] StageResult ResultAt(std::uint32_t i) const noexcept {
      assert(i < m_count && "StageReport::ResultAt: index out of range");
      return m_results[i];
    }

    [[nodiscard]] std::uint32_t CountOf(StageResult result) const noexcept {
      std::uint32_t n = 0;
      for (std::uint32_t i = 0; i < m_count; ++i) {
        if (m_results[i] == result) { ++n; }
      }
      return n;
    }

    /// 全段が走ったか。**平常時はこれが true で、報告は 1 行も出ない**
    [[nodiscard]] bool AllRan() const noexcept {
      return CountOf(StageResult::Ran) == m_count;
    }

    [[nodiscard]] bool Truncated() const noexcept { return m_truncated; }

  private:
    const char*   m_names[kMaxStages]{};
    StageResult    m_results[kMaxStages]{};
    std::uint32_t m_count     = 0;
    bool          m_truncated = false;
  };

  /**
   * @brief 表のとおりに順に走らせる (R-26)
   *
   * @details
   *  **止めない。** ある段が `Failed` を返しても残りを走らせる。
   *  1-4 の R-37 と同じ理由で、途中で止めるとフレームの状態が「どこで失敗したか」に
   *  依存する。**依存している段は自分で `Skipped` を返す**(前提条件は
   *  表に書いてある)ので、走らせて困るものは走らない。
   *
   *  @note **計測点は置いていない。** 将来スレッド別のタイムラインが要るように
   *        なったら、**このループに 3 行入れれば全段が一度に計測対象になる**。
   *        呼ぶ人のいないフックを今から置かない (R-27) — それは 1-6 で消した
   *        `PROFILE_SCOPE` と同じ形である。
   */
  template <class Host, class Ctx, std::size_t N>
  void RunStages(const SystemStage<Host, Ctx> (&stages)[N], Host& host, Ctx& ctx,
                StageReport& report) noexcept {
    static_assert(N <= StageReport::kMaxStages,
                  "the execution order table has more stages than StageReport can record. "
                  "Raise StageReport::kMaxStages (it is a fixed-size array on purpose: the "
                  "report is written on frames where allocation failed, so it must not "
                  "allocate).");

    report.Reset();
    for (std::size_t i = 0; i < N; ++i) {
      report.Record(stages[i].name, stages[i].run(host, ctx));
    }
  }

}
