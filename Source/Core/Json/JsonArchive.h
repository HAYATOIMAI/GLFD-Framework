#pragma once

/**
 * @file  JsonArchive.h
 * @brief Layer 3 統一アーカイブの共通基盤 (R3-16)
 *
 * @details
 *  `ArchiveContext` / `ArchiveIssue` / `ArchiveFlags` / パス追跡 / `Serialize`
 *  ディスパッチを収める。**`ReadArchive` も `WriteArchive` も知らない。**
 *
 *  依存は `JsonTypes.h` / `StringView.h` / `JsonArena.h` に限る。
 *  `JsonArena.h` は §5.3 の `ArchiveContext(JsonArena&, ...)` という宣言そのものが
 *  要求しており、`Core/MemoryResource.h` 以外を引き込まないため層を汚さない。
 *  **`JsonReader.h` / `JsonValue.h` / `JsonDocument.h` / `JsonWriter.h` は含めない。**
 *  セーブだけを行う利用者に Reader と DOM をコンパイルさせないためであり、
 *  これは T-23 で `/showIncludes` により機械的に検証する。
 *
 *  @note R3-10: `$` で始まるキーは予約名前空間である。`"$version"` が現在の唯一の
 *        予約キー。**ユーザー型のフィールド名に `$` 始まりを使ってはならない。**
 *        Debug ビルドでは `Member` がこれを `assert` で検出する。
 */

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Core/StringView.h"
#include "Core/Json/JsonArena.h"
#include "Core/Json/JsonTypes.h"

namespace GLFD::Json {

  // ---------------------------------------------------------------------------
  // 診断
  // ---------------------------------------------------------------------------

  /// Issue の種別 (§5.3)
  enum class ArchiveErrorKind : std::uint8_t {
    MissingRequired,      ///< RequiredMember が見つからない (常に Fatal)
    TypeMismatch,         ///< 型が期待と異なる (StrictTypes 指定時のみ Fatal)
    RangeOverflow,        ///< 数値が対象型の範囲外。切り詰めは行わない (R3-11)
    UnknownField,         ///< 未知フィールド。ReportUnknown 指定時のみ記録する (R3-5)
    ContainerFailure,     ///< コンテナの確保失敗 (常に Fatal)

    /**
     * 配列の要素数が期待と違う【1-6 で追加】。Fatal ではない。
     *
     * `TypeMismatch` に相乗りさせない理由は、調査先が違うこと
     * (要素数は JSON 側を直す話、確保失敗はメモリ側を疑う話) に加えて、
     * **`StrictTypes` を立てた瞬間に要素数不一致まで Fatal になってしまう**
     * ためである。「型には厳しく、長さには寛容に」を選べなくなり、
     * フラグ 1 つに 2 つの方針が束ねられる。R1-1f が避けたい形そのもの。
     * したがって本 kind は `StrictTypes` の支配下にも置かない。
     */
    ArrayLengthMismatch,
  };

  [[nodiscard]] constexpr const char* ToString(ArchiveErrorKind kind) noexcept {
    switch (kind) {
      case ArchiveErrorKind::MissingRequired:     return "MissingRequired";
      case ArchiveErrorKind::TypeMismatch:        return "TypeMismatch";
      case ArchiveErrorKind::RangeOverflow:       return "RangeOverflow";
      case ArchiveErrorKind::UnknownField:        return "UnknownField";
      case ArchiveErrorKind::ContainerFailure:    return "ContainerFailure";
      case ArchiveErrorKind::ArrayLengthMismatch: return "ArrayLengthMismatch";
    }
    return "<unknown>";
  }

  /**
   * @brief 1 件の診断
   *
   * @note `path` と `detail` は**ヌル終端されている**。ログ出力で `%s` に渡せることを
   *       契約とする (`StringView` 自体はヌル終端を保証しない型だが、この 2 つは
   *       診断専用でありログに流すのが唯一の用途のため、あえて保証する)。
   *       `path` は `ArchiveContext` のアリーナ上に構築され、コンテキストと同じ寿命を持つ。
   *       `detail` は静的な文字列リテラルを指す。
   */
  struct ArchiveIssue {
    ArchiveErrorKind kind{ ArchiveErrorKind::TypeMismatch };
    StringView       path;     ///< "player/stats/hp" 形式。ルートは空
    StringView       detail;   ///< 短い説明。整形は行わない
  };

