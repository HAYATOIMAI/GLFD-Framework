#pragma once

/**
 * @file  JsonReader.h
 * @brief JSON の SAX スキャナ(Layer 1)
 *
 * @details
 *  要件書 §3.4 / §3.5 / §3.5.1 に対応する。RFC 8259 準拠を基準とし、
 *  JSONC 拡張(コメント・末尾カンマ)と NaN / Infinity を opt-in で受け付ける。
 *
 *  ## 構造
 *  **明示スタックによる反復**で実装している。再帰下降ではないため、
 *  入力のネスト深度がネイティブスタックの消費に一切影響しない。
 *  `SetMaxDepth` をいくら大きくしてもスタックオーバーフローしないのはこのため。
 *
 *  ## スレッド安全性
 *  **単一スレッド前提**。ひとつの JsonReader インスタンスへの並行アクセスは
 *  同期されない。グローバル可変状態は持たないため、別インスタンス同士は
 *  JobSystem 上で並列に使用できる (R1-10)。
 *
 *  ## UTF-8 検証の範囲
 *  **文字列の内容のみ**を検証する。文字列の外側では文法上 ASCII の構造文字と
 *  空白しか許されず、0x80 以上のバイトは検証するまでもなく構文エラーになるため。
 *  検証対象には過長符号化・UTF-8 で符号化されたサロゲート (ED A0..BF)・
 *  U+10FFFF 超・途中で切れた列を含む。
 *  **コメント本体は検証しない**(破棄され外部に出ないため)。
 */

#include <cassert>
#include <concepts>
#include <cstdint>

#include "Core/StringView.h"
#include "Core/Json/JsonArena.h"
#include "Core/Json/JsonTypes.h"

namespace GLFD::Json {

  // ---------------------------------------------------------------------------
  // ハンドラ concept (要件書 §3.4)
  // ---------------------------------------------------------------------------

  /**
   * @brief SAX イベントの受け手が満たすべき要件
   *
   * @details
   *  仮想関数を使わずテンプレート + concept で実体化する。呼び出しはインライン化される。
   *  いずれかが `false` を返した時点でパースを中断し、`ErrorCode::HandlerAborted` を返す。
   *
   *  `OnString` / `OnKey` の第 2 引数 `needsCopy` の意味:
   *   - `true`  : **入力バッファ**内を直接指すスライス。入力バッファが受け手より
   *               短命なら複製が必要
   *   - `false` : **アリーナ上**に構築されたアンエスケープ済み文字列。
   *               アリーナが所有しているので複製不要
   */
  template <class T>
  concept JsonHandler = requires(T h, StringView s, bool b,
                                 int64_t i, uint64_t u, double d, uint32_t n) {
    { h.OnNull()        } -> std::same_as<bool>;
    { h.OnBool(b)       } -> std::same_as<bool>;
    { h.OnInt64(i)      } -> std::same_as<bool>;
    { h.OnUInt64(u)     } -> std::same_as<bool>;
    { h.OnDouble(d)     } -> std::same_as<bool>;
    { h.OnString(s, b)  } -> std::same_as<bool>;   // 第 2 引数 = needsCopy
    { h.OnKey(s, b)     } -> std::same_as<bool>;   // 第 2 引数 = needsCopy
    { h.OnObjectBegin() } -> std::same_as<bool>;
    { h.OnObjectEnd(n)  } -> std::same_as<bool>;   // n = メンバ数
    { h.OnArrayBegin()  } -> std::same_as<bool>;
    { h.OnArrayEnd(n)   } -> std::same_as<bool>;   // n = 要素数
  };

  // ---------------------------------------------------------------------------
  // 内部スキャナ (実装は JsonReader.cpp)
  // ---------------------------------------------------------------------------

  namespace Detail {

    /// トークン単位のスキャナが共有する状態
    struct ScanState {
      const char* begin = nullptr;   ///< 入力の先頭(オフセット算出の基準)
      const char* end   = nullptr;   ///< 入力の終端
      const char* cur   = nullptr;   ///< 現在位置
      ParseFlags  flags = ParseFlags::None;
      JsonArena*  arena = nullptr;   ///< アンエスケープ結果の確保先
    };

    /// ScanValue が返すトークンの種別
    enum class TokenKind : std::uint8_t {
      Null, True, False, Int64, UInt64, Double, String, ObjectBegin, ArrayBegin
    };

