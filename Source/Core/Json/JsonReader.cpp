/**
 * @file  JsonReader.cpp
 * @brief JsonReader のトークン単位スキャナ
 *
 * @details
 *  テンプレートに依存しない処理はすべてここに置き、ヘッダ側の Parse は
 *  構文状態の遷移とハンドラ呼び出しだけを担う。
 */

#include "Core/Json/JsonReader.h"

#include <charconv>
#include <cstring>
#include <limits>

namespace GLFD::Json::Detail {

  namespace {

    // -------------------------------------------------------------------------
    // 文字種
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr bool IsDigit(char c) noexcept {
      return c >= '0' && c <= '9';
    }

    [[nodiscard]] constexpr bool IsHexDigit(char c) noexcept {
      return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    [[nodiscard]] constexpr std::uint32_t HexValue(char c) noexcept {
      if (c >= '0' && c <= '9') { return static_cast<std::uint32_t>(c - '0'); }
      if (c >= 'a' && c <= 'f') { return static_cast<std::uint32_t>(c - 'a') + 10u; }
      return static_cast<std::uint32_t>(c - 'A') + 10u;
    }

    /// 数値トークンの直後に来てはならない文字 (R1-16)
    [[nodiscard]] constexpr bool IsNumberTrailingGarbage(char c) noexcept {
      return IsDigit(c)
          || (c >= 'a' && c <= 'z')
          || (c >= 'A' && c <= 'Z')
          || c == '.'
          || c == '_';
    }

    /**
     * @brief 文字列内容の UTF-8 シーケンスを検証する
     * @param p   先行バイトの位置
     * @param end 入力の終端
     * @return 妥当なら消費バイト数 (1..4)。不正なら 0
     * @note  過長符号化・UTF-8 で符号化されたサロゲート (ED A0..BF)・
     *        U+10FFFF 超・途中で切れた列をすべて弾く
     */
    [[nodiscard]] std::uint32_t ValidateUtf8Sequence(const char* p, const char* end) noexcept {
      const auto lead = static_cast<unsigned char>(*p);

      if (lead < 0x80u) {
        return 1;
      }

      std::uint32_t      length = 0;
      unsigned char      lowSecond  = 0x80u;
      unsigned char      highSecond = 0xBFu;

      if (lead >= 0xC2u && lead <= 0xDFu) {
        length = 2;
      }
      else if (lead == 0xE0u) {
        length = 3; lowSecond = 0xA0u;                       // 過長符号化を弾く
      }
      else if (lead >= 0xE1u && lead <= 0xECu) {
        length = 3;
      }
      else if (lead == 0xEDu) {
        length = 3; highSecond = 0x9Fu;                      // サロゲート領域を弾く
      }
      else if (lead >= 0xEEu && lead <= 0xEFu) {
        length = 3;
      }
      else if (lead == 0xF0u) {
        length = 4; lowSecond = 0x90u;                       // 過長符号化を弾く
      }
      else if (lead >= 0xF1u && lead <= 0xF3u) {
        length = 4;
      }
      else if (lead == 0xF4u) {
        length = 4; highSecond = 0x8Fu;                      // U+10FFFF 超を弾く
      }
      else {
        return 0;   // 0x80..0xC1 (継続バイト単独・過長) と 0xF5..0xFF
      }

      if (static_cast<size_t>(end - p) < static_cast<size_t>(length)) {
        return 0;   // 途中で切れている
      }

      const auto second = static_cast<unsigned char>(p[1]);
      if (second < lowSecond || second > highSecond) {
        return 0;
      }
      for (std::uint32_t i = 2; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(p[i]);
        if ((continuation & 0xC0u) != 0x80u) {
          return 0;
        }
      }
      return length;
    }

    /// コードポイントを UTF-8 にしたときのバイト数
    [[nodiscard]] constexpr std::uint32_t Utf8Length(std::uint32_t codepoint) noexcept {
      if (codepoint < 0x80u)    { return 1; }
      if (codepoint < 0x800u)   { return 2; }
      if (codepoint < 0x10000u) { return 3; }
      return 4;
    }

    /// コードポイントを UTF-8 で書き出し、書いたバイト数を返す
    std::uint32_t EncodeUtf8(std::uint32_t codepoint, char* out) noexcept {
      if (codepoint < 0x80u) {
        out[0] = static_cast<char>(codepoint);
        return 1;
      }
      if (codepoint < 0x800u) {
        out[0] = static_cast<char>(0xC0u | (codepoint >> 6));
        out[1] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 2;
      }
      if (codepoint < 0x10000u) {
        out[0] = static_cast<char>(0xE0u | (codepoint >> 12));
        out[1] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 3;
      }
      out[0] = static_cast<char>(0xF0u | (codepoint >> 18));
      out[1] = static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
      out[2] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
      out[3] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
      return 4;
    }

    /// `\uXXXX` の 4 桁を読む。p は `u` の次を指していること
    [[nodiscard]] bool ReadHex4(const char* p, const char* end, std::uint32_t& out) noexcept {
      if (static_cast<size_t>(end - p) < 4) {
        return false;
      }
      std::uint32_t value = 0;
      for (int i = 0; i < 4; ++i) {
        if (!IsHexDigit(p[i])) {
          return false;
        }
        value = (value << 4) | HexValue(p[i]);
      }
      out = value;
      return true;
    }

    [[nodiscard]] constexpr bool IsHighSurrogate(std::uint32_t c) noexcept {
      return c >= 0xD800u && c <= 0xDBFFu;
    }

    [[nodiscard]] constexpr bool IsLowSurrogate(std::uint32_t c) noexcept {
      return c >= 0xDC00u && c <= 0xDFFFu;
    }

    /// 入力の残りが keyword と完全一致するか
    [[nodiscard]] bool MatchKeyword(const ScanState& s, const char* keyword,
                                    size_t length) noexcept {
      if (static_cast<size_t>(s.end - s.cur) < length) {
        return false;
      }
      return std::memcmp(s.cur, keyword, length) == 0;
    }

    /// 指数の累算は ±10^9 で飽和させる (R1-17)
    constexpr std::int64_t kExponentLimit = 1000000000;

  }

