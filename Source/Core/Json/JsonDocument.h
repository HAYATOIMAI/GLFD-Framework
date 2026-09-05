#pragma once

/**
 * @file  JsonDocument.h
 * @brief DOM ビルダと Document(Layer 2)
 *
 * @details
 *  要件書 §4.2 に対応する。
 *
 *  ## 2 本のアリーナを持つ理由(§5 論点 1 の確定内容)
 *  配列 / オブジェクトは `OnArrayEnd(n)` / `OnObjectEnd(n)` が来るまで要素数が
 *  確定しないが、`Value` は要素を**連続領域**で保持する。したがって構築中の要素を
 *  積むスタックが要る。
 *
 *  ここで `JsonArena` が個別解放できない性質が効く。構築スタックと確定した配列を
 *  同じアリーナで交互に確保すると両者が interleave し、スタックが連続領域として
 *  伸長できない。そこで:
 *
 *  - `m_arena`   — 確定した `Value` / 文字列。**Document と同じ寿命**
 *  - `m_scratch` — 構築スタック専用。**`Parse` の完了時に `Reset()`**
 *                  (ブロックは保持して次回のパースで再利用し、`Release()` はしない)
 *
 *  スタックの伸長で捨てられる旧領域は `m_scratch` 上に溜まるが、
 *  `Reset()` で毎回まとめて回収される。`m_arena` 1 本だと同じ無駄が
 *  Document の寿命ぶん残り続ける。
 */

#include <cassert>
#include <cstdint>
#include <cstring>

#include "Core/StringView.h"
#include "Core/Json/JsonArena.h"
#include "Core/Json/JsonReader.h"
#include "Core/Json/JsonTypes.h"
#include "Core/Json/JsonValue.h"
#include "Core/Json/JsonWriter.h"

namespace GLFD::Json {

  namespace Detail {

    /**
     * @brief アリーナ上に置く、連続領域を保つ伸長可能スタック
     * @note  伸長時の旧領域はアリーナに放棄されるが、scratch アリーナは
     *        パースごとに Reset されるので蓄積しない
     */
    template <class T>
    class ScratchStack {
    public:
      /// 保持しているポインタを捨てる。scratch を Reset した直後に必ず呼ぶこと
      void Clear() noexcept {
        m_data     = nullptr;
        m_size     = 0;
        m_capacity = 0;
      }

      [[nodiscard]] std::uint32_t Size() const noexcept { return m_size; }
      [[nodiscard]] T*            Data() const noexcept { return m_data; }

      /// base の位置まで巻き戻す(要素は破棄しない = trivially destructible 前提)
      void Rewind(std::uint32_t base) noexcept {
        assert(base <= m_size);
        m_size = base;
      }

      [[nodiscard]] bool Push(const T& value, JsonArena& arena) noexcept {
        if (m_size == m_capacity && !Grow(arena)) {
          return false;
        }
        m_data[m_size] = value;
        ++m_size;
        return true;
      }

    private:
      [[nodiscard]] bool Grow(JsonArena& arena) noexcept {
        constexpr std::uint32_t kInitial = 32;
        std::uint32_t next = (m_capacity == 0) ? kInitial : m_capacity * 2u;
        if (m_capacity != 0 && m_capacity > (0xFFFFFFFFu / 2u)) {
          return false;
        }
        T* const buffer = arena.AllocateArray<T>(next);
        if (buffer == nullptr) {
          return false;
        }
        if (m_size != 0) {
          std::memcpy(buffer, m_data, static_cast<size_t>(m_size) * sizeof(T));
        }
        m_data     = buffer;
        m_capacity = next;
        return true;
      }

      T*            m_data     = nullptr;
      std::uint32_t m_size     = 0;
      std::uint32_t m_capacity = 0;
    };

  }

  /**
   * @brief SAX イベントから `Value` 木を構築するハンドラ
   *
   * @details
   *  `Document` の内部実装だが、SAX を直接使いたい場面でも再利用できるよう
   *  ヘッダに置いてある (R2-6)。`JsonHandler` を満たす。
   *
   *  ## 文字列のコピー規約(1-2 で確定・厳守)
   *  | `needsCopy` | 意味 | ここでの動作 |
   *  |---|---|---|
   *  | `true`  | **入力バッファ**を直接指すスライス | **`m_arena` へコピーする** |
   *  | `false` | **アリーナ上**のアンエスケープ済み文字列 | そのまま保持 |
   *
   *  入力バッファは `Document` より短命であり得るため、`true` の側を
   *  コピーしないと入力解放後に全文字列がダングリングする。
   *
   *  ## エラーの伝え方
   *  確保に失敗したときは `false` を返してパースを中断する。`JsonReader` は
   *  これを `HandlerAborted` として返すので、呼び出し側は `Error()` を見て
   *  `OutOfMemory` 等の実際の理由へ置き換えること。
   */
  class DomBuilder final {
  public:
    /**
     * @param arena   確定した Value / 文字列の置き場(Document と同じ寿命)
     * @param scratch 構築スタックの置き場(パースごとに Reset される)
     */
    DomBuilder(JsonArena& arena, JsonArena& scratch) noexcept
      : m_arena(&arena)
      , m_scratch(&scratch) {
    }

