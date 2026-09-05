#pragma once

/**
 * @file  Json.h
 * @brief GLFD JSON サブシステムの集約ヘッダ + エントリポイント (§5.5)
 *
 * @details
 *  ゲームコードが普段触るのはこのヘッダだけでよい。
 *
 *  ## セーブだけを行うなら、このヘッダを include しないこと
 *
 *  **`Json.h` は集約ヘッダなので DOM も Reader も入る。** R3-16 のヘッダ分割は
 *  `JsonWriteArchive.h` 等を**直接** include する利用者のためのものであり、
 *  `Json.h` はその対象外である。書き出ししか行わない翻訳単位では
 *
 *  @code
 *    #include "Core/Json/JsonWriteArchive.h"   // WriteArchive<W>
 *    #include "Core/Json/JsonArchiveTypes.h"   // スカラ / enum / T[N]
 *    #include "Core/Json/JsonWriter.h"         // JsonWriter / ストリーム
 *  @endcode
 *
 *  と書けば、Reader も DOM も `DynamicArray.h` も入らない(T-33 で実測)。
 *  `DynamicArray<T>` / `std::optional<T>` を使う場合のみ
 *  `JsonArchiveContainers.h` を足す。
 *
 *  ## 寿命に関する2つの契約(最重要)
 *
 *  1. **`Load*` は `Document` から以前に得た全てを無効化する。**
 *     読み取った `StringView`・`Value`・`ArchiveIssue::path` は、すべて
 *     `Document` のアリーナ上にある(R0-5)。`Load*` は繰り返し呼んでも確保が
 *     積み上がらないよう内部で `Document::Clear()` を行うため、
 *     **同じ `Document` へ読み直した時点で、前回読んだ文字列はすべて死ぬ。**
 *     したがって `Document` は**読み込んだ構造体と同じ寿命で保持する**こと。
 *     そのため本ヘッダは **`Document` を隠すエントリポイントを提供しない。**
 *     隠す版があると「簡単な方を使ったら文字列が壊れる」という最も踏まれやすい
 *     形が API に残るため、意図的に用意していない。
 *
 *  2. **`Serialize` は `Ar::IsWriting()` のとき値を変更してはならない。**
 *     `Serialize(Ar&, T&)` は読み書き共通で非 const 参照を取る(R3-1)。
 *     書き出しのエントリポイントは `const T&` を受け取り、`Detail::WriteObject`
 *     の中で1回だけ `const_cast` する。書き方向で値を書き換える `Serialize` を
 *     書くと、**本当に const なオブジェクトを保存したときに未定義動作**になる。
 */

#include <cstdint>

#include "Core/MemoryResource.h"
#include "Core/StringView.h"

#include "Core/Json/JsonArchive.h"
#include "Core/Json/JsonArchiveContainers.h"
#include "Core/Json/JsonArchiveTypes.h"
#include "Core/Json/JsonArena.h"
#include "Core/Json/JsonDiagnostics.h"
#include "Core/Json/JsonDocument.h"
#include "Core/Json/JsonQuery.h"
#include "Core/Json/JsonReadArchive.h"
#include "Core/Json/JsonTypes.h"
#include "Core/Json/JsonWriteArchive.h"
#include "Core/Json/JsonWriter.h"

namespace GLFD::Json {

  namespace Detail {

    /**
     * @brief 書き出しの共通実装。**本サブシステムで `const_cast` を行う唯一の箇所**
     *
     * @details
     *  `Serialize` は読み書き共通のため `T&` を取る(R3-1)。一方、保存の入力は
     *  `const T&` が自然である。方向を知っているのはエントリポイントだけなので、
     *  ここで1回だけ橋渡しする。
     *
     *  健全性の根拠: 書き方向では `WriteArchive::Member` / `Element` / `Scalar` が
     *  値を**読むだけ**で、参照を通した書き換えを一切行わない。
     *  この前提はヘッダ冒頭の契約2として利用者にも課している。
     */
    template <class W, class T>
    [[nodiscard]] bool WriteObject(W& writer, ArchiveContext& ctx, const T& in) {
      const bool ok = [&] {
        WriteArchive<W> ar(writer, ctx);
        Serialize(ar, const_cast<T&>(in));
        return ar.Finish();
      }();
      // R3-19 のとおり書き側は最初の失敗で短絡するため、詳細は ctx 側に残る
      return ok && !ctx.HasFatal();
    }

