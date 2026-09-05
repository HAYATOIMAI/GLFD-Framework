#pragma once

/**
 * @file  TestHarness.h
 * @brief テスト用の最小ハーネス
 *
 * @details
 *  リポジトリにテストフレームワークが存在しないため、外部依存ゼロで実装している。
 *  例外を使わないので、失敗しても打ち切らずに記録して続行する
 *  (1 回の実行でできるだけ多くの失敗を洗い出すため)。
 */

#include <cstdio>

namespace GLFD::Test {

  inline int g_checkCount   = 0;
  inline int g_failureCount = 0;
  inline int g_caseCount    = 0;

  inline void BeginSuite(const char* name) {
    std::printf("=== %s tests ===\n", name);
#ifdef _DEBUG
    std::printf("(configuration: Debug)\n");
#else
    std::printf("(configuration: Release / NDEBUG)\n");
#endif
  }

  inline void BeginCase(const char* name) {
    ++g_caseCount;
    std::printf("  [%d] %s\n", g_caseCount, name);
    // assert が発火すると abort() で stdout のバッファが失われ、
    // どのケースで落ちたか分からなくなる。ケースの区切りで必ず流す
    std::fflush(stdout);
  }

  inline void ReportFailure(const char* file, int line, const char* expr) {
    std::printf("    [FAIL] %s(%d): %s\n", file, line, expr);
    std::fflush(stdout);
    ++g_failureCount;
  }

  /// 結果を出力し、プロセスの終了コードを返す
  [[nodiscard]] inline int Summarize() {
    std::printf("-----------------------\n");
    std::printf("cases: %d  checks: %d  failures: %d\n",
                g_caseCount, g_checkCount, g_failureCount);
    std::printf("%s\n", (g_failureCount == 0) ? "RESULT: PASSED" : "RESULT: FAILED");
    return (g_failureCount == 0) ? 0 : 1;
  }

}

#define CHECK(expr)                                                    \
  do {                                                                 \
    ++::GLFD::Test::g_checkCount;                                      \
    if (!(expr)) {                                                     \
      ::GLFD::Test::ReportFailure(__FILE__, __LINE__, #expr);          \
    }                                                                  \
  } while (0)

/**
 * CHECK と同じ検証を行うが、**チェック数を数えない**。
 * 呼び出し側で ++g_checkCount して「ループ全体で 1 件」と数えるのが定型。
 *
 * @warning **使い分けの方針: 期待値が個別に意味を持つ検証は CHECK、
 *          同一の不変条件を大量回数まわす検証のみ CHECK_QUIET。**
 *          チェック数はこれまで「テストを壊していないこと」の指標として機能してきた
 *          (MockMemoryResource.h を共有ヘッダへ切り出した際は、JsonArenaTests が
 *          Debug 844 / Release 847 のまま不変であることが正当性の根拠だった)。
 *          「件数が多くて邪魔だから」という理由で CHECK_QUIET に倒すと、
 *          この指標の感度が落ちる。
 */
#define CHECK_QUIET(expr)                                              \
  do {                                                                 \
    if (!(expr)) {                                                     \
      ::GLFD::Test::ReportFailure(__FILE__, __LINE__, #expr);          \
    }                                                                  \
  } while (0)