    /**
     * @brief 構築状態を初期化する
     * @note  scratch アリーナを Reset した**直後**に必ず呼ぶこと。
     *        スタックが保持している古いポインタを捨てるため
     */
    void Begin() noexcept {
      m_values.Clear();
      m_members.Clear();
      m_frames.Clear();
      m_root       = Value{};
      m_pendingKey = StringView();
      m_error      = ErrorCode::None;
    }

    [[nodiscard]] const Value& Root()  const noexcept { return m_root; }
    [[nodiscard]] ErrorCode    Error() const noexcept { return m_error; }

    // --- JsonHandler の実装 ---

    bool OnNull() {
      return AcceptScalar(Value{});
    }

    bool OnBool(bool v) {
      Value value;
      value.SetBool(v);
      return AcceptScalar(value);
    }

    bool OnInt64(std::int64_t v) {
      Value value;
      value.SetInt64(v);
      return AcceptScalar(value);
    }

    bool OnUInt64(std::uint64_t v) {
      Value value;
      value.SetUInt64(v);
      return AcceptScalar(value);
    }

    bool OnDouble(double v) {
      Value value;
      value.SetDouble(v);
      return AcceptScalar(value);
    }

    bool OnString(StringView s, bool needsCopy) {
      Value value;
      if (!StoreString(value, s, needsCopy)) {
        return false;
      }
      return AcceptScalar(value);
    }

    bool OnKey(StringView s, bool needsCopy) {
      StringView key;
      if (!AdoptText(key, s, needsCopy)) {
        return false;
      }
      m_pendingKey = key;
      return true;
    }

    bool OnObjectBegin() { return PushFrame(true); }
    bool OnArrayBegin()  { return PushFrame(false); }

    bool OnObjectEnd(std::uint32_t count) {
      assert(m_frames.Size() > 0);
      const Frame frame = m_frames.Data()[m_frames.Size() - 1];
      m_frames.Rewind(m_frames.Size() - 1);
      assert(frame.isObject);
      assert(m_members.Size() - frame.base == count);

      Value object;
      if (!object.AssignObject(m_members.Data() + frame.base, count, *m_arena)) {
        m_error = ErrorCode::OutOfMemory;
        return false;
      }
      m_members.Rewind(frame.base);
      // このコンテナが親オブジェクトへ収まるときのキーは、開いた時点で
      // フレームへ退避してある。m_pendingKey は内側のオブジェクトに
      // 上書きされている可能性があるので使ってはならない
      return AcceptValue(object, frame.key);
    }

    bool OnArrayEnd(std::uint32_t count) {
      assert(m_frames.Size() > 0);
      const Frame frame = m_frames.Data()[m_frames.Size() - 1];
      m_frames.Rewind(m_frames.Size() - 1);
      assert(!frame.isObject);
      assert(m_values.Size() - frame.base == count);

      Value array;
      if (!array.AssignArray(m_values.Data() + frame.base, count, *m_arena)) {
        m_error = ErrorCode::OutOfMemory;
        return false;
      }
      m_values.Rewind(frame.base);
      return AcceptValue(array, frame.key);
    }

  private:
    /// 構築中のコンテナ 1 段
    struct Frame {
      /**
       * @brief 親がオブジェクトのとき、このコンテナが収まるキー
       * @note  コンテナを開いた時点の保留キーをここへ退避する。
       *        `{"a":[1,{"b":2}]}` のように内側でさらに OnKey が来ると
       *        単一の保留キーでは上書きされてしまうため、フレームごとに持つ
       */
      StringView    key;
      std::uint32_t base;      ///< 対応するスタックの開始位置
      bool          isObject;
    };

    /// 入力バッファ由来なら m_arena へ複製し、アリーナ由来ならそのまま採用する
    [[nodiscard]] bool AdoptText(StringView& out, StringView text, bool needsCopy) noexcept {
      if (!needsCopy) {
        out = text;               // すでに m_arena 上にある(アンエスケープ済み)
        return true;
      }
      if (text.Empty()) {
        out = StringView();
        return true;
      }
      char* const buffer = m_arena->AllocateArray<char>(text.Size());
      if (buffer == nullptr) {
        m_error = ErrorCode::OutOfMemory;
        return false;
      }
      std::memcpy(buffer, text.Data(), text.Size());
      out = StringView(buffer, text.Size());
      return true;
    }