  // ---------------------------------------------------------------------------
  // BOM (R1-4)
  // ---------------------------------------------------------------------------

  ParseError SkipByteOrderMark(ScanState& s) noexcept {
    const size_t available = static_cast<size_t>(s.end - s.cur);

    // UTF-8 BOM は読み飛ばす
    if (available >= 3
        && static_cast<unsigned char>(s.cur[0]) == 0xEFu
        && static_cast<unsigned char>(s.cur[1]) == 0xBBu
        && static_cast<unsigned char>(s.cur[2]) == 0xBFu) {
      s.cur += 3;
      return ParseError{};
    }

    // UTF-32 / UTF-16 の BOM は非対応。ここで明示的に落とさないと
    // 「0 バイト目が値として不正」という分かりにくい診断になる。
    // 文字列内容の UTF-8 不正 (StringInvalidUtf8) とは原因も対処も異なるため、
    // 入力全体のエンコーディング違いを表す EncodingNotSupported を返す
    if (available >= 4) {
      const auto b0 = static_cast<unsigned char>(s.cur[0]);
      const auto b1 = static_cast<unsigned char>(s.cur[1]);
      const auto b2 = static_cast<unsigned char>(s.cur[2]);
      const auto b3 = static_cast<unsigned char>(s.cur[3]);
      const bool utf32Le = (b0 == 0xFFu && b1 == 0xFEu && b2 == 0x00u && b3 == 0x00u);
      const bool utf32Be = (b0 == 0x00u && b1 == 0x00u && b2 == 0xFEu && b3 == 0xFFu);
      if (utf32Le || utf32Be) {
        return ParseError{ ErrorCode::EncodingNotSupported, 0 };
      }
    }
    if (available >= 2) {
      const auto b0 = static_cast<unsigned char>(s.cur[0]);
      const auto b1 = static_cast<unsigned char>(s.cur[1]);
      if ((b0 == 0xFFu && b1 == 0xFEu) || (b0 == 0xFEu && b1 == 0xFFu)) {
        return ParseError{ ErrorCode::EncodingNotSupported, 0 };
      }
    }

    return ParseError{};
  }