  /// アーカイブの挙動フラグ (§5.3)
  enum class ArchiveFlags : std::uint32_t {
    None          = 0,
    SkipDefaults  = 1u << 0,   ///< 書き: デフォルト値と一致するメンバを出力しない
    ReportUnknown = 1u << 1,   ///< 読み: 未知フィールドを Issue として記録する
    StrictTypes   = 1u << 2,   ///< 読み: 型不一致を Fatal 扱いにする

    /**
     * 読み: **上書き読み** (2-3)。同じ構造体へ 2 回目以降を読むときに立てる。
     *
     * `.local.json` のようなレイヤ設定は「書いてあるフィールドだけを上書きする」
     * 意味論を持つ。既定値なしの `Member` はキーが無ければ値に触らない (R3-4) ので
     * 素直に成立するが、**次の 2 つだけが邪魔をする**。このフラグはその 2 つを
     * 抑えるためだけに存在する。
     *
     *  - 既定値つき `Member`: キーが無いと `v = def` で **1 回目の値を潰す**
     *  - `RequiredMember`:    キーが無いと `MissingRequired` で Fatal になる
     *
     * **診断は抑えない。** `UnknownField` は `.local.json` の綴り間違いの検出
     * そのものであり、`TypeMismatch` / `RangeOverflow` は上書きに失敗したことを
     * 知らせる。抑えるのは「無いのが正常」な 2 つだけである。
     *
     * @note 読み専用のフラグである。`WriteArchive` は `SkipDefaults` しか見ないため
     *       書き側で立てても何も起きない
     */
    Overlay       = 1u << 3,
  };

  template <> struct IsFlagEnum<ArchiveFlags> : std::true_type {};