    [[nodiscard]] bool StoreString(Value& out, StringView text, bool needsCopy) noexcept {
      StringView owned;
      if (!AdoptText(owned, text, needsCopy)) {
        return false;
      }
      if (!out.SetStringRef(owned)) {
        m_error = ErrorCode::OutOfMemory;
        return false;
      }
      return true;
    }

    [[nodiscard]] bool PushFrame(bool isObject) noexcept {
      Frame frame{};
      frame.key      = m_pendingKey;   // このコンテナが親へ収まるときのキー
      frame.isObject = isObject;
      frame.base     = isObject ? m_members.Size() : m_values.Size();
      m_pendingKey   = StringView();   // 内側の OnKey と混ざらないよう手放す
      if (!m_frames.Push(frame, *m_scratch)) {
        m_error = ErrorCode::OutOfMemory;
        return false;
      }
      return true;
    }

    /// スカラを収める。保留キーを消費する
    [[nodiscard]] bool AcceptScalar(const Value& value) noexcept {
      const StringView key = m_pendingKey;
      m_pendingKey = StringView();
      return AcceptValue(value, key);
    }

    /**
     * @brief 完成した値を、現在のコンテナ(無ければルート)へ収める
     * @param key 親がオブジェクトのときに使うキー。配列・ルートでは無視される
     */
    [[nodiscard]] bool AcceptValue(const Value& value, StringView key) noexcept {
      if (m_frames.Size() == 0) {
        m_root = value;
        return true;
      }

      const Frame& top = m_frames.Data()[m_frames.Size() - 1];
      if (top.isObject) {
        Member member;
        member.key   = key;
        member.value = value;
        if (!m_members.Push(member, *m_scratch)) {
          m_error = ErrorCode::OutOfMemory;
          return false;
        }
        return true;
      }

      if (!m_values.Push(value, *m_scratch)) {
        m_error = ErrorCode::OutOfMemory;
        return false;
      }
      return true;
    }

    JsonArena* m_arena   = nullptr;
    JsonArena* m_scratch = nullptr;

    Detail::ScratchStack<Value>  m_values;    ///< 配列要素の積み場
    Detail::ScratchStack<Member> m_members;   ///< オブジェクトメンバの積み場
    Detail::ScratchStack<Frame>  m_frames;    ///< 構築中のコンテナ

    Value      m_root;
    StringView m_pendingKey;
    ErrorCode  m_error = ErrorCode::None;
  };

  static_assert(JsonHandler<DomBuilder>, "DomBuilder must satisfy JsonHandler");

  /**
   * @brief DOM を走査して Writer へ流す
   *
   * @details
   *  **自由関数テンプレートである理由**: 要件書 §3.6 は
   *  `JsonPrettyWriter : public JsonWriter<S>` と書いているが、メソッドは仮想ではないので、
   *  これを基底クラスのメンバとして実装すると**基底の実装に解決され整形が一切効かない**。
   *  自由関数なら渡された Writer の実型に解決されるため、この事故が構造的に起きない。
   *
   *  また、この関数を Layer 2 (`JsonDocument.h`) に置くことで、
   *  `JsonWriter.h`(Layer 1)が `Value` を知らずに済む。SAX 直結で minify したい
   *  利用者 (R1-15) は DOM をコンパイルしなくてよい。
   *
   *  **メンバ順・要素順は DOM のまま保たれる**(R3-3 の diff 安定性の土台)。
   *
   * @note  再帰するが、深さは Writer の `kMaxDepth`(256)で頭打ちになる。
   *        それを超えると `ArrayBegin` / `ObjectBegin` が失敗して巻き戻るので、
   *        ネイティブスタックの消費は 257 フレームで有界である
   */
  template <class W>
  [[nodiscard]] bool WriteValue(W& writer, const Value& value) {
    switch (value.Type()) {
    case ValueType::Null:   return writer.Null();
    case ValueType::Bool:   return writer.Bool(value.GetBool());
    case ValueType::Int64:  return writer.Int64(value.GetInt64());
    case ValueType::UInt64: return writer.UInt64(value.GetUInt64());
    case ValueType::Double: return writer.Double(value.GetDouble());
    case ValueType::String: return writer.String(value.GetString());

    case ValueType::Array: {
      if (!writer.ArrayBegin()) {
        return false;
      }
      for (const Value* it = value.Begin(); it != value.End(); ++it) {
        if (!WriteValue(writer, *it)) {
          return false;
        }
      }
      return writer.ArrayEnd();
    }

    case ValueType::Object: {
      if (!writer.ObjectBegin()) {
        return false;
      }
      for (const Member* it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        if (!writer.Key(it->key) || !WriteValue(writer, it->value)) {
          return false;
        }
      }
      return writer.ObjectEnd();
    }

    default:
      assert(false && "WriteValue: unknown ValueType");
      return false;
    }
  }

