#pragma once
/**
 * @file  StartupFailureLog.h
 * @brief 起動の失敗の理由を Game.log に残す行 (ECS 2-11 問題1)
 *
 * @details
 *  2-10 までは、起動の失敗の理由(シェーダーのコンパイルエラーの本文など)が `std::cerr` にしか出ず、
 *  `Game.log` には `DX11 Init Failed!` の 1 行しか残らなかった。2-9 のボックスは「Game.log を送って
 *  ください」と書くのに、その先に理由が無かった。理由はすべてここの行で `Logger` に書く。
 *
 *  - `HRESULT` は 2-9 と同じ「名前 (0x…)」の形 (`DescribeHResult`)
 *  - シェーダーのコンパイルエラーの本文は複数行になる。1 つの項目に改行を入れると、行単位で読む
 *    テストと道具 (`console_close_check` / `render_fault_check`) が崩れるので、**1 行ずつに分けて書く**。
 *    最初の行に行数を書き、各行に `shader: ` の頭を付ける
 *  - 本文の行は `LogFmt` の 1,024 バイトの欄を通さず、`Log` の経路 (`std::string`) で書く (切らない)
 *
 *  どれも起動の途中で 1 回だけ呼ぶ。毎フレームの経路には乗らない。
 */
#include <cstddef>
#include <string>

#include "../Core/Logger.h"
#include "RenderHealthLog.h"

namespace GLFD::Graphics {

  /// 起動の途中の呼び出しが失敗した: "<area>: <what> failed: E_OUTOFMEMORY (0x8007000E)"
  inline void ReportStartupCall(const char* area, const char* what, HRESULT hr) {
    char described[64];
    DescribeHResult(described, hr);
    LOG_ERROR("%s: %s failed: %s", area, what, described);
  }

  /// `text` の各行 (改行で分け、行末の CR を除き、空の行は飛ばす) を `fn(const char* begin, size_t length)` に渡す。
  /// `\0` が来たらそこで終わる (`ID3DBlob` の本文は終端を含むことがある)
  template <typename Fn>
  inline int ForEachOutputLine(const char* text, std::size_t size, Fn&& fn) {
    if (text == nullptr) { return 0; }
    int count = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= size; ++i) {
      const bool end = (i == size) || text[i] == '\0';
      if (end || text[i] == '\n') {
        std::size_t length = i - start;
        if (length > 0 && text[start + length - 1] == '\r') { --length; }
        if (length > 0) {
          fn(text + start, length);
          ++count;
        }
        start = i + 1;
        if (end) { break; }
      }
    }
    return count;
  }

  /**
   * @brief シェーダーのコンパイルの失敗を書く。最初の 1 行に何が失敗したかと行数、続けて本文を 1 行ずつ
   * @param text  コンパイラの出力 (`ID3DBlob` の本文)。無ければ nullptr
   * @return 書いた本文の行数
   */
  inline int ReportShaderCompileError(const char* entry, const char* target, HRESULT hr,
                                      const char* text, std::size_t size) {
    char described[64];
    DescribeHResult(described, hr);
    const int lines = ForEachOutputLine(text, size, [](const char*, std::size_t) {});
    if (lines == 0) {
      LOG_ERROR("shader: could not compile %s (%s): %s. the compiler gave no output", entry, target, described);
      return 0;
    }
    LOG_ERROR("shader: could not compile %s (%s): %s. %d line(s) of compiler output follow",
              entry, target, described, lines);
    return ForEachOutputLine(text, size, [](const char* begin, std::size_t length) {
      std::string line("shader: ");
      line.append(begin, length);
      Core::Logger::Get().Log(Core::LogLevel::Error, line);
    });
  }

}