    /// パース済みの `Document` から構造体へ読み出す共通実装
    template <class T>
    [[nodiscard]] bool ReadObject(T& out, Document& doc, ArchiveContext& ctx) {
      if (doc.HasError()) {
        return false;
      }
      {
        ReadArchive ar(doc.Root(), ctx);
        Serialize(ar, out);
      }
      // R3-19 のとおり読み側は短絡しないので、ここに来た時点で診断は出そろっている
      return !ctx.HasFatal();
    }

    /// 繰り返し読み込んでも確保が積み上がらないよう、両者を巻き戻す
    inline void ResetForLoad(Document& doc, ArchiveContext& ctx) noexcept {
      // Document::Parse は scratch しか Reset しないため、Clear しないと
      // ファイル内容と DOM が読むたびに本体アリーナへ積み上がる。
      // Clear は Reset(ブロック保持)なので 2 回目以降の確保は発生しない
      doc.Clear();
      // ctx が doc.Arena() を使っている場合、上の Clear でそのアリーナが
      // 巻き戻る。古いバッファを掴んだままにしないため必ず後始末する
      ctx.Clear();
    }

  }

  // ---------------------------------------------------------------------------
  // 読み込み
  // ---------------------------------------------------------------------------

  /**
   * @brief JSON テキストから構造体を読む(診断つき)
   *
   * @param out   読み込み先。失敗しても部分的に書き換わり得る(R3-4a)
   * @param json  JSON テキスト
   * @param doc   **呼び出し側が所有する** `Document`。読み取った文字列の寿命はこれに一致する
   * @param ctx   診断の受け皿。`ArchiveContext ctx(doc.Arena())` の形で作ると
   *              データと診断の寿命が揃う
   * @param flags 既定は `JsonC`(手書き config 主体のため)
   * @return パースに成功し `ctx.HasFatal()` が立たなかった場合のみ `true`
   *
   * @warning **`doc` から以前に得た `Value` / `StringView` は全て無効になる。**
   * @note  `ctx` は呼び出しごとにリセットされる(診断は累積しない)。
   */
  template <class T>
  [[nodiscard]] bool LoadFromJson(T& out, StringView json, Document& doc, ArchiveContext& ctx,
                                  ParseFlags flags = ParseFlags::JsonC) {
    Detail::ResetForLoad(doc, ctx);
    if (!doc.Parse(json, flags).IsOk()) {
      return false;
    }
    return Detail::ReadObject(out, doc, ctx);
  }

  /// @copydoc LoadFromJson(T&, StringView, Document&, ArchiveContext&, ParseFlags)
  /// @note 診断が不要な場合の簡易版。`ArchiveContext` を内部で作って上を呼ぶ。
  ///       消えるのは**診断だけ**で、`out` の文字列は `doc` 上にあるため無事。
  template <class T>
  [[nodiscard]] bool LoadFromJson(T& out, StringView json, Document& doc,
                                  ParseFlags flags = ParseFlags::JsonC) {
    ArchiveContext ctx(doc.Arena());
    return LoadFromJson(out, json, doc, ctx, flags);
  }

  /**
   * @brief JSON ファイルから構造体を読む(診断つき)
   * @param utf8Path UTF-8 のパス。**ヌル終端が要るため `StringView` は取らない**(R0-8)
   * @copydetails LoadFromJson(T&, StringView, Document&, ArchiveContext&, ParseFlags)
   */
  template <class T>
  [[nodiscard]] bool LoadFromJsonFile(T& out, const char* utf8Path,
                                      Document& doc, ArchiveContext& ctx,
                                      ParseFlags flags = ParseFlags::JsonC) {
    Detail::ResetForLoad(doc, ctx);
    if (!doc.ParseFile(utf8Path, flags).IsOk()) {
      return false;
    }
    return Detail::ReadObject(out, doc, ctx);
  }

