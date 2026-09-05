#pragma once

/**
 * @file  JsonTypes.h
 * @brief JSON サブシステムの全層で共有する型定義
 *
 * @details
 *  要件書 §3.1 〜 §3.3 に対応する。Layer 1 (SAX) 〜 Layer 3 (Archive) が
 *  共通で参照するため、この層より下位のもの (JsonArena) には依存しない。
 *  外部ライブラリ・STL コンテナ・例外・RTTI を一切使用しない。
 */

#include <cstdint>
#include <cstddef>
#include <type_traits>

#include "Core/StringView.h"

namespace GLFD::Json {

  // ---------------------------------------------------------------------------
  // 値の型 (要件書 §3.1)
  // ---------------------------------------------------------------------------

  /**
   * @brief JSON の値の種別
   * @note  整数と浮動小数を内部で区別する。エンティティ ID (uint32) と
   *        座標 (float) を型として分離できることが実用上の要点である
   */
  enum class ValueType : std::uint8_t {
    Null,
    Bool,
    Int64,
    UInt64,
    Double,
    String,
    Array,
    Object,
  };

  // ---------------------------------------------------------------------------
  // フラグ列挙 (要件書 §3.2 / §3.6)
  // ---------------------------------------------------------------------------

  /**
   * @brief パース時の拡張構文の許可設定
   * @note  手書き config / パラメータには JsonC を、セーブデータには None を推奨する
   */
  enum class ParseFlags : std::uint32_t {
    None               = 0,
    AllowComments      = 1u << 0,   ///< // 行コメントと /* */ ブロックコメント
    AllowTrailingComma = 1u << 1,   ///< 配列・オブジェクトの最終要素の後の ,
    AllowNaNInf        = 1u << 2,   ///< NaN / Infinity / -Infinity (非標準トークン)
    InSitu             = 1u << 3,   ///< フェーズ 3。現在は指定されても無視する
    JsonC              = AllowComments | AllowTrailingComma,
  };

  /**
   * @brief 書き出し時のオプション
   * @note  実際に使用するのはフェーズ 1-4 (JsonWriter)。ここでは定義のみ行う
   */
  enum class WriteFlags : std::uint32_t {
    None           = 0,
    AllowNaNInf    = 1u << 0,   ///< NaN / Infinity を非標準トークンとして出力する
    EscapeNonAscii = 1u << 1,   ///< 非 ASCII を \uXXXX へ。既定は UTF-8 のまま出力
  };

  /// ビット演算子を提供するフラグ列挙であることを表す特性
  template <class E>
  struct IsFlagEnum : std::false_type {};

  template <> struct IsFlagEnum<ParseFlags> : std::true_type {};
  template <> struct IsFlagEnum<WriteFlags> : std::true_type {};

  template <class E>
  concept FlagEnum = IsFlagEnum<E>::value;

  template <FlagEnum E>
  [[nodiscard]] constexpr E operator|(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
  }

  template <FlagEnum E>
  [[nodiscard]] constexpr E operator&(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
  }