    /// 値ひとつぶんのスキャン結果
    struct ValueToken {
      TokenKind     kind        = TokenKind::Null;
      std::int64_t  i64         = 0;
      std::uint64_t u64         = 0;
      double        dbl         = 0.0;
      StringView    text        = {};      ///< kind == String のとき有効
      bool          needsCopy   = false;   ///< kind == String のとき有効
      size_t        startOffset = 0;       ///< トークン先頭のバイトオフセット
    };

    /// 入力先頭の BOM を処理する。UTF-16 / UTF-32 の BOM はエラー (R1-4)
    [[nodiscard]] ParseError SkipByteOrderMark(ScanState& s) noexcept;

    /// 空白とコメントを読み飛ばす。「空白が許される位置」すべてで呼ばれる
    [[nodiscard]] ParseError SkipTrivia(ScanState& s) noexcept;

    /// 文字列トークンを読む。cur は開き `"` を指していること
    [[nodiscard]] ParseError ScanString(ScanState& s, StringView& text, bool& needsCopy) noexcept;

    /// 値をひとつ読む。`{` / `[` は消費してマーカーを返す
    [[nodiscard]] ParseError ScanValue(ScanState& s, ValueToken& out) noexcept;

    /// s.begin からのバイトオフセット
    [[nodiscard]] inline size_t OffsetOf(const ScanState& s, const char* p) noexcept {
      return static_cast<size_t>(p - s.begin);
    }

  }

  // ---------------------------------------------------------------------------
  // JsonReader
  // ---------------------------------------------------------------------------

  /**
   * @brief SAX 方式の JSON スキャナ
   *
   * @details
   *  `JsonArena&` を受け取るのは、エスケープを含む文字列のアンエスケープ結果を
   *  どこかに構築する必要があるため。エスケープを含まない文字列は入力バッファを
   *  直接指すので確保は発生しない。
   *
   *  **生存期間**: ハンドラへ渡される `StringView` は、入力バッファまたはアリーナの
   *  いずれかを指す。どちらも JsonReader より長生きしないので、受け手が保持するなら
   *  `needsCopy` に従って複製すること。
   *
   *  **再入**: 同一インスタンスで `Parse` を複数回呼べる。深度スタックの
   *  オーバーフロー用バッファは容量が足りる限り再利用され、追加確保は発生しない。
   */
  class JsonReader final {
  public:
    /// 既定のネスト深度上限
    static constexpr std::uint32_t kDefaultMaxDepth = 256;

    /// アリーナに触れずに扱えるネスト深度。これを超えた分だけアリーナを使う
    static constexpr std::uint32_t kInlineDepth = 64;

    explicit JsonReader(JsonArena& arena) noexcept
      : m_arena(&arena) {
    }

    // 深度スタックのスクラッチ領域を共有すると事故になるためコピー・ムーブを禁じる。
    // 構築は安価なので、必要な場所でその都度作ればよい
    JsonReader(const JsonReader&)            = delete;
    JsonReader& operator=(const JsonReader&) = delete;
    JsonReader(JsonReader&&)                 = delete;
    JsonReader& operator=(JsonReader&&)      = delete;

    /**
     * @brief JSON をパースし、SAX イベントをハンドラへ流す
     *
     * @param json    入力。ヌル終端は不要
     * @param handler JsonHandler を満たすオブジェクト
     * @param flags   拡張構文の許可設定
     * @return 成功なら `IsOk()` が true。失敗時は `code` と `offset` が原因を指す
     *
     * @note
     *  `noexcept` を付けていないのは、**ハンドラ側のユーザーコードを呼ぶため**である。
     *  JsonReader 自身は throw しない(内部スキャナはすべて noexcept)。
     *  ハンドラが throw した場合の挙動は、エンジン方針 (N-2) の対象外。
     */
    template <JsonHandler H>
    [[nodiscard]] ParseError Parse(StringView json, H& handler,
                                   ParseFlags flags = ParseFlags::None);

    /// ネスト深度の上限を設定する。`MaxDepth` 段までは受理し、`MaxDepth + 1` 段目で失敗する
    void SetMaxDepth(std::uint32_t depth) noexcept { m_maxDepth = depth; }

    [[nodiscard]] std::uint32_t MaxDepth() const noexcept { return m_maxDepth; }

  private:
    /// 深度スタックの 1 段。要素数カウンタを兼ねる
    struct Frame {
      std::uint32_t count    = 0;
      bool          isObject = false;
    };

