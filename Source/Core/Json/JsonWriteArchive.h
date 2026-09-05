#pragma once

/**
 * @file  JsonWriteArchive.h
 * @brief Layer 3: Writer へ直接書き出すアーカイブ (R3-15 / R3-16)
 *
 * @details
 *  **中間 DOM を作らない。** `Serialize` が `Member` を呼ぶ順にそのまま Writer へ
 *  流し込む。セーブ 1 回あたりの `Value` 確保がまるごと不要になる。
 *
 *  依存は `JsonArchive.h` と `JsonWriter.h` のみ。**`JsonReader.h` も `JsonValue.h` も
 *  `JsonDocument.h` も引き込まない**(R3-16)。セーブだけを行う利用者が Reader と DOM を
 *  コンパイルしないことを T-23 で `/showIncludes` により機械的に検証する。
 *
 *  使い方:
 *  @code
 *    JsonStringBuffer   buffer(arena);
 *    JsonWriter<JsonStringBuffer> writer(buffer);
 *    ArchiveContext     ctx(arena, kSaveVersion);
 *    {
 *      WriteArchive     ar(writer, ctx);
 *      Serialize(ar, save);        // ルートは ar が既に開いている
 *      if (!ar.Finish()) { ... }   // 失敗はすべてここに集約される
 *    }
 *  @endcode
 */

#include <cassert>
#include <cstdint>
#include <type_traits>

#include "Core/StringView.h"
#include "Core/Json/JsonArchive.h"
#include "Core/Json/JsonWriter.h"

namespace GLFD::Json {

  /**
   * @brief 書き込み側のアーカイブ (§5.2)
   *
   * @tparam W `JsonWriter<S>` / `JsonPrettyWriter<S>` などの Writer 型
   *
   * @details
   *  **ルートの開閉**: コンストラクタで `ObjectBegin` と `"$version"` を出し、
   *  `Finish()` で閉じる(論点1)。`JsonFileStream`(R1-20)と同じ形にしてある。
   *  両者は組で使われるため、片方だけ `Finish()` が要る設計は書き忘れを招く。
   *
   *  コンストラクタは失敗を返せないが問題にならない。`ObjectBegin` が失敗しても
   *  Writer 側に error が latch され、以降の `Member` が全て `false` を返し、
   *  最終的に `Finish()` が `false` になる。**失敗はすべて `Finish()` に集約される。**
   *
   *  **失敗後は短絡する**(論点6)。最初の失敗でフラグを latch し、以降の `Member` は
   *  ディスパッチせず即座に `false` を返す。`Serialize` は `void` を返すため
   *  ユーザーコードからは中断できないが、各 `Member` がフラグ 1 個の検査になるので
   *  実質ゼロコストで抜ける。壊れた Writer に書き続けても得るものが無いためである。
   *  (読み側は逆に短絡しない。診断を最後まで集めることが読み側の価値だから)
   *
   *  **パスは追跡しない**(R3-9)。書き込み時のオーバーヘッドを避けるためで、
   *  `Serialize` のコードは読み書きで完全に同一のまま保たれる。
   */
  template <class W>
  class WriteArchive {
  public:
    WriteArchive(W& writer, ArchiveContext& ctx) noexcept
      : m_writer(&writer), m_ctx(&ctx) {
      if (!m_writer->ObjectBegin()) { Fail(); return; }
      if (!m_writer->Key(kVersionKey)) { Fail(); return; }
      if (!m_writer->UInt64(static_cast<std::uint64_t>(ctx.Version()))) { Fail(); }
    }

    ~WriteArchive() {
      // Finish() も Abort() も呼ばれずに破棄された = 書き忘れ。
      // JsonFileStream と同じ理由で、明示的な Abort() でのみ抑制する
      assert((m_finished || m_aborted)
             && "WriteArchive: neither Finish() nor Abort() was called");
    }

    WriteArchive(const WriteArchive&)            = delete;
    WriteArchive& operator=(const WriteArchive&) = delete;

    static constexpr bool IsReading() noexcept { return false; }
    static constexpr bool IsWriting() noexcept { return true; }

    [[nodiscard]] std::uint32_t Version() const noexcept { return m_ctx->Version(); }
    [[nodiscard]] bool          Failed()  const noexcept { return m_failed; }

    /**
     * @brief 診断を記録するための一般の口(読み側と同じ面を保つためにある)
     * @note  書き側はパスを追跡しないため (R3-9)、ここで記録した Issue の
     *        `path` は空になる。`Serialize` のコードを読み書きで同一に
     *        保つために用意しており、書き側から使うことはほとんどない
     */
    [[nodiscard]] ArchiveContext& Context() noexcept { return *m_ctx; }

    // -------------------------------------------------------------------------
    // メンバ
    // -------------------------------------------------------------------------

    /// R3-3: 呼び出し順がそのまま出力順になる。並べ替えない
    template <class T>
    bool Member(StringView key, T& value) {
      if (m_failed) { return false; }
      assert(!IsReservedKey(key)
             && "WriteArchive::Member: keys starting with '$' are reserved (R3-10)");
      if (!m_writer->Key(key)) { return Fail(); }
      if (!Detail::ArchiveOne(*this, value)) { return Fail(); }
      return true;
    }