  template <FlagEnum E>
  [[nodiscard]] constexpr E operator^(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));
  }

  template <FlagEnum E>
  [[nodiscard]] constexpr E operator~(E a) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(~static_cast<U>(a)));
  }

  template <FlagEnum E>
  constexpr E& operator|=(E& a, E b) noexcept { a = a | b; return a; }

  template <FlagEnum E>
  constexpr E& operator&=(E& a, E b) noexcept { a = a & b; return a; }

  template <FlagEnum E>
  constexpr E& operator^=(E& a, E b) noexcept { a = a ^ b; return a; }

  /**
   * @brief flag のビットが value にすべて立っているか
   * @note  flag が 0 (None) の場合は false を返す。「何も指定していない」ことを
   *        「立っている」と解釈しないための規約である
   */
  template <FlagEnum E>
  [[nodiscard]] constexpr bool HasFlag(E value, E flag) noexcept {
    using U = std::underlying_type_t<E>;
    const U bits = static_cast<U>(flag);
    return (bits != U{ 0 }) && ((static_cast<U>(value) & bits) == bits);
  }

  // ---------------------------------------------------------------------------
  // エラーコード (要件書 §3.3)
  // ---------------------------------------------------------------------------

  /**
   * @brief パース / 書き出しの失敗理由
   *
   * @details
   *  「何が期待されていたか」が分かる粒度を保つこと (R1-2)。手書き config の
   *  デバッグ体験がこのエラー品質で決まる。
   *
   * @warning この enum の数値はビルド間で安定しない (明示的な値を持たないため、
   *          要素を追加すると後続の値がずれる)。**永続化・ネットワーク送出をしないこと。**
   *          外部へ出す必要がある場合は ToString() の文字列を使う。
   */
  enum class ErrorCode : std::uint16_t {
    None = 0,

    // --- 構文 ---
    DocumentEmpty,                  ///< 値が 1 つも無い (空・空白のみ)
    DocumentRootNotSingular,        ///< ルート値の後ろにゴミがある
    ValueInvalid,                   ///< 値が来るべき位置に値として解釈できないものがある
    ObjectMissingName,              ///< オブジェクトのキー (文字列) が無い
    ObjectMissingColon,             ///< キーの後に : が無い
    ObjectMissingCommaOrBrace,      ///< メンバの後に , でも } でもないものがある
    ArrayMissingCommaOrBracket,     ///< 要素の後に , でも ] でもないものがある

    // --- 文字列 ---
    StringUnterminated,             ///< 閉じ " が現れないまま入力が尽きた
    StringEscapeInvalid,            ///< 未知のエスケープ文字 (\q など)
    StringUnicodeEscapeInvalid,     ///< \uXXXX の桁不正
    StringUnicodeSurrogateInvalid,  ///< 上位 / 下位サロゲートの不整合
    StringControlCharUnescaped,     ///< U+0000..U+001F が生で出現
    StringInvalidUtf8,              ///< 不正な UTF-8 バイト列

    // --- 数値 ---
    NumberMissingFraction,          ///< 小数点の後に数字が無い (1.)
    NumberMissingExponent,          ///< 指数部に数字が無い (1e / 1e+)
    NumberTooBig,                   ///< double の表現域を超過 (オーバーフローのみ)
    NumberInvalid,                  ///< 先頭ゼロ (01) / 数値直後の余分な英数字 (0x10) など

    // --- 拡張構文 ---
    CommentNotAllowed,              ///< AllowComments 未指定でコメントが出現
    CommentUnterminated,            ///< /* が閉じない
    TrailingCommaNotAllowed,        ///< AllowTrailingComma 未指定で末尾カンマが出現
    NaNInfNotAllowed,               ///< AllowNaNInf 未指定で NaN / Infinity が出現

    // --- 入力エンコーディング (要件書 §3.3 への追加) ---
    /**
     * @brief **JSON 入力**が UTF-8 ではない
     * @note  UTF-16 / UTF-32 の BOM を検出した場合に返る。JSON サブシステムは
     *        UTF-8 のみを扱う (R1-4)。文字列**内容**の UTF-8 不正は
     *        StringInvalidUtf8 であり、こちらは**入力全体のエンコーディング**が
     *        違うことを表す。両者を分けているのは、前者がデータの誤りであるのに対し
     *        後者はファイルの保存形式の誤りで、利用者が取るべき対処が異なるため
     * @note  **パス文字列**のエンコーディング不正はこれではなく PathEncodingInvalid。
     *        こちらはファイルの中身を調べに行く合図、あちらは呼び出し側のコードを
     *        直しに行く合図で、調査先が異なる
     */
    EncodingNotSupported,

    // --- リソース ---
    DepthLimitExceeded,             ///< ネスト深度が MaxDepth を超えた
    OutOfMemory,                    ///< JsonArena からの確保に失敗した

    // --- ファイル I/O (要件書 §3.3 への追加。発生源は ReadEntireFile のみ) ---
    /**
     * @brief パスが存在しない
     * @note  offset は常に 0(ファイル全体の問題であり、入力内の位置を指さない)
     */
    FileNotFound,

    /**
     * @brief 開けたが読めない、またはそれ以外の理由で開けなかった
     * @note  権限不足・共有違反・I/O エラーをまとめて表す。パスが存在するかどうかで
     *        調べる先が変わるため FileNotFound とだけ分けている。
     *        offset は常に 0
     */
    FileReadFailed,

    /**
     * @brief 書き込み、または一時ファイルからの置き換えに失敗した
     * @note  ディスク満杯・書き込み権限・アンチウイルスによるロック・
     *        ネットワークドライブ切断など、**書き込み時にしか起きない**理由を含む。
     *        `FileReadFailed` と分けているのは、利用者への影響が根本的に違うため —
     *        読み込み失敗はデフォルト値で継続できる余地があるが、
     *        **書き込み失敗はプレイヤーの進行が消える**。ゲーム側の応答も分岐する。
     *        一時ファイル方式を採っているため、**このエラーが返っても
     *        既存のファイルは無傷**である。offset は常に 0
     */
    FileWriteFailed,

    /**
     * @brief **パス文字列**が有効な UTF-8 ではない
     * @note  パスは UTF-8 と規定しており、UTF-16 へ変換して OS API へ渡す。
     *        その変換に失敗した場合に返る。**ファイルの中身ではなくパスの話**であり、
     *        直すべきは呼び出し側のコードである(JSON 入力の側は EncodingNotSupported)。
     *        offset は常に 0
     */
    PathEncodingInvalid,

    // --- ハンドラ (R1-1c) ---
    /**
     * @brief SAX ハンドラが false を返してパースを中断した
     * @note  「JSON の構文は正しいが、ハンドラが止めた」ことを構文エラーと
     *        曖昧さなく区別するための専用コード。offset は中断を引き起こした
     *        イベントのトークン開始位置を指す
     */
    HandlerAborted,

    // --- 書き込み側 (フェーズ 1-4 で使用。ここでは定義のみ) ---
    WriteInvalidValue,              ///< 有効化されていない NaN / Inf 等
    WriteIncompleteDocument,        ///< 未クローズの Object / Array のまま終了
    WriteStreamFailure,             ///< 出力ストリームが失敗を返した
  };

  // ---------------------------------------------------------------------------
  // エラー情報
  // ---------------------------------------------------------------------------

  /**
   * @brief パース結果。成功時は code == None
   * @note  offset は常にエラー原因の位置を指す (R1-1)。line / column は
   *        パース中に保持せず、必要時に ComputeLocation で算出する
   */
  struct ParseError {
    ErrorCode code   = ErrorCode::None;  ///< 失敗理由。成功なら None
    size_t    offset = 0;                ///< 入力先頭からのバイトオフセット

    [[nodiscard]] constexpr bool IsOk() const noexcept { return code == ErrorCode::None; }

    // 要件書 §3.3 には explicit operator bool があるが、意図的に提供しない。
    // 型名が Error である以上 `if (err)` は「エラーなら」と読めてしまい、
    // 実際の意味 (成功なら true) と反転する。この危険はコメントで注意喚起するのではなく、
    // 型から演算子を取り除いて排除する。判定は必ず IsOk() を明示的に書くこと。
  };

  /// ソース上の位置。line / column ともに 1 起点
  struct SourceLocation {
    std::uint32_t line   = 1;
    std::uint32_t column = 1;
  };

  /**
   * @brief バイトオフセットから行 / 桁を算出する
   *
   * @param source パース対象の入力全体
   * @param offset 入力先頭からのバイトオフセット
   * @return 1 起点の行 / 桁
   *
   * @details
   *  **改行の数え方**: `"\n"` / `"\r\n"` / 単独の `"\r"` のいずれも 1 行の区切りとして
   *  数える。`"\r\n"` は 2 バイトで 1 区切り (`"\r"` を見た時点で次が `"\n"` なら
   *  1 バイト余分に読み飛ばす)。単独 `"\r"` も区切りとするのは、エディタの表示行と
   *  一致させるためである。
   *
   *  **column の単位**: **UTF-8 コードポイント**。バイトではない。先行バイトから
   *  求まる長さぶんをまとめて 1 コードポイントとして数え、不正・途中で切れた
   *  シーケンスは 1 バイトを 1 コードポイントとして数える。エラー位置そのものが
   *  不正バイトであり得るため、どんな入力でも必ず算出できる必要があるからである。
   *  バイト単位ではなくコードポイント単位を採るのは、日本語を含む手書き config で
   *  エディタ表示の桁と大きくずれないようにするため (R1-2)。
   *  バイト精度が必要な場合は ParseError::offset をそのまま使うこと。
   *
   *  offset が source.Size() を超える場合は Size() にクランプする (EOF 位置を返す)。
   *
   * @note  パース中には呼ばれない。高速パスを汚さないため、診断が必要な時だけ使う
   */
  [[nodiscard]] constexpr SourceLocation ComputeLocation(StringView source,
                                                         size_t offset) noexcept {
    const size_t limit = (offset < source.Size()) ? offset : source.Size();

    std::uint32_t line   = 1;
    std::uint32_t column = 1;

    size_t i = 0;
    while (i < limit) {
      const unsigned char c = static_cast<unsigned char>(source[i]);

      if (c == '\n') {
        ++line;
        column = 1;
        ++i;
        continue;
      }
      if (c == '\r') {
        ++line;
        column = 1;
        ++i;
        // "\r\n" を 1 区切りとして扱う
        if (i < limit && source[i] == '\n') {
          ++i;
        }
        continue;
      }

      // UTF-8 の先行バイトから、この文字が占めるバイト数を求める
      size_t advance = 1;
      if      ((c & 0xE0u) == 0xC0u) { advance = 2; }
      else if ((c & 0xF0u) == 0xE0u) { advance = 3; }
      else if ((c & 0xF8u) == 0xF0u) { advance = 4; }

      if (advance > 1) {
        // 継続バイトが揃っていなければ不正とみなし、1 バイトを 1 桁として数える。
        // 継続バイトの走査は limit ではなく入力全体に対して行う
        // (offset が多バイト文字の途中を指すことがあるため)
        size_t k = 1;
        for (; k < advance && (i + k) < source.Size(); ++k) {
          if ((static_cast<unsigned char>(source[i + k]) & 0xC0u) != 0x80u) {
            break;
          }
        }
        if (k != advance) {
          advance = 1;
        }
      }

      i += advance;
      ++column;
    }

    return SourceLocation{ line, column };
  }

  /**
   * @brief エラーコードを識別子名の文字列へ変換する
   * @return 静的記憶域を指す ASCII の StringView。ヌル終端は保証しない
   * @note   ログ・ファイルへ出しても壊れないよう、意図的に ASCII の
   *         enum 識別子名をそのまま返す。人間向けの日本語メッセージが必要なら
   *         上位の診断層で対応付けること
   */
  [[nodiscard]] constexpr StringView ToString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None:                          return "None";

    case ErrorCode::DocumentEmpty:                 return "DocumentEmpty";
    case ErrorCode::DocumentRootNotSingular:       return "DocumentRootNotSingular";
    case ErrorCode::ValueInvalid:                  return "ValueInvalid";
    case ErrorCode::ObjectMissingName:             return "ObjectMissingName";
    case ErrorCode::ObjectMissingColon:            return "ObjectMissingColon";
    case ErrorCode::ObjectMissingCommaOrBrace:     return "ObjectMissingCommaOrBrace";
    case ErrorCode::ArrayMissingCommaOrBracket:    return "ArrayMissingCommaOrBracket";

    case ErrorCode::StringUnterminated:            return "StringUnterminated";
    case ErrorCode::StringEscapeInvalid:           return "StringEscapeInvalid";
    case ErrorCode::StringUnicodeEscapeInvalid:    return "StringUnicodeEscapeInvalid";
    case ErrorCode::StringUnicodeSurrogateInvalid: return "StringUnicodeSurrogateInvalid";
    case ErrorCode::StringControlCharUnescaped:    return "StringControlCharUnescaped";
    case ErrorCode::StringInvalidUtf8:             return "StringInvalidUtf8";

    case ErrorCode::NumberMissingFraction:         return "NumberMissingFraction";
    case ErrorCode::NumberMissingExponent:         return "NumberMissingExponent";
    case ErrorCode::NumberTooBig:                  return "NumberTooBig";
    case ErrorCode::NumberInvalid:                 return "NumberInvalid";

    case ErrorCode::CommentNotAllowed:             return "CommentNotAllowed";
    case ErrorCode::CommentUnterminated:           return "CommentUnterminated";
    case ErrorCode::TrailingCommaNotAllowed:       return "TrailingCommaNotAllowed";
    case ErrorCode::NaNInfNotAllowed:              return "NaNInfNotAllowed";

    case ErrorCode::EncodingNotSupported:          return "EncodingNotSupported (input is not UTF-8)";

    case ErrorCode::DepthLimitExceeded:            return "DepthLimitExceeded";
    case ErrorCode::OutOfMemory:                   return "OutOfMemory";

    case ErrorCode::WriteInvalidValue:             return "WriteInvalidValue";
    case ErrorCode::WriteIncompleteDocument:       return "WriteIncompleteDocument";
    case ErrorCode::WriteStreamFailure:            return "WriteStreamFailure";

    case ErrorCode::FileNotFound:                  return "FileNotFound";
    case ErrorCode::FileReadFailed:                return "FileReadFailed";
    case ErrorCode::FileWriteFailed:
      return "FileWriteFailed (write or replace failed; existing file left intact)";
    case ErrorCode::PathEncodingInvalid:           return "PathEncodingInvalid (path is not valid UTF-8)";

    case ErrorCode::HandlerAborted:                return "HandlerAborted";
    }
    return "<unknown>";
  }

}
