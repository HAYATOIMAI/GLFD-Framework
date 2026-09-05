/**
 * @file  JsonTypesTests.cpp
 * @brief JsonTypes.h の単体テスト(フラグ演算子 / ParseError / ComputeLocation / ToString)
 */

#include <cstring>

#include "TestHarness.h"
#include "Core/Json/JsonTypes.h"

using GLFD::StringView;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::Summarize;

using GLFD::Json::ComputeLocation;
using GLFD::Json::ErrorCode;
using GLFD::Json::HasFlag;
using GLFD::Json::ParseError;
using GLFD::Json::ParseFlags;
using GLFD::Json::SourceLocation;
using GLFD::Json::ToString;
using GLFD::Json::ValueType;
using GLFD::Json::WriteFlags;

// ---------------------------------------------------------------------------
// constexpr 検証
// ---------------------------------------------------------------------------

namespace {

  // フラグのビット演算
  static_assert((ParseFlags::AllowComments | ParseFlags::AllowTrailingComma)
                == ParseFlags::JsonC);
  static_assert(HasFlag(ParseFlags::JsonC, ParseFlags::AllowComments));
  static_assert(HasFlag(ParseFlags::JsonC, ParseFlags::AllowTrailingComma));
  static_assert(!HasFlag(ParseFlags::JsonC, ParseFlags::AllowNaNInf));
  static_assert(!HasFlag(ParseFlags::None, ParseFlags::AllowComments));
  // None は「何も指定していない」ので、立っているとは見なさない
  static_assert(!HasFlag(ParseFlags::JsonC, ParseFlags::None));
  static_assert(!HasFlag(ParseFlags::None, ParseFlags::None));
  // 複数ビットの同時判定は「すべて立っている」ことを要求する
  static_assert(HasFlag(ParseFlags::JsonC | ParseFlags::AllowNaNInf, ParseFlags::JsonC));
  static_assert(!HasFlag(ParseFlags::AllowComments, ParseFlags::JsonC));
  static_assert((ParseFlags::JsonC & ParseFlags::AllowComments) == ParseFlags::AllowComments);
  static_assert((ParseFlags::JsonC ^ ParseFlags::AllowComments) == ParseFlags::AllowTrailingComma);
  static_assert(HasFlag(~ParseFlags::AllowComments, ParseFlags::AllowTrailingComma));

  // WriteFlags 側にも同じ演算子が効くこと
  static_assert(HasFlag(WriteFlags::AllowNaNInf | WriteFlags::EscapeNonAscii,
                        WriteFlags::EscapeNonAscii));
  static_assert(!HasFlag(WriteFlags::None, WriteFlags::AllowNaNInf));

  // ParseError
  static_assert(ParseError{}.IsOk());
  static_assert(ParseError{}.code == ErrorCode::None);
  static_assert(ParseError{}.offset == 0);
  static_assert(!ParseError{ ErrorCode::ValueInvalid, 3 }.IsOk());

  // ToString
  static_assert(ToString(ErrorCode::None) == StringView("None"));
  static_assert(ToString(ErrorCode::ObjectMissingColon) == StringView("ObjectMissingColon"));
  static_assert(ToString(ErrorCode::HandlerAborted) == StringView("HandlerAborted"));

  // ComputeLocation
  static_assert(ComputeLocation(StringView("abc"), 0).line == 1);
  static_assert(ComputeLocation(StringView("abc"), 0).column == 1);
  static_assert(ComputeLocation(StringView("abc"), 2).column == 3);
  static_assert(ComputeLocation(StringView("a\nb"), 2).line == 2);
  static_assert(ComputeLocation(StringView("a\nb"), 2).column == 1);