    /// 反復パーサの状態
    enum class State : std::uint8_t { Value, ObjectKey, AfterValue, Done };

    /**
     * @brief インライン領域を使い切ったとき、アリーナ上のバッファへ移送する
     * @param frames   現在のフレーム配列。成功時に更新される
     * @param capacity 現在の容量。成功時に更新される
     * @param used     移送すべき有効フレーム数(カウンタを失わないため)
     * @param inputSize 入力バイト数。深さ 1 段には最低 1 バイト必要なので深さの上界になる
     * @return 確保に失敗したら false
     */
    [[nodiscard]] bool GrowFrames(Frame*& frames, std::uint32_t& capacity,
                                  std::uint32_t used, size_t inputSize) noexcept;

    /// スカラのトークンを対応するハンドラ呼び出しへ振り分ける
    template <JsonHandler H>
    [[nodiscard]] static bool EmitScalar(H& handler, const Detail::ValueToken& token);

    JsonArena*    m_arena       = nullptr;
    std::uint32_t m_maxDepth    = kDefaultMaxDepth;

    // Parse をまたいで再利用する。アリーナは個別解放できないため、
    // 深いネストのファイルを連続で読んでも確保が積み上がらないようにしている。
    //
    // @warning **アリーナ上のポインタをメンバとして保持している。**
    //          同じ `JsonReader` を使い回す場合、その間にアリーナを `Reset()`
    //          してはならない(再配布された領域を指したままになる)。
    //          `Document` は `Parse` ごとに `JsonReader` をローカル構築するため
    //          この経路を踏まない(R2-9)。利用者が `JsonReader` を保持する場合のみ注意。
    Frame*        m_heapFrames   = nullptr;
    std::uint32_t m_heapCapacity = 0;

    Frame         m_inlineFrames[kInlineDepth] = {};
  };

  // ---------------------------------------------------------------------------
  // 実装
  // ---------------------------------------------------------------------------

  template <JsonHandler H>
  bool JsonReader::EmitScalar(H& handler, const Detail::ValueToken& token) {
    switch (token.kind) {
    case Detail::TokenKind::Null:   return handler.OnNull();
    case Detail::TokenKind::True:   return handler.OnBool(true);
    case Detail::TokenKind::False:  return handler.OnBool(false);
    case Detail::TokenKind::Int64:  return handler.OnInt64(token.i64);
    case Detail::TokenKind::UInt64: return handler.OnUInt64(token.u64);
    case Detail::TokenKind::Double: return handler.OnDouble(token.dbl);
    case Detail::TokenKind::String: return handler.OnString(token.text, token.needsCopy);
    case Detail::TokenKind::ObjectBegin:
    case Detail::TokenKind::ArrayBegin:
    default:
      assert(false && "EmitScalar: container tokens must be handled by the caller");
      return false;
    }
  }

  template <JsonHandler H>
  ParseError JsonReader::Parse(StringView json, H& handler, ParseFlags flags) {
    Detail::ScanState s;
    s.begin = json.Data();
    s.end   = json.Data() + json.Size();
    s.cur   = s.begin;
    s.flags = flags;
    s.arena = m_arena;

    if (const ParseError e = Detail::SkipByteOrderMark(s); !e.IsOk()) {
      return e;
    }
    if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
      return e;
    }
    if (s.cur >= s.end) {
      return ParseError{ ErrorCode::DocumentEmpty, Detail::OffsetOf(s, s.cur) };
    }

    Frame*        frames   = m_inlineFrames;
    std::uint32_t capacity = kInlineDepth;
    std::uint32_t depth    = 0;