  // ---------------------------------------------------------------------------
  // 空白とコメント (§3.6)
  // ---------------------------------------------------------------------------

  ParseError SkipTrivia(ScanState& s) noexcept {
    for (;;) {
      while (s.cur < s.end) {
        const char c = *s.cur;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          ++s.cur;
        }
        else {
          break;
        }
      }

      if (s.cur >= s.end || *s.cur != '/') {
        return ParseError{};
      }

      // `/` の次を見て、実際にコメントの開始かどうかを確かめる
      if (static_cast<size_t>(s.end - s.cur) < 2) {
        return ParseError{};
      }
      const char next = s.cur[1];
      if (next != '/' && next != '*') {
        // `/` 単独はコメント開始ではないため、AllowComments の有無に関わらず構文エラー。
        // CommentNotAllowed は `//` または `/*` が実際に出現したときのみ返す
        return ParseError{};
      }

      const size_t commentOffset = OffsetOf(s, s.cur);
      if (!HasFlag(s.flags, ParseFlags::AllowComments)) {
        return ParseError{ ErrorCode::CommentNotAllowed, commentOffset };
      }

      if (next == '/') {
        // 行コメント。ComputeLocation と同じく `\n` と単独 `\r` の双方を行末とみなす
        s.cur += 2;
        while (s.cur < s.end && *s.cur != '\n' && *s.cur != '\r') {
          ++s.cur;
        }
      }
      else {
        // ブロックコメント。**ネストしない**(JSONC / JSON5 系の一般的な挙動)。
        // `/* /* */ */` は最初の `*/` で閉じ、残りは構文エラーになる
        s.cur += 2;
        bool closed = false;
        while (s.cur + 1 < s.end) {
          if (s.cur[0] == '*' && s.cur[1] == '/') {
            s.cur += 2;
            closed = true;
            break;
          }
          ++s.cur;
        }
        if (!closed) {
          s.cur = s.end;
          return ParseError{ ErrorCode::CommentUnterminated, commentOffset };
        }
      }
    }
  }

  // ---------------------------------------------------------------------------
  // 文字列 (R1-5 / R1-6)
  // ---------------------------------------------------------------------------