  // ValueType の並びが要件書 §3.1 のとおりであること
  static_assert(static_cast<int>(ValueType::Null)   == 0);
  static_assert(static_cast<int>(ValueType::Bool)   == 1);
  static_assert(static_cast<int>(ValueType::Int64)  == 2);
  static_assert(static_cast<int>(ValueType::UInt64) == 3);
  static_assert(static_cast<int>(ValueType::Double) == 4);
  static_assert(static_cast<int>(ValueType::String) == 5);
  static_assert(static_cast<int>(ValueType::Array)  == 6);
  static_assert(static_cast<int>(ValueType::Object) == 7);

  // 基底型が要件どおりであること
  static_assert(sizeof(ValueType) == 1);
  static_assert(sizeof(ErrorCode) == 2);
  static_assert(sizeof(ParseFlags) == 4);
  static_assert(sizeof(WriteFlags) == 4);

}

// ---------------------------------------------------------------------------
// フラグの実行時動作(複合代入)
// ---------------------------------------------------------------------------

static void Test_Flags() {
  BeginCase("flags: compound assignment and round-trip");

  ParseFlags flags = ParseFlags::None;
  CHECK(!HasFlag(flags, ParseFlags::AllowComments));

  flags |= ParseFlags::AllowComments;
  CHECK(HasFlag(flags, ParseFlags::AllowComments));
  CHECK(!HasFlag(flags, ParseFlags::AllowTrailingComma));

  flags |= ParseFlags::AllowTrailingComma;
  CHECK(flags == ParseFlags::JsonC);

  flags &= ~ParseFlags::AllowComments;
  CHECK(flags == ParseFlags::AllowTrailingComma);

  flags ^= ParseFlags::AllowTrailingComma;
  CHECK(flags == ParseFlags::None);

  // InSitu はフラグとして存在するだけ(フェーズ 3)。他と衝突しないこと
  CHECK(!HasFlag(ParseFlags::JsonC, ParseFlags::InSitu));
  CHECK(HasFlag(ParseFlags::JsonC | ParseFlags::InSitu, ParseFlags::InSitu));

  WriteFlags writeFlags = WriteFlags::None;
  writeFlags |= WriteFlags::EscapeNonAscii;
  CHECK(HasFlag(writeFlags, WriteFlags::EscapeNonAscii));
  CHECK(!HasFlag(writeFlags, WriteFlags::AllowNaNInf));
}

// ---------------------------------------------------------------------------
// ToString: 全コードが一意で空でない名前を返すこと
// ---------------------------------------------------------------------------

static void Test_ToString() {
  BeginCase("ToString: every ErrorCode maps to a unique, non-empty name");

  static const ErrorCode kAll[] = {
    ErrorCode::None,
    ErrorCode::DocumentEmpty,
    ErrorCode::DocumentRootNotSingular,
    ErrorCode::ValueInvalid,
    ErrorCode::ObjectMissingName,
    ErrorCode::ObjectMissingColon,
    ErrorCode::ObjectMissingCommaOrBrace,
    ErrorCode::ArrayMissingCommaOrBracket,
    ErrorCode::StringUnterminated,
    ErrorCode::StringEscapeInvalid,
    ErrorCode::StringUnicodeEscapeInvalid,
    ErrorCode::StringUnicodeSurrogateInvalid,
    ErrorCode::StringControlCharUnescaped,
    ErrorCode::StringInvalidUtf8,
    ErrorCode::NumberMissingFraction,
    ErrorCode::NumberMissingExponent,
    ErrorCode::NumberTooBig,
    ErrorCode::NumberInvalid,
    ErrorCode::CommentNotAllowed,
    ErrorCode::CommentUnterminated,
    ErrorCode::TrailingCommaNotAllowed,
    ErrorCode::NaNInfNotAllowed,
    ErrorCode::EncodingNotSupported,
    ErrorCode::DepthLimitExceeded,
    ErrorCode::OutOfMemory,
    ErrorCode::FileNotFound,
    ErrorCode::FileReadFailed,
    ErrorCode::FileWriteFailed,
    ErrorCode::PathEncodingInvalid,
    ErrorCode::HandlerAborted,
    ErrorCode::WriteInvalidValue,
    ErrorCode::WriteIncompleteDocument,
    ErrorCode::WriteStreamFailure,
  };
  constexpr size_t kCount = sizeof(kAll) / sizeof(kAll[0]);

  // 要件書 §3.3 の 29 件 (HandlerAborted / EncodingNotSupported 含む)
  // + ファイル I/O 4 件 = 33 件
  CHECK(kCount == 33);

  for (size_t i = 0; i < kCount; ++i) {
    const StringView name = ToString(kAll[i]);
    CHECK(!name.Empty());
    CHECK(name != StringView("<unknown>"));

    // ASCII のみであること(ログ・ファイルへ出しても壊れないこと)
    for (size_t k = 0; k < name.Size(); ++k) {
      CHECK(static_cast<unsigned char>(name[k]) < 0x80u);
    }

    // 名前が一意であること
    for (size_t j = i + 1; j < kCount; ++j) {
      CHECK(name != ToString(kAll[j]));
    }
  }

  // 定義されていない値は "<unknown>" になること(クラッシュしない)
  CHECK(ToString(static_cast<ErrorCode>(9999)) == StringView("<unknown>"));
}