    State state = State::Value;
    while (state != State::Done) {
      switch (state) {

      case State::Value: {
        Detail::ValueToken token;
        if (const ParseError e = Detail::ScanValue(s, token); !e.IsOk()) {
          return e;
        }

        const bool isObject = (token.kind == Detail::TokenKind::ObjectBegin);
        const bool isArray  = (token.kind == Detail::TokenKind::ArrayBegin);

        if (!isObject && !isArray) {
          if (!EmitScalar(handler, token)) {
            return ParseError{ ErrorCode::HandlerAborted, token.startOffset };
          }
          state = State::AfterValue;
          break;
        }

        // コンテナに入る。MaxDepth 段までは受理し、その次で失敗させる
        if (depth >= m_maxDepth) {
          return ParseError{ ErrorCode::DepthLimitExceeded, token.startOffset };
        }
        if (depth >= capacity && !GrowFrames(frames, capacity, depth, json.Size())) {
          return ParseError{ ErrorCode::OutOfMemory, token.startOffset };
        }

        frames[depth].count    = 0;
        frames[depth].isObject = isObject;
        ++depth;

        if (!(isObject ? handler.OnObjectBegin() : handler.OnArrayBegin())) {
          return ParseError{ ErrorCode::HandlerAborted, token.startOffset };
        }

        if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
          return e;
        }

        // 空のコンテナを先に片付ける
        const char closer = isObject ? '}' : ']';
        if (s.cur < s.end && *s.cur == closer) {
          const size_t closeOffset = Detail::OffsetOf(s, s.cur);
          ++s.cur;
          --depth;
          if (!(isObject ? handler.OnObjectEnd(0) : handler.OnArrayEnd(0))) {
            return ParseError{ ErrorCode::HandlerAborted, closeOffset };
          }
          state = State::AfterValue;
        }
        else {
          state = isObject ? State::ObjectKey : State::Value;
        }
        break;
      }

      case State::ObjectKey: {
        if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
          return e;
        }
        if (s.cur >= s.end || *s.cur != '"') {
          return ParseError{ ErrorCode::ObjectMissingName, Detail::OffsetOf(s, s.cur) };
        }

        const size_t keyOffset = Detail::OffsetOf(s, s.cur);
        StringView   key;
        bool         needsCopy = false;
        if (const ParseError e = Detail::ScanString(s, key, needsCopy); !e.IsOk()) {
          return e;
        }
        if (!handler.OnKey(key, needsCopy)) {
          return ParseError{ ErrorCode::HandlerAborted, keyOffset };
        }

        if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
          return e;
        }
        if (s.cur >= s.end || *s.cur != ':') {
          return ParseError{ ErrorCode::ObjectMissingColon, Detail::OffsetOf(s, s.cur) };
        }
        ++s.cur;

        if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
          return e;
        }
        state = State::Value;
        break;
      }

      case State::AfterValue: {
        if (depth == 0) {
          state = State::Done;
          break;
        }

        Frame& frame = frames[depth - 1];
        if (frame.count != UINT32_MAX) {
          ++frame.count;
        }

        if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
          return e;
        }

        const bool  isObject   = frame.isObject;
        const char  closer     = isObject ? '}' : ']';
        const ErrorCode missing = isObject ? ErrorCode::ObjectMissingCommaOrBrace
                                           : ErrorCode::ArrayMissingCommaOrBracket;

        if (s.cur >= s.end) {
          return ParseError{ missing, Detail::OffsetOf(s, s.cur) };
        }

        if (*s.cur == closer) {
          const size_t closeOffset = Detail::OffsetOf(s, s.cur);
          const std::uint32_t count = frame.count;
          ++s.cur;
          --depth;
          if (!(isObject ? handler.OnObjectEnd(count) : handler.OnArrayEnd(count))) {
            return ParseError{ ErrorCode::HandlerAborted, closeOffset };
          }
          state = State::AfterValue;
          break;
        }

        if (*s.cur == ',') {
          const size_t commaOffset = Detail::OffsetOf(s, s.cur);
          ++s.cur;
          if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
            return e;
          }

          // 末尾カンマ
          if (s.cur < s.end && *s.cur == closer) {
            if (!HasFlag(s.flags, ParseFlags::AllowTrailingComma)) {
              return ParseError{ ErrorCode::TrailingCommaNotAllowed, commaOffset };
            }
            const size_t closeOffset = Detail::OffsetOf(s, s.cur);
            const std::uint32_t count = frame.count;
            ++s.cur;
            --depth;
            if (!(isObject ? handler.OnObjectEnd(count) : handler.OnArrayEnd(count))) {
              return ParseError{ ErrorCode::HandlerAborted, closeOffset };
            }
            state = State::AfterValue;
            break;
          }

          state = isObject ? State::ObjectKey : State::Value;
          break;
        }

        return ParseError{ missing, Detail::OffsetOf(s, s.cur) };
      }

      case State::Done:
      default:
        break;
      }
    }

    // ルート値の後ろにゴミが無いこと
    if (const ParseError e = Detail::SkipTrivia(s); !e.IsOk()) {
      return e;
    }
    if (s.cur < s.end) {
      return ParseError{ ErrorCode::DocumentRootNotSingular, Detail::OffsetOf(s, s.cur) };
    }
    return ParseError{};
  }

}