  /**
   * @brief JSON ドキュメント。パースした DOM とその記憶域を所有する
   *
   * @details
   *  ## 生存期間の契約(R0-5)
   *  `Root()` から辿れる全ての `Value` / `Member` / `StringView` は、
   *  **この Document が所有するアリーナ上にある**。したがって
   *  `Clear()` / `Parse()` の再実行 / デストラクタ / ムーブ代入のいずれかが走ると、
   *  それ以前に取得した参照はすべて無効になる。
   *
   *  ## スレッド安全性
   *  単一スレッド前提。別インスタンス同士は並列に使える (N-3)。
   */
  class Document final {
  public:
    /// 構築スタック用アリーナの既定ブロックサイズ
    static constexpr size_t kDefaultScratchBlockSize = 16 * 1024;

    explicit Document(Memory::IMemoryResource* resource,
                      size_t arenaBlockSize   = JsonArena::kDefaultBlockSize,
                      size_t scratchBlockSize = kDefaultScratchBlockSize) noexcept
      : m_arena(resource, arenaBlockSize)
      , m_scratch(resource, scratchBlockSize) {
    }

    ~Document() = default;

    Document(Document&&) noexcept            = default;
    Document& operator=(Document&&) noexcept = default;
    Document(const Document&)                = delete;
    Document& operator=(const Document&)     = delete;

    /**
     * @brief JSON をパースして DOM を構築する
     * @note  **入力バッファは呼び出し中だけ有効であればよい。** エスケープを
     *        含まない文字列は入力を直接指すスライスとして届くが、DOM ビルダが
     *        アリーナへ複製するため、パース後に入力を破棄してよい
     */
    [[nodiscard]] ParseError Parse(StringView json, ParseFlags flags = ParseFlags::None);

    /**
     * @brief ファイル全体を読み込んでパースする
     * @param utf8Path **UTF-8 かつヌル終端**のパス。`StringView` を取らないのは
     *                 ヌル終端が保証されないため (R0-8)
     *
     * @note  ファイル内容は **`m_arena` 上**へ読み込む。パース後、
     *        `needsCopy == true` だった文字列はこの領域を指したままになるが、
     *        `m_arena` は Document と生存期間が一致するので安全である。
     *        **この置き場を `m_scratch` へ移すと全文字列がダングリングする。**
     */
    [[nodiscard]] ParseError ParseFile(const char* utf8Path,
                                       ParseFlags flags = ParseFlags::None);

    [[nodiscard]] bool       HasError() const noexcept { return !m_error.IsOk(); }
    [[nodiscard]] ParseError Error()    const noexcept { return m_error; }

    /// パース失敗時は Null を返す(部分構築された DOM を露出しない / R2-7)
    [[nodiscard]] const Value& Root() const noexcept { return m_root; }
    [[nodiscard]] Value&       Root()       noexcept { return m_root; }

    /**
     * @brief 診断パスで DOM を引く (2-1)
     *
     * @param path `ArchiveIssue::path` と同じ `"window/width"` 形式 (R3-9)。
     *             **空ならルート自身**を返す
     * @return 見つかった値。無ければ `nullptr`(`assert` しない)
     *
     * @details
     *  ログに出た診断パスを**そのまま貼れる**ことが存在理由である。
     *  解決規則・厳格な添字解釈・引けないキーの一覧は `JsonQuery.h` を参照。
     *  確保は一切行わない。
     *
     *  @note ここは `Json::Query(Root(), path)` への転送でしかない。実体を
     *        `JsonDocument.cpp` に置いてあるのは、**このヘッダから
     *        `JsonQuery.h` を include しない**ため。`"/"` 区切りは R3-9
     *        (Layer 3 の診断)の規約であり、Layer 2 の DOM が抱える形に
     *        したくない
     *
     *  @warning 返り値の寿命はこの `Document` に従う (R0-5)。`Clear()` /
     *           `Parse()` の再実行 / 破棄のいずれかで無効になる
     */
    [[nodiscard]] const Value* Query(StringView path) const noexcept;

