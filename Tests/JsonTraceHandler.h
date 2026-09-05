#pragma once

/**
 * @file  JsonTraceHandler.h
 * @brief SAX イベント列を文字列へ記録するテスト用ハンドラ(要件 T-7)
 *
 * @details
 *  ## トレース表記
 *  | イベント | 表記 |
 *  |---|---|
 *  | `OnNull()`        | `n` |
 *  | `OnBool(true)`    | `t` |
 *  | `OnBool(false)`   | `f` |
 *  | `OnInt64(v)`      | `i(v)` |
 *  | `OnUInt64(v)`     | `u(v)` |
 *  | `OnDouble(v)`     | `d(v)` — `std::to_chars` の shortest 表現 |
 *  | `OnString(s,c)`   | `s(...)` / `s*(...)` |
 *  | `OnKey(s,c)`      | `k(...)` / `k*(...)` |
 *  | `OnObjectBegin()` | `{` |
 *  | `OnObjectEnd(n)`  | `}n` |
 *  | `OnArrayBegin()`  | `[` |
 *  | `OnArrayEnd(n)`   | `]n` |
 *
 *  `*` は `needsCopy == false`、すなわち**エスケープを含んでいたためアリーナ上に
 *  アンエスケープ結果が構築された**ことを表す。`*` が無ければ入力バッファを
 *  直接指すスライスである。文字列の中身はバイト列をそのまま出すのではなく、
 *  非印字バイトを `<XX>` の 16 進表記へ置き換えて記録する(期待値を ASCII で
 *  書けるようにするため)。
 */

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Core/StringView.h"

namespace GLFD::Test {

  /// SAX イベント列を記録し、任意の位置で中断させられるハンドラ
  class JsonTraceHandler final {
  public:
    /// スタック上に置かれる前提のサイズ。テストのイベント列には十分な余裕がある
    static constexpr size_t kCapacity = 16 * 1024;

    // --- JsonHandler の実装 ---

    bool OnNull()          { return Begin() && Put('n'); }
    bool OnBool(bool v)    { return Begin() && Put(v ? 't' : 'f'); }

    bool OnInt64(std::int64_t v) {
      return Begin() && Put('i') && Put('(') && Number(v) && Put(')');
    }
    bool OnUInt64(std::uint64_t v) {
      return Begin() && Put('u') && Put('(') && Number(v) && Put(')');
    }
    bool OnDouble(double v) {
      return Begin() && Put('d') && Put('(') && Number(v) && Put(')');
    }

    bool OnString(StringView s, bool needsCopy) { return Text('s', s, needsCopy); }
    bool OnKey(StringView s, bool needsCopy)    { return Text('k', s, needsCopy); }

    bool OnObjectBegin() { return Begin() && Put('{'); }
    bool OnArrayBegin()  { return Begin() && Put('['); }

    bool OnObjectEnd(std::uint32_t n) {
      return Begin() && Put('}') && Number(static_cast<std::uint64_t>(n));
    }
    bool OnArrayEnd(std::uint32_t n) {
      return Begin() && Put(']') && Number(static_cast<std::uint64_t>(n));
    }

    // --- テスト側の操作 ---

    /// count 件のイベントまでは成功し、その次のイベントで false を返す
    void AbortAfter(int count) noexcept { m_abortAfter = count; }

    /// 記録されたイベント列
    [[nodiscard]] StringView Trace() const noexcept { return StringView(m_buffer, m_size); }

    /// 呼び出されたイベント数(中断したイベントも 1 件として数える)
    [[nodiscard]] int EventCount() const noexcept { return m_events; }

    /// バッファが溢れたか(溢れた場合トレースは信用できない)
    [[nodiscard]] bool Overflowed() const noexcept { return m_overflow; }

    void Reset() noexcept {
      m_size       = 0;
      m_events     = 0;
      m_abortAfter = -1;
      m_overflow   = false;
    }

  private:
    /// イベント冒頭の共通処理。中断指定に達していたら false
    bool Begin() noexcept {
      ++m_events;
      return (m_abortAfter < 0) || (m_events <= m_abortAfter);
    }

    bool Put(char c) noexcept {
      if (m_size < kCapacity) {
        m_buffer[m_size++] = c;
      }
      else {
        m_overflow = true;
      }
      return true;
    }

    bool Append(const char* p, size_t n) noexcept {
      for (size_t i = 0; i < n; ++i) {
        (void)Put(p[i]);
      }
      return true;
    }

    template <class T>
    bool Number(T value) noexcept {
      char       scratch[64];
      const auto result = std::to_chars(scratch, scratch + sizeof(scratch), value);
      if (result.ec != std::errc{}) {
        return Append("<?>", 3);
      }
      return Append(scratch, static_cast<size_t>(result.ptr - scratch));
    }

    /// 文字列 / キーの共通処理。非印字バイトは <XX> へ落として記録する
    bool Text(char tag, StringView s, bool needsCopy) noexcept {
      if (!Begin()) {
        return false;
      }
      (void)Put(tag);
      if (!needsCopy) {
        (void)Put('*');           // アリーナ上にアンエスケープ済み
      }
      (void)Put('(');
      for (size_t i = 0; i < s.Size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20u && c < 0x7Fu && c != '(' && c != ')') {
          (void)Put(static_cast<char>(c));
        }
        else {
          static constexpr char kHex[] = "0123456789ABCDEF";
          (void)Put('<');
          (void)Put(kHex[(c >> 4) & 0x0Fu]);
          (void)Put(kHex[c & 0x0Fu]);
          (void)Put('>');
        }
      }
      (void)Put(')');
      return true;
    }

    char   m_buffer[kCapacity] = {};
    size_t m_size              = 0;
    int    m_events            = 0;
    int    m_abortAfter        = -1;
    bool   m_overflow          = false;
  };

}