  ParseError ScanString(ScanState& s, StringView& text, bool& needsCopy) noexcept {
    const size_t openOffset = OffsetOf(s, s.cur);
    ++s.cur;                                  // 開き `"`
    const char* const contentBegin = s.cur;

    // --- パス 1: 検証しながらアンエスケープ後の長さを測る ---
    // アリーナは個別解放できないので、伸長方式ではなく実寸を先に求める。
    // 不正な文字列に対しては 1 バイトも確保しない副次効果がある
    size_t      decodedLength = 0;
    bool        hasEscape     = false;
    const char* p             = s.cur;

    for (;;) {
      if (p >= s.end) {
        s.cur = s.end;
        return ParseError{ ErrorCode::StringUnterminated, openOffset };
      }

      const auto c = static_cast<unsigned char>(*p);

      if (c == '"') {
        break;
      }

      if (c < 0x20u) {
        s.cur = p;
        return ParseError{ ErrorCode::StringControlCharUnescaped, OffsetOf(s, p) };
      }

      if (c == '\\') {
        hasEscape = true;
        const size_t escapeOffset = OffsetOf(s, p);
        if (p + 1 >= s.end) {
          s.cur = s.end;
          return ParseError{ ErrorCode::StringUnterminated, openOffset };
        }

        const char escape = p[1];
        switch (escape) {
        case '"': case '\\': case '/':
        case 'b': case 'f': case 'n': case 'r': case 't':
          decodedLength += 1;
          p += 2;
          continue;

        case 'u': {
          std::uint32_t codepoint = 0;
          if (!ReadHex4(p + 2, s.end, codepoint)) {
            s.cur = p;
            return ParseError{ ErrorCode::StringUnicodeEscapeInvalid, escapeOffset };
          }
          p += 6;

          if (IsLowSurrogate(codepoint)) {
            // 下位サロゲートが単独で現れた
            s.cur = p;
            return ParseError{ ErrorCode::StringUnicodeSurrogateInvalid, escapeOffset };
          }
          if (IsHighSurrogate(codepoint)) {
            std::uint32_t low = 0;
            if (p + 1 >= s.end || p[0] != '\\' || p[1] != 'u'
                || !ReadHex4(p + 2, s.end, low) || !IsLowSurrogate(low)) {
              s.cur = p;
              return ParseError{ ErrorCode::StringUnicodeSurrogateInvalid, escapeOffset };
            }
            p += 6;
            decodedLength += 4;   // 合成後は必ず U+10000 以上 = 4 バイト
            continue;
          }

          decodedLength += Utf8Length(codepoint);
          continue;
        }

        default:
          s.cur = p;
          return ParseError{ ErrorCode::StringEscapeInvalid, escapeOffset };
        }
      }

      if (c < 0x80u) {
        decodedLength += 1;
        ++p;
        continue;
      }

      const std::uint32_t sequenceLength = ValidateUtf8Sequence(p, s.end);
      if (sequenceLength == 0) {
        s.cur = p;
        return ParseError{ ErrorCode::StringInvalidUtf8, OffsetOf(s, p) };
      }
      decodedLength += sequenceLength;
      p += sequenceLength;
    }

    const char* const contentEnd = p;
    s.cur = p + 1;                            // 閉じ `"` を消費する

    // --- エスケープが無ければ入力バッファをそのまま指す (needsCopy = true) ---
    if (!hasEscape) {
      text      = StringView(contentBegin, static_cast<size_t>(contentEnd - contentBegin));
      needsCopy = true;
      return ParseError{};
    }

    // --- パス 2: 実寸で確保してデコードする (needsCopy = false) ---
    if (decodedLength == 0) {
      text      = StringView(contentBegin, 0);
      needsCopy = false;
      return ParseError{};
    }

    char* const buffer = s.arena->AllocateArray<char>(decodedLength);
    if (buffer == nullptr) {
      return ParseError{ ErrorCode::OutOfMemory, openOffset };
    }

    size_t written = 0;
    for (const char* q = contentBegin; q < contentEnd; ) {
      if (*q != '\\') {
        buffer[written++] = *q++;
        continue;
      }

      const char escape = q[1];
      switch (escape) {
      case '"':  buffer[written++] = '"';    q += 2; break;
      case '\\': buffer[written++] = '\\';   q += 2; break;
      case '/':  buffer[written++] = '/';    q += 2; break;
      case 'b':  buffer[written++] = '\b';   q += 2; break;
      case 'f':  buffer[written++] = '\f';   q += 2; break;
      case 'n':  buffer[written++] = '\n';   q += 2; break;
      case 'r':  buffer[written++] = '\r';   q += 2; break;
      case 't':  buffer[written++] = '\t';   q += 2; break;
      default: {
        // パス 1 で検証済みなので、ここに来るのは `u` だけ
        std::uint32_t codepoint = 0;
        (void)ReadHex4(q + 2, contentEnd, codepoint);
        q += 6;
        if (IsHighSurrogate(codepoint)) {
          std::uint32_t low = 0;
          (void)ReadHex4(q + 2, contentEnd, low);
          q += 6;
          codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
        }
        written += EncodeUtf8(codepoint, buffer + written);
        break;
      }
      }
    }

    assert(written == decodedLength && "unescape length mismatch between pass 1 and pass 2");

    text      = StringView(buffer, written);
    needsCopy = false;
    return ParseError{};
  }

  // ---------------------------------------------------------------------------
  // 数値 (R1-7 / R1-16 / R1-17 / R1-19)
  // ---------------------------------------------------------------------------

  namespace {

