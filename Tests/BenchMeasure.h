#pragma once
/**
 * @file  BenchMeasure.h
 * @brief 計測ツールの共通部品(ECS 2-5 の基準線)
 *
 * @details
 *  **エンジン(Source/)には手を入れない。** ここにあるのは計測の道具だけ。
 *
 *  ## 平均使用コア数は計測区間だけで取る
 *  2-4 ではプロセス全体の CPU 時間 / 実時間を外から測っていた。それだと設定の読み込み・
 *  生成・助走まで含み、計測したフレームのコア数にならない。ここではベンチの中で
 *  計測区間の始めと終わりに取り、差の比を出す。
 *
 *   - `coresByTimes`   : `GetProcessTimes`(カーネル + ユーザー)/ 実時間。
 *                        **タイマ割り込み単位で数えている可能性がある**(区間が短いと丸めが効く)
 *   - `coresByCycles`  : `QueryProcessCycleTime` / TSC の進み。
 *                        **近似である。** 性能コアと効率コアで周波数が違い、ターボで変動する
 *  2 つが食い違ったら、どちらかを信じるのではなく**区間を長くして**取り直す(2-5 の指示)。
 *
 *  ## 機械で読める行
 *  人が読む表とは別に、`@` で始まる行を出す。`run_baseline.ps1` はこの行だけを読む。
 *  **最後に `@end` を出す。** `@end` が無い出力は、途中で終わったものとして失敗扱いになる。
 *
 *  ## 失敗を起こす引数(スクリプトの歯の確認に使う)
 *   `--fail=crash`    計測中に std::abort() で落ちる
 *   `--fail=hang`     計測中に止まる(スクリプトの時間切れが拾う)
 *   `--fail=noend`    計測中に終了コード 0 のまま抜ける(`@end` が出ない)
 *   `--fail=exitcode` 最後まで出力したうえで終了コード 7 で終わる
 *   `--fail-at=N`     計測の何フレーム目で起こすか(既定 5)
 */

#include <cstdint>

namespace GLFD::Bench {

  /// ある瞬間のプロセス全体の CPU の値
  struct CpuSample {
    double        wallSec       = 0.0;   ///< steady_clock
    std::uint64_t processTime   = 0;     ///< GetProcessTimes のカーネル + ユーザー(100ns 単位)
    std::uint64_t processCycles = 0;     ///< QueryProcessCycleTime
    std::uint64_t tsc           = 0;     ///< __rdtsc
  };

  /// 2 つの CpuSample の差
  struct CpuWindow {
    double wallSec       = 0.0;
    double cpuSec        = 0.0;
    double coresByTimes  = 0.0;
    double coresByCycles = 0.0;
  };

  [[nodiscard]] CpuSample SampleCpu();
  [[nodiscard]] CpuWindow Diff(const CpuSample& begin, const CpuSample& end);

  /// 性能コアと効率コアの内訳(EfficiencyClass の最大を性能コアとみなす)
  struct Topology {
    unsigned hardwareConcurrency = 0;
    unsigned physicalCores       = 0;
    unsigned logicalProcessors   = 0;
    unsigned performanceCores    = 0;
    unsigned performanceLogical  = 0;
    unsigned efficiencyCores     = 0;
    unsigned efficiencyLogical   = 0;
  };

  [[nodiscard]] Topology ReadTopology();

  enum class Fail { None, Crash, Hang, NoEnd, ExitCode };

  /// `--` で始まる引数を抜き取り、残りを位置引数として並べ直す
  struct Args {
    int         positionalCount = 0;
    const char* positional[8]   = {};
    Fail        fail            = Fail::None;
    int         failAt          = 5;
    const char* failName        = "none";
    bool        bad             = false;   ///< 知らない `--` 引数があった
  };

  [[nodiscard]] Args ParseArgs(int argc, char** argv);

  /// 位置引数 i を整数で返す。無ければ fallback
  [[nodiscard]] int IntArg(const Args& args, int i, int fallback);

  /// 計測の m フレーム目で、指定された失敗を起こす。Crash / Hang / NoEnd は戻らない
  void MaybeFail(const Args& args, int measuredFrame);

  /// 終了コード(ExitCode の失敗を指定されていれば 7)
  [[nodiscard]] int ExitCode(const Args& args);

  /// 共通の @meta 行(トポロジと失敗の指定)
  void PrintCommonMeta(const Args& args);

  /// `@cpu` 行
  void PrintCpu(const CpuWindow& window);

  /// `@row` 行。名前に空白を含めないこと
  void PrintRow(const char* kind, const char* name, double median, double mean,
                double min, double p95);

  /**
   * @brief 計測した各フレームの時間を、フレームの順に 1 行で出す(`@framevals n=N v1 v2 ...`)
   * @details **p99 と最大は、1 回ごとではなく全回のフレームをまとめてから取る**(ECS 2-7)。
   *  1 回 270 フレームの p99 は下から約 3 番目の値で、最大は 1 フレームだけの値になり、
   *  最小値と同じく標本数で動く(開発手法 §5.3)。**並べ替える前に呼ぶこと**(Summarize は
   *  配列をその場で並べ替える)
   */
  void PrintFrameValues(const double* values, int count);

  /// 最後の行。これが無い出力は失敗として扱われる
  void PrintEnd();

}
