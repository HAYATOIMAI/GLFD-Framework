#pragma once

/**
 * @file  JsonWriter.h
 * @brief 出力ストリームと JSON ライタ(Layer 1 の書き込み側)
 *
 * @details
 *  要件書 §3.6 に対応する。
 *
 *  ## 層の向き
 *  このヘッダは **`Value` を一切知らない**。SAX 直結で minify / 整形だけしたい
 *  利用者 (R1-15) が DOM をコンパイルせずに済むようにするため。
 *  DOM を書き出す `WriteValue` / `Document::WriteTo` は Layer 2 側
 *  (`JsonDocument.h`)に置いてある。
 *
 *  ## Win32 の隔離
 *  `JsonFileStream` の OS 呼び出しは `JsonFileIO.h` の関数群へ委譲しており、
 *  このヘッダにも `JsonWriter.cpp` にも `<windows.h>` は現れない (R2-10)。
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <concepts>

#include "Core/StringView.h"
#include "Core/Json/JsonArena.h"
#include "Core/Json/JsonFileIO.h"
#include "Core/Json/JsonTypes.h"

namespace GLFD::Json {

  // ---------------------------------------------------------------------------
  // 出力ストリーム concept (要件書 §3.6)
  // ---------------------------------------------------------------------------

  /**
   * @brief バイト列の受け手が満たすべき要件
   * @note  いずれも失敗時に `false` を返す。**アボートしない** (R0-3 と同じ方針)
   */
  template <class T>
  concept JsonOutputStream = requires(T s, char c, const char* p, size_t n) {
    { s.Put(c)      } -> std::same_as<bool>;
    { s.Write(p, n) } -> std::same_as<bool>;
    { s.Flush()     } -> std::same_as<bool>;
  };

  // ---------------------------------------------------------------------------
  // JsonStringBuffer
  // ---------------------------------------------------------------------------

  /**
   * @brief アリーナ上に出力を組み立てるメモリバッファ
   *
   * @details
   *  ## 生存期間の契約
   *  `View()` が返す `StringView` は**アリーナ上**を指す。アリーナが
   *  `Reset()` / `Release()` された時点で無効になる。
   *  **`Document::Scratch()` を使った場合、次の `Parse()` / `Clear()` までが有効期間**。
   *
   *  ## ヌル終端しない (R0-7)
   *  `View().Data()` を C 文字列として扱ってはならない。`printf` へ渡すなら
   *  `"%.*s"` と `(int)Size()` を併用すること。
   *
   *  ## 伸長 (R2-5b と同じ方針)
   *  既定初期容量 1024 バイト、倍率 2。アリーナは個別解放できないため、
   *  伸長のたびに旧領域が放棄される。累積放棄量は最終容量とおおむね等しい
   *  (出力サイズぶんが余分に積もるだけ)。**出力量の見当が付く場合は
   *  `Reserve()` を先に呼べば伸長は一度も起きない。**
   */
  class JsonStringBuffer final {
  public:
    static constexpr size_t kDefaultInitialCapacity = 1024;

    /**
     * @param arena           出力の置き場
     * @param initialCapacity 0 なら最初の書き込みまで確保しない(遅延確保)
     */
    explicit JsonStringBuffer(JsonArena& arena, size_t initialCapacity = 0) noexcept
      : m_arena(&arena) {
      if (initialCapacity != 0) {
        (void)Reserve(initialCapacity);
      }
    }

    JsonStringBuffer(const JsonStringBuffer&)            = delete;
    JsonStringBuffer& operator=(const JsonStringBuffer&) = delete;

    /// capacity バイトぶんを先に確保する。失敗したら false
    [[nodiscard]] bool Reserve(size_t capacity) noexcept;

    bool Put(char c) {
      if (m_size == m_capacity && !Grow(m_size + 1)) {
        return false;
      }
      m_data[m_size++] = c;
      return true;
    }

    bool Write(const char* data, size_t size) {
      if (size == 0) {
        return true;
      }
      assert(data != nullptr && "JsonStringBuffer::Write: null data with size > 0");
      if (m_capacity - m_size < size && !Grow(m_size + size)) {
        return false;
      }
      CopyBytes(m_data + m_size, data, size);
      m_size += size;
      return true;
    }

    /// メモリバッファなので何もしない。concept を満たすために存在する
    bool Flush() { return true; }

    /// 書き込んだ内容。**ヌル終端しない**
    [[nodiscard]] StringView View() const noexcept { return StringView(m_data, m_size); }

    [[nodiscard]] size_t Size()     const noexcept { return m_size; }
    [[nodiscard]] size_t Capacity() const noexcept { return m_capacity; }

    /// 一度でも確保に失敗したか。`Put` / `Write` の戻り値を取りこぼしても検知できる
    [[nodiscard]] bool HasFailed() const noexcept { return m_failed; }

    /// 書き込み先のアリーナ。`ArchiveContext` を作るときに使う
    [[nodiscard]] JsonArena& Arena() const noexcept { return *m_arena; }

    /**
     * @brief 内容を捨てる。**容量は保持する**(同じアリーナで繰り返し使える)
     *
     * @warning **アリーナが `Reset()` された後にこのバッファを使い続けてはならない。**
     *          `Clear()` は容量を保持する = **アリーナ上の古いバッファを指したまま**に
     *          なるため、`Reset()` で再配布された領域へ書き込むことになる。
     *          特に `Document::Scratch()` を渡した `JsonStringBuffer` を保持したまま
     *          `Document::Parse()` を呼ぶと、Parse が scratch を `Reset()` するので
     *          この状態になる。アリーナを跨いで使い回さず、作り直すこと。
     *          (同じ危険を `Detail::ArenaVector` は `Clear()` でバッファごと捨てる形で
     *           回避している。こちらが容量保持を残しているのは、1-4 で「同じアリーナで
     *           繰り返し使う」用途を意図して選んだ性質であり、その前提の下では安全)
     */
    void Clear() noexcept {
      m_size   = 0;
      m_failed = false;
    }

  private:
    [[nodiscard]] bool Grow(size_t required) noexcept;

    static void CopyBytes(char* destination, const char* source, size_t size) noexcept;

    JsonArena* m_arena    = nullptr;
    char*      m_data     = nullptr;
    size_t     m_size     = 0;
    size_t     m_capacity = 0;
    bool       m_failed   = false;
  };

  static_assert(JsonOutputStream<JsonStringBuffer>,
                "JsonStringBuffer must satisfy JsonOutputStream");

  // ---------------------------------------------------------------------------
  // JsonFileStream
  // ---------------------------------------------------------------------------

  /**
   * @brief ファイルへの書き出し(内部バッファリング + 一時ファイル方式)
   *
   * @details
   *  ## セーブデータの完全性
   *  書き込みは**一時ファイル**に対して行い、`Finish()` で目標ファイルへ
   *  原子的に置き換える。書き込み途中でクラッシュしても
   *  **既存のファイルは無傷で残る**。直接上書きだと、途中で落ちた時点で
   *  古いセーブごと失われる。
   *
   *  `Finish()` を呼ばずに破棄された場合、デストラクタが一時ファイルを削除する。
   *  **中途半端な内容が目標ファイルに現れることは無い。**
   *
   *  ## `Flush()` の意味
   *  内部バッファを**一時ファイルへ**吐き出すだけで、置き換えは行わない。
   *  `JsonOutputStream` の `Flush` に置き換えまで持たせると、Writer が途中で
   *  `Flush` を呼んだ瞬間に不完全な JSON が本番ファイルになってしまう。
   *  **置き換えは `Finish()` だけが行う。**
   *
   *  ## パス
   *  **UTF-8 かつヌル終端**であること (R0-8)。内部で UTF-16 へ変換して
   *  `CreateFileW` を呼ぶ (R2-12)。
   *
   *  ## 失敗が現れるタイミング
   *  実際に開くのは `<目標パス>.<pid>.<tick>.tmp` という**別のパス**なので、
   *  「目標が既存のディレクトリである」「目標が読み取り専用である」といった
   *  **目標側の問題は構築時ではなく `Finish()` で表面化する**。構築時に検出できるのは
   *  パスの文字コード不正・親ディレクトリの不在・一時ファイルを作れない状況に限られる。
   *  いずれの場合も既存ファイルは無傷のままである。
   */
  class JsonFileStream final {
  public:
    /// 内部バッファの既定サイズ。JsonArena の既定ブロックサイズに揃えてある
    static constexpr size_t kDefaultBufferSize = 64 * 1024;

    /**
     * @param utf8Path   出力先。UTF-8 かつヌル終端
     * @param arena      内部バッファとパス変換に使う
     * @param bufferSize 内部バッファのバイト数
     * @note  構築が失敗することがある。`IsOpen()` / `Error()` で確認すること
     */
    JsonFileStream(const char* utf8Path, JsonArena& arena,
                   size_t bufferSize = kDefaultBufferSize) noexcept;

    /// `Finish()` されていなければ一時ファイルを削除して破棄する
    ~JsonFileStream();

    JsonFileStream(const JsonFileStream&)            = delete;
    JsonFileStream& operator=(const JsonFileStream&) = delete;
    JsonFileStream(JsonFileStream&&)                 = delete;
    JsonFileStream& operator=(JsonFileStream&&)      = delete;

    [[nodiscard]] bool      IsOpen() const noexcept { return m_handle.open; }
    [[nodiscard]] ErrorCode Error()  const noexcept { return m_error; }

    bool Put(char c);
    bool Write(const char* data, size_t size);

    /// 内部バッファを一時ファイルへ吐き出す。**置き換えはしない**
    bool Flush();

    /**
     * @brief 書き切って目標ファイルへ置き換える
     * @return 成功したら true。失敗時は `Error()` に理由が入り、
     *         **目標ファイルは無傷のまま**一時ファイルが削除される
     * @note  2 回目以降の呼び出しは false を返す(既に閉じているため)
     * @note  `[[nodiscard]]` を付けているのは、戻り値を無視すると
     *        **セーブが静かに失敗する**ためである。これは `FileWriteFailed` を
     *        設けてまで対処しようとしている失敗モードそのもので、
     *        コメントによる注意喚起では防げない
     */
    [[nodiscard]] bool Finish() noexcept;

    /**
     * @brief 書き込みを明示的に中止する
     * @details
     *  一時ファイルを削除し、目標ファイルには一切触れない。
     *  **`Finish()` を書き忘れて破棄された場合と区別するために存在する。**
     *  どちらも「一時ファイルが消えて既存ファイルが無傷」という同じ結果になるが、
     *  前者は意図した中止、後者は「セーブしたつもりで何も書かれていない」という
     *  最悪の失敗であり、コードからは区別できない。
     *  明示的に中止する意図があるときは必ずこれを呼ぶこと
     */
    void Abort() noexcept;

  private:
    [[nodiscard]] bool FlushBuffer() noexcept;

    FileWriteHandle m_handle;
    char*           m_buffer     = nullptr;
    size_t          m_bufferSize = 0;
    size_t          m_pending    = 0;
    ErrorCode       m_error      = ErrorCode::None;
  };

  static_assert(JsonOutputStream<JsonFileStream>,
                "JsonFileStream must satisfy JsonOutputStream");

  // ---------------------------------------------------------------------------
  // 内部ヘルパ(実装は JsonWriter.cpp。テンプレートから切り離してヘッダを軽く保つ)
  // ---------------------------------------------------------------------------

  namespace Detail {

    /// 数値の書式化に必要なバッファサイズ(shortest round-trip の double を含む)
    inline constexpr size_t kNumberBufferSize = 48;

    [[nodiscard]] size_t FormatInt64(std::int64_t value, char* buffer, size_t capacity) noexcept;
    [[nodiscard]] size_t FormatUInt64(std::uint64_t value, char* buffer, size_t capacity) noexcept;

    /**
     * @brief double を shortest round-trip 表現で書式化する (R1-11)
     * @details
     *  `std::to_chars` の既定(shortest round-trip)を使い、ロケールに依存しない。
     *  **結果が整数に見える場合は `.0` を付ける。** `to_chars(1.0)` は `"1"` を返すが、
     *  そのまま出すと再パース時に `Int64(1)` になり **型が変わってしまう**。
     *  `-0.0` も `"-0"` → `"-0.0"` となり符号が保たれる。
     * @note  NaN / Inf は呼び出し側で先に処理すること(ここへ渡してはならない)
     */
    [[nodiscard]] size_t FormatDouble(double value, char* buffer, size_t capacity) noexcept;

    /**
     * @brief UTF-8 を 1 コードポイント読む
     * @return 消費バイト数 (1..4)。不正なら 0
     * @note   `EscapeNonAscii` 指定時にのみ必要になる。既定の透過出力では呼ばれない
     */
    [[nodiscard]] std::uint32_t DecodeUtf8(const char* p, const char* end,
                                           std::uint32_t& outCodepoint) noexcept;

    /// 整形方針。テンプレート引数として渡し、`if constexpr` で分岐する
    struct CompactFormat { static constexpr bool kPretty = false; };
    struct PrettyFormat  { static constexpr bool kPretty = true;  };

    /**
     * @brief `JsonWriter` / `JsonPrettyWriter` の共通実装
     *
     * @details
     *  整形の違いは `Format::kPretty` による `if constexpr` 分岐だけで、
     *  **状態機械の実装は 1 本しかない**。派生側は何も再定義しないので、
     *  「非仮想メソッドを隠して整形が効かなくなる」事故が起きようがない。
     *  DOM 走査 (`WriteValue`) は自由関数テンプレートとして Layer 2 側に置き、
     *  渡された Writer の実型に解決させている。
     *
     *  ## assert とエラーコードの使い分け
     *  - **呼び出し順序の誤用**(値の位置で `Key`、閉じ過ぎ、括弧の不一致 など)
     *    = **コードの不具合**なので Debug で `assert`、Release で `WriteInvalidValue` (R1-14)
     *  - **データ由来の失敗**(`AllowNaNInf` 無しでの NaN / Inf、`EscapeNonAscii` 時の
     *    不正 UTF-8、`kMaxDepth` を超えるネスト)= **到達し得る入力**なので
     *    `assert` せず、エラーコードだけを記録する
     *
     *  後者で assert しないのは、同じ入力に対して Debug が停止し Release が
     *  エラーを返す、という挙動差を作らないため。`JsonReader` が不正な入力データを
     *  一貫してエラーコードで返すのと揃えてある。
     */
    template <JsonOutputStream S, class Format>
    class JsonWriterImpl {
    public:
      /// ネストの上限。`JsonReader::kDefaultMaxDepth` と一致させてある
      static constexpr std::uint32_t kMaxDepth = 256;

      explicit JsonWriterImpl(S& stream, WriteFlags flags = WriteFlags::None) noexcept
        : m_stream(&stream)
        , m_flags(flags) {
      }

      JsonWriterImpl(const JsonWriterImpl&)            = delete;
      JsonWriterImpl& operator=(const JsonWriterImpl&) = delete;

      // --- 値 ---

      bool Null()       { return WriteToken("null", 4); }
      bool Bool(bool v) { return v ? WriteToken("true", 4) : WriteToken("false", 5); }

      bool Int64(std::int64_t v) {
        char         buffer[kNumberBufferSize];
        const size_t length = FormatInt64(v, buffer, sizeof(buffer));
        return WriteToken(buffer, length);
      }

      bool UInt64(std::uint64_t v) {
        char         buffer[kNumberBufferSize];
        const size_t length = FormatUInt64(v, buffer, sizeof(buffer));
        return WriteToken(buffer, length);
      }

      bool Double(double v) {
        // NaN / Inf は既定でエラー (R1-12)
        if (v != v) {
          return WriteNonFinite("NaN", 3);
        }
        if (v == kInfinity) {
          return WriteNonFinite("Infinity", 8);
        }
        if (v == -kInfinity) {
          return WriteNonFinite("-Infinity", 9);
        }
        char         buffer[kNumberBufferSize];
        const size_t length = FormatDouble(v, buffer, sizeof(buffer));
        return WriteToken(buffer, length);
      }

      bool String(StringView v) {
        if (!BeginValue()) {
          return false;
        }
        if (!WriteQuoted(v)) {
          return false;
        }
        EndValue();
        return true;
      }

      // --- キーとコンテナ ---

      bool Key(StringView k) {
        if (m_error != ErrorCode::None) {
          return false;
        }
        if (m_depth == 0 || !IsObjectAt(m_depth - 1) || m_keyWritten) {
          // ルート位置 / 配列の中 / キーの連続 はいずれも誤用 (R1-14)
          return Misuse();
        }

        const std::uint32_t level = m_depth - 1;
        if (HasElementAt(level) && !Raw(',')) {
          return false;
        }
        SetHasElementAt(level);
        if (!NewlineAndIndent(m_depth)) {
          return false;
        }
        if (!WriteQuoted(k) || !Raw(':')) {
          return false;
        }
        if constexpr (Format::kPretty) {
          if (!Raw(' ')) {
            return false;
          }
        }
        m_keyWritten = true;
        return true;
      }

      bool ObjectBegin() { return ContainerBegin(true); }
      bool ArrayBegin()  { return ContainerBegin(false); }
      bool ObjectEnd()   { return ContainerEnd(true); }
      bool ArrayEnd()    { return ContainerEnd(false); }

      // --- 状態の問い合わせ ---

      /// ルート値が 1 つ書かれ、全てのコンテナが閉じているか
      [[nodiscard]] bool IsComplete() const noexcept {
        return m_error == ErrorCode::None && m_depth == 0 && m_rootDone;
      }

      /**
       * @brief 最初に起きた失敗の理由
       * @note  ストリームが `false` を返した場合は `WriteStreamFailure` になる。
       *        その裏側の理由(`JsonStringBuffer` なら確保失敗)は
       *        ストリーム側の `HasFailed()` / `Error()` で確認すること
       */
      [[nodiscard]] ErrorCode Error() const noexcept { return m_error; }

      [[nodiscard]] std::uint32_t Depth() const noexcept { return m_depth; }

    protected:
      void SetIndentInternal(char character, std::uint32_t count) noexcept {
        m_indentChar  = character;
        m_indentCount = count;
      }

    private:
      static constexpr double kInfinity = 1e308 * 10.0;   // コンパイル時に +inf になる

      // --- ビットスタック(256 段 x 2 ビット = 64 バイト)---

      [[nodiscard]] bool IsObjectAt(std::uint32_t level) const noexcept {
        return (m_isObject[level >> 6] & (1ull << (level & 63u))) != 0;
      }
      void SetIsObjectAt(std::uint32_t level, bool value) noexcept {
        const std::uint64_t bit = 1ull << (level & 63u);
        if (value) { m_isObject[level >> 6] |= bit; }
        else       { m_isObject[level >> 6] &= ~bit; }
      }
      [[nodiscard]] bool HasElementAt(std::uint32_t level) const noexcept {
        return (m_hasElement[level >> 6] & (1ull << (level & 63u))) != 0;
      }
      void SetHasElementAt(std::uint32_t level) noexcept {
        m_hasElement[level >> 6] |= (1ull << (level & 63u));
      }
      void ClearHasElementAt(std::uint32_t level) noexcept {
        m_hasElement[level >> 6] &= ~(1ull << (level & 63u));
      }

      // --- 失敗の記録 ---

      /// 呼び出し順序の誤用。Debug では assert、Release ではコードを記録 (R1-14)
      bool Misuse() noexcept {
        assert(false && "JsonWriter: misuse of the call sequence");
        if (m_error == ErrorCode::None) {
          m_error = ErrorCode::WriteInvalidValue;
        }
        return false;
      }

      bool StreamFailed() noexcept {
        if (m_error == ErrorCode::None) {
          m_error = ErrorCode::WriteStreamFailure;
        }
        return false;
      }

      // --- 生の出力 ---

      bool Raw(char c) {
        return m_stream->Put(c) ? true : StreamFailed();
      }
      bool Raw(const char* data, size_t size) {
        return m_stream->Write(data, size) ? true : StreamFailed();
      }

      bool NewlineAndIndent(std::uint32_t depth) {
        if constexpr (!Format::kPretty) {
          (void)depth;
          return true;
        }
        else {
          if (!Raw('\n')) {
            return false;
          }
          const std::uint32_t total = depth * m_indentCount;
          for (std::uint32_t i = 0; i < total; ++i) {
            if (!Raw(m_indentChar)) {
              return false;
            }
          }
          return true;
        }
      }

      // --- 値の位置合わせ ---

      bool BeginValue() {
        if (m_error != ErrorCode::None) {
          return false;
        }
        if (m_depth == 0) {
          // ルート値は 1 つだけ
          return m_rootDone ? Misuse() : true;
        }

        const std::uint32_t level = m_depth - 1;
        if (IsObjectAt(level)) {
          if (!m_keyWritten) {
            return Misuse();   // キーが要る位置に値が来た
          }
          m_keyWritten = false;   // カンマ・改行・コロンは Key() が出している
          return true;
        }

        if (HasElementAt(level) && !Raw(',')) {
          return false;
        }
        SetHasElementAt(level);
        return NewlineAndIndent(m_depth);
      }

      void EndValue() noexcept {
        if (m_depth == 0) {
          m_rootDone = true;
        }
      }

      bool WriteToken(const char* text, size_t length) {
        if (!BeginValue()) {
          return false;
        }
        if (!Raw(text, length)) {
          return false;
        }
        EndValue();
        return true;
      }

      /**
       * @brief NaN / Infinity。`AllowNaNInf` が無ければ `WriteInvalidValue` (R1-12)
       * @note  ここで assert しないのは、**呼び出し順序の誤用ではなくデータ依存**
       *        だからである。物理演算のバグで float に NaN が乗るのは現実に起こり、
       *        R1-12 自身が「セーブ経路では書き込み前のサニタイズを推奨」と
       *        書いているとおり、到達し得る入力として扱うべきものである
       */
      bool WriteNonFinite(const char* text, size_t length) {
        if (m_error != ErrorCode::None) {
          return false;
        }
        if (!HasFlag(m_flags, WriteFlags::AllowNaNInf)) {
          m_error = ErrorCode::WriteInvalidValue;
          return false;
        }
        return WriteToken(text, length);
      }

      bool ContainerBegin(bool isObject) {
        if (!BeginValue()) {
          return false;
        }
        if (m_depth >= kMaxDepth) {
          // 深さも入力側の性質なので assert しない。
          // JsonReader が DepthLimitExceeded をエラーコードで返すのと揃えてある
          if (m_error == ErrorCode::None) {
            m_error = ErrorCode::DepthLimitExceeded;
          }
          return false;
        }
        if (!Raw(isObject ? '{' : '[')) {
          return false;
        }
        SetIsObjectAt(m_depth, isObject);
        ClearHasElementAt(m_depth);
        ++m_depth;
        return true;
      }

      bool ContainerEnd(bool isObject) {
        if (m_error != ErrorCode::None) {
          return false;
        }
        if (m_depth == 0 || IsObjectAt(m_depth - 1) != isObject || m_keyWritten) {
          // 閉じ過ぎ / 括弧の不一致 / キーの後に値なしで閉じた
          return Misuse();
        }

        const std::uint32_t level = m_depth - 1;
        const bool          empty = !HasElementAt(level);
        --m_depth;

        // 空のコンテナは 1 行に潰す ({} / [])
        if (!empty && !NewlineAndIndent(m_depth)) {
          return false;
        }
        if (!Raw(isObject ? '}' : ']')) {
          return false;
        }
        EndValue();
        return true;
      }

      // --- 文字列 ---

      bool WriteQuoted(StringView text);

      /// `\uXXXX` を 1 つ出力する
      bool WriteUnicodeEscape(std::uint32_t codepoint) {
        static constexpr char kHex[] = "0123456789abcdef";
        char escape[6];
        escape[0] = '\\';
        escape[1] = 'u';
        escape[2] = kHex[(codepoint >> 12) & 0xFu];
        escape[3] = kHex[(codepoint >> 8) & 0xFu];
        escape[4] = kHex[(codepoint >> 4) & 0xFu];
        escape[5] = kHex[codepoint & 0xFu];
        return Raw(escape, 6);
      }

      S*         m_stream = nullptr;
      WriteFlags m_flags  = WriteFlags::None;
      ErrorCode  m_error  = ErrorCode::None;

      std::uint32_t m_depth        = 0;
      bool          m_keyWritten   = false;
      bool          m_rootDone     = false;

      char          m_indentChar   = ' ';
      std::uint32_t m_indentCount  = 2;

      std::uint64_t m_isObject[kMaxDepth / 64]   = {};
      std::uint64_t m_hasElement[kMaxDepth / 64] = {};
    };

  }

  /**
   * @brief 余白を入れない JSON ライタ(セーブデータ向け)
   * @note  `JsonPrettyWriter` とは**継承関係になく**、共通実装を整形方針で
   *        パラメータ化したもの。派生側は何も再定義しないため、
   *        非仮想メソッドが隠れて整形が効かなくなる事故は起きない
   */
  template <JsonOutputStream S>
  class JsonWriter final : public Detail::JsonWriterImpl<S, Detail::CompactFormat> {
  public:
    explicit JsonWriter(S& stream, WriteFlags flags = WriteFlags::None) noexcept
      : Detail::JsonWriterImpl<S, Detail::CompactFormat>(stream, flags) {
    }
  };

  /**
   * @brief 整形して書き出す JSON ライタ(エディタ出力向け)
   *
   * @details
   *  ## 整形仕様(バイト列としてテストで固定してある)
   *  | 項目 | 仕様 |
   *  |---|---|
   *  | インデント | スペース 2 個(`SetIndent` で変更可) |
   *  | 改行 | **`\n` のみ**。`\r\n` は選べない |
   *  | `:` の後 | 空白 1 個 |
   *  | `,` の後 | 改行 |
   *  | 空の `{}` / `[]` | **1 行に潰す** |
   *  | 末尾の改行 | **付けない** |
   *
   *  `\r\n` を選べなくしているのは、git の diff 安定性(R3-3 と同じ動機)と、
   *  選択肢を残すと round-trip テストのバイト列固定が二重になるため。
   *  末尾に改行を付けないのは、出力がちょうど JSON 値そのものになるようにするため
   *  (ファイル末尾の改行が要るなら呼び出し側で足す)。
   */
  template <JsonOutputStream S>
  class JsonPrettyWriter final : public Detail::JsonWriterImpl<S, Detail::PrettyFormat> {
  public:
    explicit JsonPrettyWriter(S& stream, WriteFlags flags = WriteFlags::None) noexcept
      : Detail::JsonWriterImpl<S, Detail::PrettyFormat>(stream, flags) {
    }

    /// インデント 1 段ぶんの文字と個数。既定はスペース 2 個
    void SetIndent(char character, std::uint32_t count) noexcept {
      this->SetIndentInternal(character, count);
    }
  };

  /**
   * @brief Writer を SAX ハンドラとして使うためのアダプタ (R1-15)
   *
   * @details
   *  `JsonReader` の出力をそのまま Writer へ流し込み、DOM を経由せずに
   *  minify / 整形を行うための薄いラッパ。`JsonHandler` を満たす。
   *  このヘッダは `JsonReader.h` を include しない(Layer 1 内での相互依存を作らない)
   *  ので、`JsonHandler` を満たすことの確認は利用側で行う。
   */
  template <class W>
  class JsonWriterHandler final {
  public:
    explicit JsonWriterHandler(W& writer) noexcept : m_writer(&writer) {}

    bool OnNull()                    { return m_writer->Null(); }
    bool OnBool(bool v)              { return m_writer->Bool(v); }
    bool OnInt64(std::int64_t v)     { return m_writer->Int64(v); }
    bool OnUInt64(std::uint64_t v)   { return m_writer->UInt64(v); }
    bool OnDouble(double v)          { return m_writer->Double(v); }
    bool OnString(StringView s, bool){ return m_writer->String(s); }
    bool OnKey(StringView s, bool)   { return m_writer->Key(s); }
    bool OnObjectBegin()             { return m_writer->ObjectBegin(); }
    bool OnObjectEnd(std::uint32_t)  { return m_writer->ObjectEnd(); }
    bool OnArrayBegin()              { return m_writer->ArrayBegin(); }
    bool OnArrayEnd(std::uint32_t)   { return m_writer->ArrayEnd(); }

  private:
    W* m_writer = nullptr;
  };

  // ---------------------------------------------------------------------------
  // 文字列のエスケープ
  // ---------------------------------------------------------------------------

  namespace Detail {

    template <JsonOutputStream S, class Format>
    bool JsonWriterImpl<S, Format>::WriteQuoted(StringView text) {
      if (!Raw('"')) {
        return false;
      }

      const bool  escapeNonAscii = HasFlag(m_flags, WriteFlags::EscapeNonAscii);
      const char* const begin    = text.Data();
      const char* const end      = begin + text.Size();

      // エスケープ不要な区間はまとめて Write する。
      // 大半の文字列はエスケープを一切含まないので「走査 1 回 + Write 1 回」で済む
      const char* runStart = begin;
      const char* cursor   = begin;

      const auto flushRun = [&]() -> bool {
        if (cursor == runStart) {
          return true;
        }
        const bool ok = Raw(runStart, static_cast<size_t>(cursor - runStart));
        runStart = cursor;
        return ok;
      };

      while (cursor < end) {
        const auto byte = static_cast<unsigned char>(*cursor);

        // 既定では 0x80 以上を透過するので、日本語を含む文字列でも区間が長く伸びる
        if (byte >= 0x20u && byte != '"' && byte != '\\'
            && (byte < 0x80u || !escapeNonAscii)) {
          ++cursor;
          continue;
        }

        if (!flushRun()) {
          return false;
        }

        if (byte == '"')  { if (!Raw("\\\"", 2)) { return false; } ++cursor; runStart = cursor; continue; }
        if (byte == '\\') { if (!Raw("\\\\", 2)) { return false; } ++cursor; runStart = cursor; continue; }

        if (byte < 0x20u) {
          // 短縮形があるものは短縮形で出す(人が読む config / エディタ出力で効く)
          const char* shortForm = nullptr;
          switch (byte) {
          case 0x08u: shortForm = "\\b"; break;
          case 0x09u: shortForm = "\\t"; break;
          case 0x0Au: shortForm = "\\n"; break;
          case 0x0Cu: shortForm = "\\f"; break;
          case 0x0Du: shortForm = "\\r"; break;
          default: break;
          }
          if (shortForm != nullptr) {
            if (!Raw(shortForm, 2)) { return false; }
          }
          else if (!WriteUnicodeEscape(byte)) {
            return false;
          }
          ++cursor;
          runStart = cursor;
          continue;
        }

        // ここへ来るのは escapeNonAscii かつ 0x80 以上のときだけ
        std::uint32_t      codepoint = 0;
        const std::uint32_t consumed = DecodeUtf8(cursor, end, codepoint);
        if (consumed == 0) {
          // 壊れたバイト列を U+FFFD へ黙って置き換えるとデータが静かに変質する。
          // 「純 ASCII で出す」という指定に対して勝手に別の文字へ変えるのは
          // Writer の裁量を超えるため、エラーにして呼び出し側へ返す。
          //
          // ここで assert しないのは、これが**呼び出し順序の誤用ではなくデータ依存**
          // だからである (R1-14 の対象外)。JsonReader が不正 UTF-8 を
          // StringInvalidUtf8 で返して assert しないのと揃えてある。
          // データ由来の失敗で Debug と Release の挙動を変えない
          if (m_error == ErrorCode::None) {
            m_error = ErrorCode::WriteInvalidValue;
          }
          return false;
        }

        if (codepoint < 0x10000u) {
          if (!WriteUnicodeEscape(codepoint)) { return false; }
        }
        else {
          // BMP 外はサロゲートペアへ再分解する
          const std::uint32_t adjusted = codepoint - 0x10000u;
          if (!WriteUnicodeEscape(0xD800u + (adjusted >> 10))
              || !WriteUnicodeEscape(0xDC00u + (adjusted & 0x3FFu))) {
            return false;
          }
        }
        cursor += consumed;
        runStart = cursor;
      }

      if (!flushRun()) {
        return false;
      }
      return Raw('"');
    }

  }

}