    /**
     * @brief RFC 8259 の数値文法を自前で走査する
     *
     * @details
     *  `from_chars` は `0x10` / `.5` / `1.` / `01` / `1e` / `nan` / `inf` を
     *  素通しするため (v145 で実測)、トークンの切り出しと妥当性検証は
     *  全面的にここで行い、検証済みの厳密な範囲だけを `from_chars` へ渡す (R1-16)。
     */
    ParseError ScanNumberToken(ScanState& s, ValueToken& out) noexcept {
      const char* const tokenBegin  = s.cur;
      const size_t      tokenOffset = OffsetOf(s, s.cur);

      const bool negative = (s.cur < s.end && *s.cur == '-');
      if (negative) {
        ++s.cur;
      }

      // --- 整数部 ---
      if (s.cur >= s.end || !IsDigit(*s.cur)) {
        return ParseError{ ErrorCode::NumberInvalid, OffsetOf(s, s.cur) };
      }

      const char* const intBegin = s.cur;
      if (*s.cur == '0') {
        ++s.cur;
        // 先頭ゼロの禁止: "01" / "-01"
        if (s.cur < s.end && IsDigit(*s.cur)) {
          return ParseError{ ErrorCode::NumberInvalid, OffsetOf(s, s.cur) };
        }
      }
      else {
        while (s.cur < s.end && IsDigit(*s.cur)) {
          ++s.cur;
        }
      }
      const char* const intEnd = s.cur;

      // --- 小数部 ---
      bool              hasFraction = false;
      const char*       fracBegin   = nullptr;
      const char*       fracEnd     = nullptr;
      if (s.cur < s.end && *s.cur == '.') {
        ++s.cur;
        if (s.cur >= s.end || !IsDigit(*s.cur)) {
          return ParseError{ ErrorCode::NumberMissingFraction, OffsetOf(s, s.cur) };
        }
        fracBegin = s.cur;
        while (s.cur < s.end && IsDigit(*s.cur)) {
          ++s.cur;
        }
        fracEnd     = s.cur;
        hasFraction = true;
      }

      // --- 指数部 ---
      bool         hasExponent = false;
      std::int64_t exponent    = 0;
      if (s.cur < s.end && (*s.cur == 'e' || *s.cur == 'E')) {
        ++s.cur;
        bool exponentNegative = false;
        if (s.cur < s.end && (*s.cur == '+' || *s.cur == '-')) {
          exponentNegative = (*s.cur == '-');
          ++s.cur;
        }
        if (s.cur >= s.end || !IsDigit(*s.cur)) {
          return ParseError{ ErrorCode::NumberMissingExponent, OffsetOf(s, s.cur) };
        }
        while (s.cur < s.end && IsDigit(*s.cur)) {
          // 桁を無制限に積むと E10 自体が溢れるので飽和させる (R1-17)
          if (exponent < kExponentLimit) {
            exponent = exponent * 10 + static_cast<std::int64_t>(*s.cur - '0');
            if (exponent > kExponentLimit) {
              exponent = kExponentLimit;
            }
          }
          ++s.cur;
        }
        if (exponentNegative) {
          exponent = -exponent;
        }
        hasExponent = true;
      }

      const char* const tokenEnd = s.cur;

      // --- 数値の直後に英数字 / `.` / `_` が続くなら数値エラー (R1-16) ---
      // これで 0x10 / 1.2.3 / 1e5e5 / 123abc が「末尾ゴミ」ではなく数値として診断される
      if (s.cur < s.end && IsNumberTrailingGarbage(*s.cur)) {
        return ParseError{ ErrorCode::NumberInvalid, OffsetOf(s, s.cur) };
      }

      out.startOffset = tokenOffset;

      // --- 整数として表せる形か (R1-7) ---
      if (!hasFraction && !hasExponent) {
        if (negative) {
          std::int64_t value = 0;
          const auto   result = std::from_chars(tokenBegin, tokenEnd, value);
          if (result.ec == std::errc{} && result.ptr == tokenEnd) {
            // R1-19: "-0" は Int64(0) になり、符号は失われる(意図的な非可逆)
            out.kind = TokenKind::Int64;
            out.i64  = value;
            return ParseError{};
          }
        }
        else {
          std::uint64_t value  = 0;
          const auto    result = std::from_chars(tokenBegin, tokenEnd, value);
          if (result.ec == std::errc{} && result.ptr == tokenEnd) {
            if (value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
              out.kind = TokenKind::Int64;
              out.i64  = static_cast<std::int64_t>(value);
            }
            else {
              out.kind = TokenKind::UInt64;
              out.u64  = value;
            }
            return ParseError{};
          }
        }
        // int64 / uint64 に収まらなかった場合は Double へ落とす
      }

      // --- 10 進指数 E10 を求める (R1-17) ---
      // 先頭の有効数字を 0.d1d2... x 10^E10 の形に正規化したときの指数。
      // E10 = (整数部の桁数) - (先頭有効数字の位置) + (明示指数)
      const auto intDigits = static_cast<std::int64_t>(intEnd - intBegin);

      std::int64_t firstSignificant = -1;
      std::int64_t digitIndex       = 0;
      for (const char* d = intBegin; d < intEnd; ++d, ++digitIndex) {
        if (*d != '0') { firstSignificant = digitIndex; break; }
      }
      if (firstSignificant < 0 && hasFraction) {
        for (const char* d = fracBegin; d < fracEnd; ++d, ++digitIndex) {
          if (*d != '0') { firstSignificant = digitIndex; break; }
        }
      }
      const bool allZero = (firstSignificant < 0);

      std::int64_t e10 = 0;
      if (!allZero) {
        e10 = intDigits - firstSignificant + exponent;
        if (e10 >  2 * kExponentLimit) { e10 =  2 * kExponentLimit; }
        if (e10 < -2 * kExponentLimit) { e10 = -2 * kExponentLimit; }
      }

      // --- Double へ変換する ---
      out.kind = TokenKind::Double;

      double     value  = 0.0;
      const auto result = std::from_chars(tokenBegin, tokenEnd, value);

      if (result.ec == std::errc{}) {
        out.dbl = value;                       // 出力値は必ず自分で代入する
        return ParseError{};
      }

      if (result.ec == std::errc::result_out_of_range) {
        // result_out_of_range はオーバー / アンダーフローの双方で返り、
        // value は規格上不変とされる一方 MSVC は書き換える。よって値は読まず、
        // トークン走査で求めた E10 の符号だけで切り分ける (R1-17)
        if (allZero || e10 <= 0) {
          out.dbl = negative ? -0.0 : 0.0;     // アンダーフローはエラーにしない
          return ParseError{};
        }
        return ParseError{ ErrorCode::NumberTooBig, tokenOffset };
      }

      // ここへは来ない想定(トークンは検証済み)。念のため値を確定させて失敗を返す
      out.dbl = 0.0;
      return ParseError{ ErrorCode::NumberInvalid, tokenOffset };
    }

  }

