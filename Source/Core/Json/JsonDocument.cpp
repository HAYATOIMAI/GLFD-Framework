/**
 * @file  JsonDocument.cpp
 * @brief Document の実装
 */

#include "Core/Json/JsonDocument.h"

#include "Core/Json/JsonFileIO.h"
// パス形式の規約は Layer 3 側にある。ヘッダではなくここで引き込む
#include "Core/Json/JsonQuery.h"

namespace GLFD::Json {

  const Value* Document::Query(StringView path) const noexcept {
    return Json::Query(m_root, path);
  }

  void Document::Clear() noexcept {
    // Release ではなく Reset。ブロックを保持するので、直後の Parse で
    // IMemoryResource への追加要求が発生しない (R0-4)
    m_arena.Reset();
    m_scratch.Reset();
    m_root   = Value{};
    m_source = StringView{};   // アリーナごと消えるので参照を残さない (2-4)
    m_error  = ParseError{};
  }

  ParseError Document::ParseInPlace(StringView json, ParseFlags flags) {
    // 構築スタックは毎回まっさらから始める。scratch は呼び出し側で Reset 済み
    DomBuilder builder(m_arena, m_scratch);
    builder.Begin();

    // JsonReader は copy / move ともに delete されているためメンバに持てない。
    // Document 1 個につき Parse は通常 1 回なので、深度スタックのバッファ再利用が
    // 失われても実質的な損失は無い
    JsonReader reader(m_arena);
    reader.SetMaxDepth(m_maxDepth);

    ParseError error = reader.Parse(json, builder, flags);

    // ハンドラ(= DOM ビルダ)が確保に失敗して中断した場合、JsonReader からは
    // HandlerAborted として返る。実際の理由へ置き換える
    if (error.code == ErrorCode::HandlerAborted && builder.Error() != ErrorCode::None) {
      error.code = builder.Error();
    }

    // 構築スタックを回収する。ここで scratch 上のポインタはすべて無効になるが、
    // 確定した DOM は m_arena 上にあるので影響しない
    m_scratch.Reset();

    if (!error.IsOk()) {
      // R2-7: 部分構築された DOM を露出しない
      m_root  = Value{};
      m_error = error;
      return error;
    }

    m_root  = builder.Root();
    m_error = ParseError{};
    return error;
  }

  ParseError Document::Parse(StringView json, ParseFlags flags) {
    Clear();
    return ParseInPlace(json, flags);
  }

  ParseError Document::ParseFile(const char* utf8Path, ParseFlags flags) {
    Clear();

    // ファイル内容は m_arena へ読み込む。パース後、エスケープを含まなかった
    // 文字列はこの領域を直接指したままになるが、m_arena は Document と
    // 生存期間が一致するので安全である。
    // ここを m_scratch へ移すと、直後の Reset で全文字列がダングリングする
    StringView contents;
    ErrorCode  ioError = ErrorCode::None;
    if (!ReadEntireFile(utf8Path, m_arena, contents, ioError)) {
      m_root  = Value{};
      m_error = ParseError{ ioError, 0 };   // ファイル系のエラーは offset を持たない
      return m_error;
    }

    // Clear() を呼ばない。呼ぶと今読み込んだ内容ごと巻き戻ってしまう
    // 診断で `line:column` を出すために保持する (2-4)。これはアリーナ上の
    // テキストであり Document と寿命が一致する。`Parse(StringView)` 経路では
    // 呼び出し側のバッファなので保持しない
    m_source = contents;
    return ParseInPlace(contents, flags);
  }

}