// ---------------------------------------------------------------------------
// ComputeLocation
// ---------------------------------------------------------------------------

static void Test_ComputeLocation_LineEndings() {
  BeginCase("ComputeLocation: LF / CRLF / lone CR each count as one break");

  // LF のみ
  {
    const StringView src("ab\ncd\nef");
    CHECK(ComputeLocation(src, 0).line == 1);
    CHECK(ComputeLocation(src, 0).column == 1);
    CHECK(ComputeLocation(src, 1).column == 2);
    CHECK(ComputeLocation(src, 3).line == 2);      // 'c'
    CHECK(ComputeLocation(src, 3).column == 1);
    CHECK(ComputeLocation(src, 4).column == 2);
    CHECK(ComputeLocation(src, 6).line == 3);      // 'e'
    CHECK(ComputeLocation(src, 6).column == 1);
  }

  // CRLF は 2 バイトで 1 区切り
  {
    const StringView src("ab\r\ncd\r\nef");
    CHECK(ComputeLocation(src, 4).line == 2);      // 'c'
    CHECK(ComputeLocation(src, 4).column == 1);
    CHECK(ComputeLocation(src, 5).column == 2);
    CHECK(ComputeLocation(src, 8).line == 3);      // 'e'
    CHECK(ComputeLocation(src, 8).column == 1);
  }

  // 単独 CR も 1 区切り
  {
    const StringView src("ab\rcd\ref");
    CHECK(ComputeLocation(src, 3).line == 2);
    CHECK(ComputeLocation(src, 3).column == 1);
    CHECK(ComputeLocation(src, 6).line == 3);
    CHECK(ComputeLocation(src, 6).column == 1);
  }

  // LF / CRLF / CR の混在
  {
    const StringView src("a\nb\r\nc\rd");
    CHECK(ComputeLocation(src, 0).line == 1);      // 'a'
    CHECK(ComputeLocation(src, 2).line == 2);      // 'b'
    CHECK(ComputeLocation(src, 2).column == 1);
    CHECK(ComputeLocation(src, 5).line == 3);      // 'c'
    CHECK(ComputeLocation(src, 5).column == 1);
    CHECK(ComputeLocation(src, 7).line == 4);      // 'd'
    CHECK(ComputeLocation(src, 7).column == 1);
  }

  // 改行そのものを指した場合は、その行の末尾を指す
  {
    const StringView src("ab\ncd");
    CHECK(ComputeLocation(src, 2).line == 1);
    CHECK(ComputeLocation(src, 2).column == 3);
  }
}