  // ---------------------------------------------------------------------------
  // 値のディスパッチ
  // ---------------------------------------------------------------------------

  ParseError ScanValue(ScanState& s, ValueToken& out) noexcept {
    const size_t tokenOffset = OffsetOf(s, s.cur);
    out.startOffset = tokenOffset;

    if (s.cur >= s.end) {
      return ParseError{ ErrorCode::ValueInvalid, tokenOffset };
    }

    const char c = *s.cur;
    switch (c) {
    case '{':
      ++s.cur;
      out.kind = TokenKind::ObjectBegin;
      return ParseError{};

    case '[':
      ++s.cur;
      out.kind = TokenKind::ArrayBegin;
      return ParseError{};

    case '"': {
      out.kind = TokenKind::String;
      return ScanString(s, out.text, out.needsCopy);
    }

    case 't':
      if (MatchKeyword(s, "true", 4)) {
        s.cur += 4;
        out.kind = TokenKind::True;
        return ParseError{};
      }
      return ParseError{ ErrorCode::ValueInvalid, tokenOffset };

    case 'f':
      if (MatchKeyword(s, "false", 5)) {
        s.cur += 5;
        out.kind = TokenKind::False;
        return ParseError{};
      }
      return ParseError{ ErrorCode::ValueInvalid, tokenOffset };

    case 'n':
      if (MatchKeyword(s, "null", 4)) {
        s.cur += 4;
        out.kind = TokenKind::Null;
        return ParseError{};
      }
      // 小文字の "nan" はここで落ちる。AllowNaNInf でも受け付けない (R1-18)
      return ParseError{ ErrorCode::ValueInvalid, tokenOffset };

    case 'N':
      // NaN / Infinity は true / false / null と同じキーワード完全一致で照合し、
      // from_chars には一切渡さない。渡すと小文字 nan / inf が漏れる (R1-18)
      if (MatchKeyword(s, "NaN", 3)) {
        if (!HasFlag(s.flags, ParseFlags::AllowNaNInf)) {
          return ParseError{ ErrorCode::NaNInfNotAllowed, tokenOffset };
        }
        s.cur += 3;
        out.kind = TokenKind::Double;
        out.dbl  = std::numeric_limits<double>::quiet_NaN();
        return ParseError{};
      }
      return ParseError{ ErrorCode::ValueInvalid, tokenOffset };

    case 'I':
      if (MatchKeyword(s, "Infinity", 8)) {
        if (!HasFlag(s.flags, ParseFlags::AllowNaNInf)) {
          return ParseError{ ErrorCode::NaNInfNotAllowed, tokenOffset };
        }
        s.cur += 8;
        out.kind = TokenKind::Double;
        out.dbl  = std::numeric_limits<double>::infinity();
        return ParseError{};
      }
      // "Inf" / "INFINITY" などの部分一致はここで落ちる (R1-18)
      return ParseError{ ErrorCode::ValueInvalid, tokenOffset };

    case '-':
      if (MatchKeyword(s, "-Infinity", 9)) {
        if (!HasFlag(s.flags, ParseFlags::AllowNaNInf)) {
          return ParseError{ ErrorCode::NaNInfNotAllowed, tokenOffset };
        }
        s.cur += 9;
        out.kind = TokenKind::Double;
        out.dbl  = -std::numeric_limits<double>::infinity();
        return ParseError{};
      }
      return ScanNumberToken(s, out);

    default:
      if (IsDigit(c)) {
        return ScanNumberToken(s, out);
      }
      // '.' や '+' で始まるトークンはここで落ちる (".5" / "+1")
      return ParseError{ ErrorCode::ValueInvalid, tokenOffset };
    }
  }

}