  /**
   * @brief その種別を「読めなかった」として扱うか
   *
   * @details
   *  `HasFatal()` は「1 件でも Fatal があるか」しか答えないため、**どの Issue が
   *  Fatal かを呼び出し側が知る手段が無かった**。診断を出す側は
   *  「起動はしたが一部おかしい」と「読めなかった」を別のレベルで出したいので、
   *  判定そのものをここへ出してある(`ArchiveContext` は本関数へ委譲する。
   *  判定を 2 箇所に書かないため)。
   *
   *  `RangeOverflow` / `UnknownField` / `ArrayLengthMismatch` は Fatal にしない。
   *  `TypeMismatch` は `StrictTypes` 指定時のみ。
   */
  [[nodiscard]] constexpr bool IsFatal(ArchiveErrorKind kind, ArchiveFlags flags) noexcept {
    switch (kind) {
      case ArchiveErrorKind::MissingRequired:
      case ArchiveErrorKind::ContainerFailure:
        return true;
      case ArchiveErrorKind::TypeMismatch:
        return HasFlag(flags, ArchiveFlags::StrictTypes);
      case ArchiveErrorKind::RangeOverflow:
      case ArchiveErrorKind::UnknownField:
      case ArchiveErrorKind::ArrayLengthMismatch:
        return false;
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // 内部の器
  // ---------------------------------------------------------------------------

  namespace Detail {

    /**
     * @brief アリーナ上の単純な伸長配列
     *
     * @details
     *  `DynamicArray` は使わない(論点7 の確定事項)。`DynamicArray.h` は
     *  `<memory>` `<stdexcept>` `<algorithm>` `<iterator>` を引き込んで R3-16 の
     *  ヘッダ分割を壊し、`Resize` などの実体化で `/EH` 無しビルドに C4530 を出す。
     *  `JsonDocument.h` の `ScratchStack` と形は似ているが、あちらへ依存すると
     *  DOM が入ってしまうため意図的に重複させている。
     *
     *  倍率 2 で伸長し、失敗時は `false`。旧領域はアリーナ上に放棄する。
     */
    template <class T>
    class ArenaVector {
    public:
      // アリーナ上の生の領域へ代入で書き込むため、要素はトリビアルコピー可能に限る。
      // 構築・破棄を持つ型は扱えない(アリーナは個別解放しないため破棄の機会が無い)
      static_assert(std::is_trivially_copyable_v<T>,
                    "ArenaVector stores elements by assignment into raw arena memory");
      static_assert(std::is_trivially_destructible_v<T>,
                    "ArenaVector never destroys its elements");

      static constexpr std::uint32_t kInitialCapacity = 8;

      [[nodiscard]] std::uint32_t Size()     const noexcept { return m_size; }
      [[nodiscard]] std::uint32_t Capacity() const noexcept { return m_capacity; }
      [[nodiscard]] const T*      Data()     const noexcept { return m_data; }
      [[nodiscard]] T*            Data()           noexcept { return m_data; }
      [[nodiscard]] bool          Empty()    const noexcept { return m_size == 0u; }

      /**
       * @brief 内容と**保持しているバッファの参照ごと**捨てる
       *
       * @details
       *  `m_size = 0` だけ戻して `m_data` を残す形にしてはならない。
       *  それは「捨てた後も同じバッファを再利用できる」という前提の上に成り立つが、
       *  **アリーナ上のバッファは所有者の知らないうちに無効化される**ため
       *  (`JsonArena::Reset()` はブロックを保持したまま先頭へ巻き戻す)、
       *  その前提が成立しない。
       *
       *  具体的には、`ArchiveContext` が `Document::Arena()` を使っている状態で
       *  `Document::Clear()` → `ArchiveContext::Clear()` → `Report()` と進むと、
       *  再配布済みの領域へ Issue を書き込み **DOM が使っているメモリを静かに壊す**。
       *  `JsonDocument.h` の `ScratchStack::Clear()` は最初から
       *  この形になっている(そちらが正しい先例)。
       */
      void Clear() noexcept {
        m_data     = nullptr;
        m_size     = 0u;
        m_capacity = 0u;
      }

      [[nodiscard]] bool Push(const T& value, JsonArena& arena) noexcept {
        if (m_size == m_capacity && !Grow(arena)) {
          return false;
        }
        m_data[m_size++] = value;
        return true;
      }

    private:
      [[nodiscard]] bool Grow(JsonArena& arena) noexcept {
        const std::uint32_t next = (m_capacity == 0u) ? kInitialCapacity : (m_capacity * 2u);
        if (next <= m_capacity) {
          return false;   // 桁溢れ
        }
        T* const buffer = arena.AllocateArray<T>(next);
        if (buffer == nullptr) {
          return false;
        }
        for (std::uint32_t i = 0; i < m_size; ++i) {
          buffer[i] = m_data[i];
        }
        m_data     = buffer;
        m_capacity = next;
        return true;
      }

      T*            m_data     = nullptr;
      std::uint32_t m_size     = 0;
      std::uint32_t m_capacity = 0;
    };

    /**
     * @brief パス追跡 (R3-9)
     *
     * @details
     *  正常系で文字列連結を一切行わない。セグメントを積むだけにしておき、
     *  Issue を記録する瞬間にだけ `Materialize` でアリーナ上へ組み立てる。
     *
     *  配列要素は `"items/3/name"` のように**添字も `/` 区切りの一要素**として表す。
     *  角括弧を使わないのは、区切りが 1 種類になって組み立てが単純になることと、
     *  フェーズ2 のパスアクセス (`doc.Query("player/stats/hp")`) にそのまま
     *  貼れる形にしておくためである。
     *
     *  段数はインライン 64 段で頭打ちにする(`JsonReader` のインライン 64 フレームと
     *  同じ考え方)。溢れた分は追跡をやめ、パス末尾に `/...` を付けて截断を示す。
     *  **診断が確保失敗で落ちる経路を作らない**ことを優先している。
     */
    class PathStack {
    public:
      /**
       * @brief 追跡する段数の上限。これを超えた分は截断され、末尾に `"/..."` が付く
       *
       * @warning **`ReadArchive::kMaxDepth` と対で見ること。** あちらはこの値から
       *          導出されており(`= Detail::PathStack::kMaxDepth`)、
       *          `ReadArchive` の枠がこの段数で尽きるため、**通常のロード経路では
       *          截断が一度も起きない**(枠が尽きた側が `ContainerFailure` を
       *          報告して降下をやめる。実測でパスはちょうど 64 段まで伸び、
       *          あと 1 push で溢れるところで止まる)。
       *          截断の分岐は死んでいるのではなく**休眠している**。
       *          `ReadArchive::kMaxDepth` に**この値より大きい独立の上限**を
       *          与えた瞬間、本番経路に現れる。導出をやめるときは、
       *          `JsonQueryTests` の T-46b(截断パスを `Query` に渡す)を
       *          実アーカイブ経由の形へ書き換えられるか検討すること
       *
       * @note **休眠を支えている本当の不変条件は定数の大小ではなく、
       *       「パスの深さが枠カウンタを超えない」ことである。**
       *       `static_assert(ReadArchive::kMaxDepth <= kMaxDepth)` はその**代理指標**で、
       *       定数を触らない変更は素通しする。押し場所は現在 5 箇所:
       *
       *       - `ReadMemberAt` の `PathScope(key)` と `Element` の `PathScope(index)` は
       *         **枠を積む直前**に押す。枠が積めなければ `Detail::ArchiveOne` は
       *         `Serialize` を呼ばず(`BeginObject` 失敗で即 return)、配列側も
       *         `BeginArray` が返す 0 を回すため、**積めなかった枠の下に押しは入らない**
       *       - `MissingRequired` / `$version` / 未知フィールドの 3 箇所は
       *         **押して報告して即座に戻す葉**であり、入れ子にならない
       *
       *       この 2 点でパスの深さは常に枠の数以下に収まる。**余裕は 0 で**、
       *       パスはちょうど上限まで伸びてから止まる(`kMaxDepth` を 8 に落として
       *       深さ 20 のデータを 5 形態流し、最深 8 セグメント・截断 0 件を実測)。
       *       したがって危険な変更は定数ではなく、**枠を伴わない `PathScope` を新設し、
       *       その中でさらに押す**形である。それを作ると定数の関係は保たれたまま
       *       截断が動き出し、`static_assert` は鳴らない。
       */
      static constexpr std::uint32_t kMaxDepth = 64;

      void PushKey(StringView key) noexcept {
        if (m_depth < kMaxDepth) {
          m_segments[m_depth].key     = key;
          m_segments[m_depth].index   = 0;
          m_segments[m_depth].isIndex = false;
          ++m_depth;
        }
        else {
          ++m_overflow;
        }
      }

      void PushIndex(std::uint32_t index) noexcept {
        if (m_depth < kMaxDepth) {
          m_segments[m_depth].key     = StringView{};
          m_segments[m_depth].index   = index;
          m_segments[m_depth].isIndex = true;
          ++m_depth;
        }
        else {
          ++m_overflow;
        }
      }

      void Pop() noexcept {
        if (m_overflow > 0u) {
          --m_overflow;
        }
        else if (m_depth > 0u) {
          --m_depth;
        }
      }

      void Reset() noexcept {
        m_depth    = 0;
        m_overflow = 0;
      }

      [[nodiscard]] std::uint32_t Depth()     const noexcept { return m_depth; }
      [[nodiscard]] bool          Truncated() const noexcept { return m_overflow > 0u; }

      /**
       * @brief 現在のパスをアリーナ上へ組み立てる
       * @return ヌル終端された文字列。確保に失敗した場合は空
       */
      [[nodiscard]] StringView Materialize(JsonArena& arena) const noexcept {
        size_t length = 0;
        for (std::uint32_t i = 0; i < m_depth; ++i) {
          if (i != 0u) { ++length; }                       // '/'
          length += m_segments[i].isIndex ? DecimalDigits(m_segments[i].index)
                                          : m_segments[i].key.Size();
        }
        if (m_overflow > 0u) {
          length += (m_depth == 0u) ? 3u : 4u;             // "..." または "/..."
        }
        if (length == 0u) {
          return StringView{};
        }

        char* const buffer = arena.AllocateArray<char>(length + 1u);
        if (buffer == nullptr) {
          return StringView{};   // 診断のためにロードを失敗させない
        }

        size_t offset = 0;
        for (std::uint32_t i = 0; i < m_depth; ++i) {
          if (i != 0u) { buffer[offset++] = '/'; }
          if (m_segments[i].isIndex) {
            offset += WriteDecimal(buffer + offset, m_segments[i].index);
          }
          else {
            const StringView key = m_segments[i].key;
            for (size_t k = 0; k < key.Size(); ++k) {
              buffer[offset++] = key[k];
            }
          }
        }
        if (m_overflow > 0u) {
          // `m_depth == 0` で截断されている状態には到達しない。溢れの記録は
          // `m_depth == kMaxDepth`(> 0)でしか増えず、`Pop` は溢れを先に減らす。
          // 純粋な防御であり、**テストを書く対象ではない**
          if (m_depth != 0u) { buffer[offset++] = '/'; }
          buffer[offset++] = '.';
          buffer[offset++] = '.';
          buffer[offset++] = '.';
        }
        buffer[offset] = '\0';
        return StringView(buffer, offset);
      }

    private:
      struct Segment {
        StringView    key;
        std::uint32_t index   = 0;
        bool          isIndex = false;
      };

      [[nodiscard]] static size_t DecimalDigits(std::uint32_t value) noexcept {
        size_t digits = 1;
        while (value >= 10u) { value /= 10u; ++digits; }
        return digits;
      }

      /// CRT の書式化を使わずに 10 進数を書く
      static size_t WriteDecimal(char* out, std::uint32_t value) noexcept {
        char   temp[10];
        size_t count = 0;
        do {
          temp[count++] = static_cast<char>('0' + (value % 10u));
          value /= 10u;
        } while (value != 0u);

        for (size_t i = 0; i < count; ++i) {
          out[i] = temp[count - 1u - i];
        }
        return count;
      }

      Segment       m_segments[kMaxDepth]{};
      std::uint32_t m_depth    = 0;
      std::uint32_t m_overflow = 0;
    };

  }

  // ---------------------------------------------------------------------------
  // ArchiveContext
  // ---------------------------------------------------------------------------

  /**
   * @brief バージョン・診断の集約 (§5.3)
   *
   * @details
   *  アリーナはコンストラクタで受け取る。**`Document::Scratch()` を内部で掴まない。**
   *  `Scratch()` は `Document::Parse` の冒頭で `Reset` されるため、握ると Issue の
   *  パス文字列が次の `Parse` で消える。どのアリーナを使うかは呼び出し側が決める。
   */
  class ArchiveContext {
  public:
    explicit ArchiveContext(JsonArena& arena, std::uint32_t version = 0,
                            ArchiveFlags flags = ArchiveFlags::None) noexcept
      : m_arena(&arena), m_version(version), m_flags(flags) {
    }

    ArchiveContext(const ArchiveContext&)            = delete;
    ArchiveContext& operator=(const ArchiveContext&) = delete;

    // --- 問い合わせ ---

    [[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
    [[nodiscard]] ArchiveFlags  Flags()   const noexcept { return m_flags; }

    [[nodiscard]] bool          HasIssues()  const noexcept { return m_issues.Size() != 0u; }
    [[nodiscard]] std::uint32_t IssueCount() const noexcept { return m_issues.Size(); }
    [[nodiscard]] const ArchiveIssue* Issues() const noexcept { return m_issues.Data(); }

    /**
     * @brief ロードを失敗として扱うべきか
     * @details
     *  `MissingRequired` / `ContainerFailure` は常に Fatal。
     *  `StrictTypes` 指定時は `TypeMismatch` も Fatal。
     *  加えて**ルートが読めなかった場合**と**Issue 配列の確保に失敗した場合**も Fatal。
     *  §5.3 の定義 (MissingRequired / ContainerFailure) の上位互換になっている。
     */
    [[nodiscard]] bool HasFatal() const noexcept { return m_fatal; }

    /// Issue 配列の確保に失敗し、記録を取りこぼしたか (この場合 HasFatal も立つ)
    [[nodiscard]] bool IssuesTruncated() const noexcept { return m_issueStorageExhausted; }

    /**
     * @brief ReportUnknown 用の作業領域が確保できず、未知フィールドの報告が
     *        不完全になったか
     * @note  これは Fatal にしない。**診断機能の確保失敗でロードを失敗させるのは
     *        本末転倒**であるため、報告が欠けたことだけを伝える。
     */
    [[nodiscard]] bool DiagnosticsTruncated() const noexcept { return m_diagnosticsTruncated; }

    [[nodiscard]] JsonArena& Arena() noexcept { return *m_arena; }

    // --- アーカイブ実装が使う面 ---

    [[nodiscard]] Detail::PathStack& Path() noexcept { return m_path; }

    /// `"$version"` を読み取った `ReadArchive` が格納する (R3-7)
    void SetVersion(std::uint32_t version) noexcept { m_version = version; }

    /**
     * @brief 現在のパスを添えて Issue を記録する
     * @note  確保に失敗しても失敗を返さない。記録できなかったことは
     *        `IssuesTruncated()` で分かり、`HasFatal()` が立つ
     */
    void Report(ArchiveErrorKind kind, StringView detail) noexcept {
      if (IsFatalKind(kind)) {
        m_fatal = true;
      }

      ArchiveIssue issue;
      issue.kind   = kind;
      issue.path   = m_path.Materialize(*m_arena);
      issue.detail = detail;

      if (!m_issues.Push(issue, *m_arena)) {
        // Issue を積むこと自体が確保を要するため、「確保失敗を記録しようとして
        // 確保に失敗する」経路が存在する。確保不要なフラグで逃がす
        m_issueStorageExhausted = true;
        m_fatal           = true;
      }
    }

    void MarkFatal() noexcept { m_fatal = true; }
    void MarkDiagnosticsTruncated() noexcept { m_diagnosticsTruncated = true; }

    /// アリーナはそのままに、診断とパスだけを捨てる
    void Clear() noexcept {
      m_issues.Clear();
      m_path.Reset();
      m_fatal                = false;
      m_issueStorageExhausted      = false;
      m_diagnosticsTruncated = false;
    }

  private:
    /// 判定の実体は自由関数 `IsFatal` にある(診断側と 2 重に書かないため)
    [[nodiscard]] bool IsFatalKind(ArchiveErrorKind kind) const noexcept {
      return IsFatal(kind, m_flags);
    }

    JsonArena*                    m_arena;
    std::uint32_t                 m_version = 0;
    ArchiveFlags                  m_flags   = ArchiveFlags::None;
    Detail::ArenaVector<ArchiveIssue> m_issues;
    Detail::PathStack             m_path;
    bool                          m_fatal                = false;
    bool                          m_issueStorageExhausted      = false;
    bool                          m_diagnosticsTruncated = false;
  };

  // ---------------------------------------------------------------------------
  // 予約キー (R3-10)
  // ---------------------------------------------------------------------------

  /// ドキュメントのバージョンを保持する予約キー (R3-7)
  inline constexpr StringView kVersionKey{ "$version" };

  /// `$` 始まりは予約名前空間。ユーザー型のフィールド名には使えない
  [[nodiscard]] constexpr bool IsReservedKey(StringView key) noexcept {
    return !key.Empty() && key[0] == '$';
  }

  // ---------------------------------------------------------------------------
  // Serialize のディスパッチ (R3-2)
  // ---------------------------------------------------------------------------

  /**
   * @brief 自由関数 `Serialize` を追加できない型のための逃げ道
   *
   * @details
   *  主テンプレートは**わざと未定義**にしてある。特殊化して
   *  `static bool Serialize(Ar& ar, T& value)` を提供する。
   *  ディスパッチではこれを最優先で見る。ADL より優先させるのは、
   *  既に `Serialize` を持つサードパーティ型を上書きできなければ
   *  逃げ道として機能しないためである。
   */
  template <class T>
  struct JsonSerializer;

  /**
   * @brief 組み込みスカラの表
   *
   * @details
   *  主テンプレートは未定義 = スカラではない。`JsonArchiveTypes.h` が
   *  `bool` / 各整数幅 / `float` / `double` / `StringView` を特殊化する。
   *
   *  組み込み型を `Serialize` の**オーバーロード**で提供しない理由: `ar` の型が
   *  `GLFD::Json` に属するため、`Serialize(ar, v)` はどの `v` に対しても
   *  `GLFD::Json` を ADL の対象に含む。そこに `Serialize(Ar&, int&)` を置くと
   *  意図しない候補が常に見えることになる。特性テーブルなら曖昧さが生じない。
   */
  template <class T>
  struct ArchiveScalar;

  namespace Detail {

    template <class T>
    inline constexpr bool kAlwaysFalse = false;

    template <class Ar, class T>
    concept HasCustomSerializer = requires(Ar & ar, T & v) {
      { JsonSerializer<T>::Serialize(ar, v) } -> std::same_as<bool>;
    };

    template <class Ar, class T>
    concept HasAdlSerialize = requires(Ar & ar, T & v) { Serialize(ar, v); };

    template <class T>
    concept IsArchiveScalar = requires { { ArchiveScalar<T>::kIsScalar } -> std::convertible_to<bool>; }
                              && ArchiveScalar<T>::kIsScalar;

    /**
     * @brief 値ひとつをアーカイブする。読み書きで共通の唯一の入口
     *
     * @details
     *  優先順位は **`JsonSerializer<T>` 特殊化 → ADL の `Serialize` → 組み込みスカラ**。
     *
     *  **ユーザー型のときだけ `BeginObject` / `EndObject` で包む。**
     *  これにより §5.1 のとおり、利用者の `Serialize` は `ar.Member(...)` を
     *  並べるだけで済む。`JsonSerializer<T>` 特殊化は包まない
     *  (その特殊化自身が形を決めるため。スカラにも配列にもできる)。
     */
    template <class Ar, class T>
    [[nodiscard]] bool ArchiveOne(Ar& ar, T& value) {
      if constexpr (HasCustomSerializer<Ar, T>) {
        return JsonSerializer<T>::Serialize(ar, value);
      }
      else if constexpr (HasAdlSerialize<Ar, T>) {
        if (!ar.BeginObject()) {
          return false;
        }
        Serialize(ar, value);
        return ar.EndObject();
      }
      else if constexpr (IsArchiveScalar<T>) {
        return ar.Scalar(value);
      }
      else {
        static_assert(kAlwaysFalse<T>,
                      "No Serialize found for T. Provide an ADL free function "
                      "'void Serialize(Ar&, T&)', or specialize GLFD::Json::JsonSerializer<T>.");
        return false;
      }
    }

    /// `Serialize` が見つかる型か (診断のためだけの合成)
    template <class Ar, class T>
    concept Archivable = HasCustomSerializer<Ar, T> || HasAdlSerialize<Ar, T> || IsArchiveScalar<T>;

    /**
     * @brief `SkipDefaults` 用の比較可能性 (論点8)
     * @note  比較できない型ではコンパイルエラーにせず、単に省略しない。
     *        `SkipDefaults` は出力サイズの最適化であり、**多く出力する側に倒れるのは
     *        常に安全**であるため。`operator==` を持たない構造体をメンバに 1 つ
     *        持っただけでフラグ全体が使えなくなる方が実害が大きい
     */
    template <class T>
    concept ArchiveComparable = requires(const T& a, const T& b) {
      { a == b } -> std::convertible_to<bool>;
    };

    /**
     * @brief パスのセグメントを積み下ろしする番人
     * @note  書き込み側はパスを追跡しないため (R3-9)、`WriteArchive` はこれを使わない。
     *        `Serialize` のコードは読み書きで完全に同一のまま保たれる
     */
    class PathScope {
    public:
      PathScope(PathStack& path, StringView key,
                [[maybe_unused]] std::uint32_t frameDepth) noexcept : m_path(&path) {
        m_path->PushKey(key);
        AssertWithinFrames(frameDepth);
      }
      PathScope(PathStack& path, std::uint32_t index,
                [[maybe_unused]] std::uint32_t frameDepth) noexcept : m_path(&path) {
        m_path->PushIndex(index);
        AssertWithinFrames(frameDepth);
      }
      ~PathScope() { m_path->Pop(); }

      PathScope(const PathScope&)            = delete;
      PathScope& operator=(const PathScope&) = delete;

    private:
      /**
       * @brief 休眠の不変条件をその場で確かめる(Debug のみ)
       *
       * @details
       *  押した直後のパス深さは、**枠の数を超えてはならない**。
       *  `kMaxDepth` の @warning にあるとおり、截断が本番経路に現れないのは
       *  この性質のためであり、`ReadArchive::kMaxDepth` の `static_assert` は
       *  その代理指標にすぎない(定数を触らない変更は素通しする)。
       *
       *  破れるのは「**枠を伴わない `PathScope` を入れ子にした**」ときで、
       *  そのとき定数は無傷のままパスだけが深く伸びる。ここで止めれば、
       *  深いデータを待たずに**普通のテストが原因の場所で落ちる**。
       *
       *  @param frameDepth 呼び出し側の枠カウンタ。`$version` の読み取りだけは
       *         ルート枠が積まれる前に走るため、その場で `m_depth + 1` を渡している
       */
      void AssertWithinFrames([[maybe_unused]] std::uint32_t frameDepth) const noexcept {
        assert(m_path->Depth() <= frameDepth
               && "a PathScope was pushed deeper than the frame stack; "
                  "see the warning on Detail::PathStack::kMaxDepth");
      }

      PathStack* m_path;
    };

  }

}