    /**
     * @brief パース元のテキスト。`ComputeLocation` に渡して `line:column` を出す (2-4)
     *
     * @return `ParseFile` で読んだ場合はその内容。**`Parse(StringView)` の場合は空**
     *
     * @details
     *  返すのは**この `Document` がアリーナ上に所有しているテキストだけ**である。
     *  `Parse(StringView)` の入力は「呼び出し中だけ有効であればよい」という契約
     *  (このヘッダの `Parse` を参照)なので、保持すると `Document` より短命な
     *  バッファを指すダングリングを作る。その経路の呼び出し側は元テキストを
     *  手に持っているため、`ComputeLocation` に自分で渡せばよい。
     *
     *  **失敗時こそ有効である。** R2-7 が禁じているのは部分構築の DOM の露出で、
     *  ソーステキストはその対象ではない。`line:column` が要るのは失敗時である。
     *
     *  ## 空が返る場合の扱い(**後から `Parse` 版を使う人が踏む**)
     *  `Parse(StringView)` で読んだ `Document` の `Source()` は**常に空**である。
     *  これを `ComputeLocation` へそのまま渡すと、`offset` が何であっても
     *  **`{ line = 1, column = 1 }` が返る**(走査する入力が無いため)。
     *  つまり**誤った位置が静かに出る**。位置を出す側は次のどちらかにすること。
     *
     *   - 空かどうかを見て、空なら位置を出さない
     *     (`Json::FormatParseError` はこれを行い、`file: Code` に落とす)
     *   - `Parse` に渡した元テキストを呼び出し側が保持し、それを渡す
     *
     *  @warning 寿命はアリーナに従う (R3-34)。`Clear()` / 再 `Parse()` / 破棄の
     *           いずれかで無効になる。**保存してはならない** — 診断を出すその場で
     *           使い切ること(`ReloadGameConfig` の `onIssues` の中は安全な位置)
     */
    [[nodiscard]] StringView Source() const noexcept { return m_source; }

    /**
     * @brief 両アリーナを `Reset()` し、ルートを Null に戻す
     * @note  `Release()` ではなく `Reset()` なので、直後に `Parse` しても
     *        `IMemoryResource` への追加要求は発生しない。
     *        **それ以前に取得した全ての参照が無効になる。**
     */
    void Clear() noexcept;

    [[nodiscard]] JsonArena& Arena()   noexcept { return m_arena; }
    [[nodiscard]] JsonArena& Scratch() noexcept { return m_scratch; }

    void SetMaxDepth(std::uint32_t depth) noexcept { m_maxDepth = depth; }
    [[nodiscard]] std::uint32_t MaxDepth() const noexcept { return m_maxDepth; }

    /**
     * @brief 余白を入れずに書き出す(セーブデータ向け)
     * @return 全て書き切れたら true。ストリームが失敗した場合などは false
     * @note   出力先に `JsonStringBuffer` を使う場合、**`Scratch()` を渡すのが自然**。
     *         出力は一時データであり、`Arena()` に置くと書き出すたびに Document の
     *         メモリが恒久的に増える。scratch なら次の `Parse()` / `Clear()` で
     *         回収されるが、その帰結として **`View()` の有効期間もそこまで**になる
     */
    template <JsonOutputStream S>
    [[nodiscard]] bool WriteTo(S& stream, WriteFlags flags = WriteFlags::None) const {
      JsonWriter<S> writer(stream, flags);
      if (!WriteValue(writer, m_root)) {
        return false;
      }
      assert(writer.IsComplete() && "Document::WriteTo: writer left the document unclosed");
      return writer.IsComplete() && stream.Flush();
    }

    /// 整形して書き出す(エディタ出力向け)。整形仕様は `JsonPrettyWriter` を参照
    template <JsonOutputStream S>
    [[nodiscard]] bool WritePrettyTo(S& stream, WriteFlags flags = WriteFlags::None) const {
      JsonPrettyWriter<S> writer(stream, flags);
      if (!WriteValue(writer, m_root)) {
        return false;
      }
      assert(writer.IsComplete() && "Document::WritePrettyTo: writer left the document unclosed");
      return writer.IsComplete() && stream.Flush();
    }

  private:
    /// Clear() を伴わないパース本体。ParseFile が読み込み後に使う
    [[nodiscard]] ParseError ParseInPlace(StringView json, ParseFlags flags);

    JsonArena     m_arena;
    JsonArena     m_scratch;
    Value         m_root;
    StringView    m_source;   ///< ParseFile が読んだテキスト (2-4)
    ParseError    m_error;
    std::uint32_t m_maxDepth = JsonReader::kDefaultMaxDepth;
  };

}