static void Test_ComputeLocation_Boundaries() {
  BeginCase("ComputeLocation: EOF clamping and empty input");

  const StringView src("abc\ndef");

  // offset == Size() は EOF 位置
  CHECK(ComputeLocation(src, src.Size()).line == 2);
  CHECK(ComputeLocation(src, src.Size()).column == 4);

  // offset > Size() は Size() へクランプ(クラッシュしない)
  CHECK(ComputeLocation(src, 9999).line == 2);
  CHECK(ComputeLocation(src, 9999).column == 4);
  CHECK(ComputeLocation(src, StringView::npos).line == 2);

  // 空入力
  const StringView empty;
  CHECK(ComputeLocation(empty, 0).line == 1);
  CHECK(ComputeLocation(empty, 0).column == 1);
  CHECK(ComputeLocation(empty, 5).line == 1);
  CHECK(ComputeLocation(empty, 5).column == 1);

  // 末尾が改行の入力
  const StringView trailing("abc\n");
  CHECK(ComputeLocation(trailing, trailing.Size()).line == 2);
  CHECK(ComputeLocation(trailing, trailing.Size()).column == 1);
}

static void Test_ComputeLocation_Utf8Columns() {
  BeginCase("ComputeLocation: column counts UTF-8 code points, not bytes");

  // "あいう" は UTF-8 で 3 バイト x 3 文字。この .cpp は UTF-8 BOM 付きなので
  // 文字列リテラルは UTF-8 バイト列として格納される
  const char utf8[] = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86X";   // あいうX
  const StringView src(utf8, 10);

  CHECK(src.Size() == 10);
  CHECK(ComputeLocation(src, 0).column == 1);    // 'あ' の先頭
  CHECK(ComputeLocation(src, 3).column == 2);    // 'い' の先頭
  CHECK(ComputeLocation(src, 6).column == 3);    // 'う' の先頭
  CHECK(ComputeLocation(src, 9).column == 4);    // 'X'
  // バイト単位で数えていたら 'X' は 10 桁目になるはず。そうなっていないことを確認
  CHECK(ComputeLocation(src, 9).column != 10);

  // 多バイト文字の途中を指した場合も、その文字を 1 桁として数える
  CHECK(ComputeLocation(src, 1).column == 2);
  CHECK(ComputeLocation(src, 2).column == 2);

  // 2 バイト文字 / 4 バイト文字
  const char mixed[] = "\xC3\xA9\xF0\x9F\x98\x80Z";              // e-acute, emoji, Z
  const StringView mixedView(mixed, 7);
  CHECK(ComputeLocation(mixedView, 0).column == 1);
  CHECK(ComputeLocation(mixedView, 2).column == 2);   // emoji の先頭
  CHECK(ComputeLocation(mixedView, 6).column == 3);   // 'Z'

  // 不正な UTF-8 は 1 バイトを 1 桁として数える(必ず算出できること)
  {
    const char broken[] = { 'a', '\xE3', '\x81', 'b', 'c' };   // 3 バイト列が途中で壊れている
    const StringView brokenView(broken, 5);
    const SourceLocation loc = ComputeLocation(brokenView, 4);
    CHECK(loc.line == 1);
    CHECK(loc.column >= 1 && loc.column <= 5);   // 破綻せず何らかの値を返すこと
  }
  {
    // 継続バイトが単独で現れるケース
    const char lone[] = { '\x81', '\x82', 'a' };
    const StringView loneView(lone, 3);
    CHECK(ComputeLocation(loneView, 2).column == 3);
  }
  {
    // 先行バイトの直後に入力が尽きるケース
    const char truncated[] = { 'a', '\xF0' };
    const StringView truncatedView(truncated, 2);
    CHECK(ComputeLocation(truncatedView, 2).column == 3);
  }
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("JsonTypes");

  Test_Flags();
  Test_ToString();
  Test_ComputeLocation_LineEndings();
  Test_ComputeLocation_Boundaries();
  Test_ComputeLocation_Utf8Columns();

  return Summarize();
}