    /**
     * @brief 既定値つきのメンバ
     * @note  既定では第 3 引数を**無視して常に出力する**。省略は `SkipDefaults` で opt-in。
     *
     * @warning `SkipDefaults` で省略したフィールドを、読み側が `RequiredMember` で
     *          読むと `MissingRequired` になる。省略できるのは読み側が既定値つきの
     *          `Member` で受ける field だけである
     */
    template <class T>
    bool Member(StringView key, T& value, const T& defaultValue) {
      if (m_failed) { return false; }
      if (HasFlag(m_ctx->Flags(), ArchiveFlags::SkipDefaults)) {
        // 比較できない型では省略しない(論点8)。多く出力する側に倒れるのは常に安全
        if constexpr (Detail::ArchiveComparable<T>) {
          if (value == defaultValue) { return true; }
        }
      }
      return Member(key, value);
    }

    /// 書き側では通常の `Member` と同じ。必須性は読み側の関心事である
    template <class T>
    bool RequiredMember(StringView key, T& value) {
      return Member(key, value);
    }

    // -------------------------------------------------------------------------
    // 配列 (論点3)
    // -------------------------------------------------------------------------

    /**
     * @brief 配列を開き、書き出す要素数を返す
     * @param count コンテナの要素数
     * @return `count` をそのまま返す(失敗時は 0)
     */
    std::uint32_t BeginArray(std::uint32_t count) {
      if (m_failed) { return 0; }
      if (!m_writer->ArrayBegin()) { (void)Fail(); return 0; }
      return count;
    }

    bool EndArray() {
      if (m_failed) { return false; }
      if (!m_writer->ArrayEnd()) { return Fail(); }
      return true;
    }

    template <class T>
    bool Element(T& value) {
      if (m_failed) { return false; }
      if (!Detail::ArchiveOne(*this, value)) { return Fail(); }
      return true;
    }

    // -------------------------------------------------------------------------
    // ディスパッチが呼ぶ面
    // -------------------------------------------------------------------------

    bool BeginObject() {
      if (m_failed) { return false; }
      if (!m_writer->ObjectBegin()) { return Fail(); }
      return true;
    }

    bool EndObject() {
      if (m_failed) { return false; }
      if (!m_writer->ObjectEnd()) { return Fail(); }
      return true;
    }

    /**
     * @brief `null` を書き出す (1-6 で追加)
     * @note  `ReadArchive::IsNull()` と対になる。`std::optional<T>` のように
     *        **値の不在を型で表す**型が必要とする。スカラ表には `null` に
     *        対応する C++ の型が無いため、書き出す口がここにしかない
     */
    bool Null() {
      if (m_failed) { return false; }
      if (!m_writer->Null()) { return Fail(); }
      return true;
    }

    /// 組み込みスカラの書き出し。幅は `std::is_*` で分岐するので型ごとの重複が無い
    template <class T>
    bool Scalar(T& value) {
      if (m_failed) { return false; }

      bool ok = false;
      if constexpr (std::is_same_v<T, bool>) {
        ok = m_writer->Bool(value);
      }
      else if constexpr (std::is_same_v<T, StringView>) {
        ok = m_writer->String(value);
      }
      else if constexpr (std::is_floating_point_v<T>) {
        ok = m_writer->Double(static_cast<double>(value));
      }
      else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        ok = m_writer->Int64(static_cast<std::int64_t>(value));
      }
      else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
        ok = m_writer->UInt64(static_cast<std::uint64_t>(value));
      }
      else {
        static_assert(Detail::kAlwaysFalse<T>, "WriteArchive::Scalar: unsupported scalar type");
      }

      if (!ok) { return Fail(); }
      return true;
    }

    // -------------------------------------------------------------------------
    // 終了
    // -------------------------------------------------------------------------

    /**
     * @brief ルートオブジェクトを閉じ、書き出しが完了したかを返す
     *
     * @warning 戻り値を無視するとセーブが静かに失敗する。`FileWriteFailed` を
     *          設けてまで対処しようとしている失敗モードそのものなので `[[nodiscard]]`
     */
    [[nodiscard]] bool Finish() noexcept {
      if (m_finished || m_aborted) { return false; }
      m_finished = true;

      if (m_failed) { return false; }
      if (!m_writer->ObjectEnd()) { return Fail(); }
      if (!m_writer->IsComplete()) { return Fail(); }
      return true;
    }

    /// 意図的な中止。デストラクタの assert を抑制する
    void Abort() noexcept { m_aborted = true; }

  private:
    /**
     * @brief 最初の失敗を latch し、コンテキストへ 1 度だけ記録する
     * @note  書き側はパスを追跡しないため path は空になる
     */
    bool Fail() noexcept {
      if (!m_failed) {
        m_failed = true;
        m_ctx->Report(ArchiveErrorKind::ContainerFailure,
                      StringView("the writer failed (out of memory or write error)"));
      }
      return false;
    }

    W*              m_writer   = nullptr;
    ArchiveContext* m_ctx      = nullptr;
    bool            m_failed   = false;
    bool            m_finished = false;
    bool            m_aborted  = false;
  };

}