  /// @copydoc LoadFromJsonFile(T&, const char*, Document&, ArchiveContext&, ParseFlags)
  template <class T>
  [[nodiscard]] bool LoadFromJsonFile(T& out, const char* utf8Path, Document& doc,
                                      ParseFlags flags = ParseFlags::JsonC) {
    ArchiveContext ctx(doc.Arena());
    return LoadFromJsonFile(out, utf8Path, doc, ctx, flags);
  }

  // ---------------------------------------------------------------------------
  // 書き出し
  // ---------------------------------------------------------------------------

  /**
   * @brief 構造体をメモリバッファへ書き出す(診断つき)
   * @param ctx バージョンはここから取る(`ArchiveContext ctx(buffer.Arena(), version)`)
   * @note  出力は `out.View()` で取れる。`out` を支えるアリーナと同じ寿命を持つ
   */
  template <class T>
  [[nodiscard]] bool SaveToJson(const T& in, JsonStringBuffer& out, ArchiveContext& ctx,
                                bool pretty = false) {
    if (pretty) {
      JsonPrettyWriter<JsonStringBuffer> writer(out);
      return Detail::WriteObject(writer, ctx, in);
    }
    JsonWriter<JsonStringBuffer> writer(out);
    return Detail::WriteObject(writer, ctx, in);
  }

  /// @copydoc SaveToJson(const T&, JsonStringBuffer&, ArchiveContext&, bool)
  /// @note 簡易版。`ArchiveContext` をバッファのアリーナから内部で作る
  template <class T>
  [[nodiscard]] bool SaveToJson(const T& in, JsonStringBuffer& out,
                                std::uint32_t version, bool pretty = false) {
    ArchiveContext ctx(out.Arena(), version);
    return SaveToJson(in, out, ctx, pretty);
  }

  /**
   * @brief 構造体を JSON ファイルへ書き出す
   *
   * @param utf8Path UTF-8 のパス(R0-8)
   * @param resource 一時バッファとファイルストリームの確保元
   * @return 目標ファイルの置き換えまで成功した場合のみ `true`
   *
   * @details
   *  **不完全な JSON を本番ファイルにしない。** `WriteArchive::Finish()` が失敗した
   *  場合、あるいは `ctx.HasFatal()` が立った場合は `JsonFileStream::Abort()` を呼び、
   *  一時ファイルを捨てて**既存ファイルを無傷のまま残す**(R1-20)。
   *  `Finish()` は `MoveFileEx` で目標を置き換えるので、閉じきっていない JSON に
   *  対して呼ぶと壊れたセーブが本物になる。
   *
   *  `ctx.HasFatal()` も見るのは、`JsonSerializer<T>` 特殊化が致命的な問題を
   *  記録した場合に JSON が整形式でも中身がオブジェクトを表していないため。
   *  ロード失敗は既定値で継続できるが、**セーブ失敗はプレイヤーの進行が消える**
   *  (R1-1f)ので、疑わしければ書かない側に倒す。
   *
   * @note 書き側の診断は戻り値に集約される。個別の Issue が要る場合は
   *       `SaveToJson` でバッファへ書き、そのバッファをファイルへ出すこと。
   */
  template <class T>
  [[nodiscard]] bool SaveToJsonFile(const T& in, const char* utf8Path,
                                    Memory::IMemoryResource* resource,
                                    std::uint32_t version, bool pretty = false) {
    JsonArena      arena(resource);
    ArchiveContext ctx(arena, version);
    JsonFileStream stream(utf8Path, arena);

    const bool written = pretty
      ? [&] { JsonPrettyWriter<JsonFileStream> w(stream); return Detail::WriteObject(w, ctx, in); }()
      : [&] { JsonWriter<JsonFileStream>       w(stream); return Detail::WriteObject(w, ctx, in); }();

    if (!written) {
      stream.Abort();   // 一時ファイルを捨てる。既存ファイルは無傷
      return false;
    }
    return stream.Finish();   // ここで初めて置き換わる
  }

}