namespace GLFD::Json {

  bool JsonReader::GrowFrames(Frame*& frames, std::uint32_t& capacity,
                              std::uint32_t used, size_t inputSize) noexcept {
    // 深さ 1 段には最低 1 バイト必要なので、入力長が深さの上界になる。
    // MaxDepth が大きくても、短い入力のために巨大なバッファを取らずに済む
    std::uint64_t wanted = m_maxDepth;
    if (inputSize < wanted) {
      wanted = inputSize;
    }
    // 呼び出し側が used < m_maxDepth を確認済みなので、この引き上げは上限を超えない
    if (wanted < static_cast<std::uint64_t>(used) + 1u) {
      wanted = static_cast<std::uint64_t>(used) + 1u;
    }

    const auto wanted32 = static_cast<std::uint32_t>(wanted);

    // Parse をまたいでバッファを再利用する。容量が足りていれば再確保しない
    if (m_heapFrames == nullptr || m_heapCapacity < wanted32) {
      Frame* const buffer = m_arena->AllocateArray<Frame>(wanted32);
      if (buffer == nullptr) {
        return false;
      }
      m_heapFrames   = buffer;
      m_heapCapacity = wanted32;
    }

    // 既存フレームを移送する。カウンタを失うと OnArrayEnd(n) の n が壊れる
    if (frames != m_heapFrames) {
      for (std::uint32_t i = 0; i < used; ++i) {
        m_heapFrames[i] = frames[i];
      }
    }

    frames   = m_heapFrames;
    capacity = m_heapCapacity;
    return true;
  }

}
