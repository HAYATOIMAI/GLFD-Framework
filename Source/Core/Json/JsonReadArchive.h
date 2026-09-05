#pragma once

/**
 * @file  JsonReadArchive.h
 * @brief Layer 3: DOM から読み出すアーカイブ (R3-15 / R3-16)
 *
 * @details
 *  読みはキーを任意順で引く必要があるためランダムアクセス = DOM が要る。
 *  書き (`WriteArchive`) が Writer へ直接書くのと基盤が違うのは欠陥ではなく、
 *  各方向の自然な形である(R3-15)。統一アーカイブが保証するのは
 *  **メンバ列挙が 1 箇所であること**であって、基盤の一致ではない。
 *
 *  依存は `JsonArchive.h` と `JsonDocument.h`(DOM 必須)。
 *
 *  使い方:
 *  @code
 *    Document doc(resource);
 *    if (!doc.Parse(json, ParseFlags::JsonC).IsOk()) { ... }
 *    ArchiveContext ctx(doc.Arena(), 0, ArchiveFlags::ReportUnknown);
 *    ReadArchive    ar(doc.Root(), ctx);
 *    Serialize(ar, params);          // ルートは ar が既に開いている
 *    if (ctx.HasFatal()) { ... }     // 読み側の成否はここで見る
 *  @endcode
 */

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "Core/StringView.h"
#include "Core/Json/JsonArchive.h"
#include "Core/Json/JsonDocument.h"

namespace GLFD::Json {

  namespace Detail {

    /// 範囲チェック付きの縮小変換。収まらなければ `out` を触らず false (R3-11)
    template <class T>
    [[nodiscard]] constexpr bool NarrowSigned(std::int64_t source, T& out) noexcept {
      constexpr auto kMin = static_cast<std::int64_t>((std::numeric_limits<T>::min)());
      constexpr auto kMax = static_cast<std::int64_t>((std::numeric_limits<T>::max)());
      if (source < kMin || source > kMax) {
        return false;
      }
      out = static_cast<T>(source);
      return true;
    }

    /// @copydoc NarrowSigned
    template <class T>
    [[nodiscard]] constexpr bool NarrowUnsigned(std::uint64_t source, T& out) noexcept {
      constexpr auto kMax = static_cast<std::uint64_t>((std::numeric_limits<T>::max)());
      if (source > kMax) {
        return false;
      }
      out = static_cast<T>(source);
      return true;
    }

  }

  /**
   * @brief 読み込み側のアーカイブ (§5.2)
   *
   * @details
   *  **失敗しても短絡しない**(論点6)。`HasFatal()` が立った後も最後まで読み続ける。
   *  読み側の価値は診断レポートそのものであり、手書き config に 5 箇所の誤りが
   *  あるなら 5 箇所とも報告すべきだからである(R3-9)。
   *
   *  **パスを追跡する**(R3-9)。セグメントのスタックだけを持ち、Issue を記録する
   *  瞬間にだけアリーナ上へ組み立てる。正常系では文字列連結が一度も起きない。
   */
  class ReadArchive {
  public:
    /**
     * @brief 枠の段数の上限。**`Detail::PathStack::kMaxDepth` から導出している**
     *
     * @warning この導出が「診断パスの截断が本番経路に現れない」ことを支えている。
     *          枠は必ずパスと同じか 1 段深い側で尽きるため、`PathStack` が
     *          溢れる前に `ContainerFailure` を報告して降下が止まる。
     *          **ここに独立の(より大きい)値を書くと、截断が初めて実行される。**
     *          その場合 `"/..."` 付きのパスが `ArchiveIssue::path` に現れ、
     *          `Query` はそれを解決しない(意図どおりだが、利用者から見ると
     *          「診断は出るのに値が引けない」経路が新たに生まれる)。
     *          変更するなら `JsonQuery.h` の截断の扱いと README も併せて見直すこと
     */
    static constexpr std::uint32_t kMaxDepth = Detail::PathStack::kMaxDepth;

    // 上の @warning を読ませるための番人。今は導出しているので自明に真であり、
    // **意図的に深くするときだけ**この行を消す一手が挟まる。消す前に
    // `JsonQuery.h` の截断の扱いと `JsonQueryTests` の T-46b を見ること
    static_assert(kMaxDepth <= Detail::PathStack::kMaxDepth,
                  "this wakes the dormant truncation branch of PathStack::Materialize; "
                  "read the warning above, JsonQuery.h and T-46b before removing this");

    ReadArchive(const Value& root, ArchiveContext& ctx) noexcept
      : m_ctx(&ctx), m_pending(&root) {

      if (!root.IsObject()) {
        // ルートが読めなければ何も取り出せない。StrictTypes に関わらず Fatal とする
        ctx.Report(ArchiveErrorKind::TypeMismatch,
                   StringView("the archive root must be a JSON object"));
        ctx.MarkFatal();
        m_pending = nullptr;
        return;
      }

      ReadVersion(root);
      (void)PushFrame(&root, true);   // ルートオブジェクトを開く
    }

    ~ReadArchive() {
      // ルートの枠は EndObject を通らないため、未知フィールドの掃き出しをここで行う。
      // 純粋な診断であり失敗を返す先が無いので、デストラクタで完結させている
      if (m_depth > 0u) {
        ReportUnknownFields(m_frames[0]);
      }
    }

    ReadArchive(const ReadArchive&)            = delete;
    ReadArchive& operator=(const ReadArchive&) = delete;

    static constexpr bool IsReading() noexcept { return true; }
    static constexpr bool IsWriting() noexcept { return false; }

    [[nodiscard]] std::uint32_t Version() const noexcept { return m_ctx->Version(); }

    /**
     * @brief 上書き読みか (2-3)
     * @note  抑えるのは**既定値の代入**と **`MissingRequired` の記録**の 2 箇所だけ。
     *        どちらも「上書き断片に無いのが正常」なものである
     */
    [[nodiscard]] bool IsOverlay() const noexcept {
      return HasFlag(m_ctx->Flags(), ArchiveFlags::Overlay);
    }

    // -------------------------------------------------------------------------
    // メンバ
    // -------------------------------------------------------------------------

    /**
     * @brief 既定値なしのメンバ (R3-4)
     * @return キーが無ければ `value` を変更せず `false`。
     *         構造体側の初期化子で既定値を持つ運用を許容する
     */
    template <class T>
    bool Member(StringView key, T& value) {
      if (!RootIsUsable()) { return false; }
      const std::uint32_t index = LocateMember(key);
      if (index == kInvalidIndex) {
        return false;
      }
      return ReadMemberAt(index, key, value);
    }

    /**
     * @brief 既定値つきのメンバ (R3-4)
     * @return キーが無ければ `value = defaultValue` として `true`。
     *         **パッチで増えたフィールドを旧セーブが読める = 後方互換**
     */
    template <class T>
    bool Member(StringView key, T& value, const T& defaultValue) {
      // ルートが読めなかった場合は既定値の代入もしない。
      // 「何も起きなかった」状態を呼び出し側へ返せる唯一のケースであり、
      // ロード前の値を保持したまま失敗を扱えるようにするため
      if (!RootIsUsable()) { return false; }

      const std::uint32_t index = LocateMember(key);
      if (index == kInvalidIndex) {
        // 上書き読み (2-3) では既定値を代入しない。代入すると**1 回目に読んだ値を
        // 既定値で潰す**。「書いてあるフィールドだけが上書きされる」という
        // レイヤ設定の意味論は、この 1 行の抑制で成立する
        if (IsOverlay()) { return false; }

        value = defaultValue;
        return true;
      }
      return ReadMemberAt(index, key, value);
    }

    /// 必須メンバ (R3-6)。欠損時は `MissingRequired` を記録し `HasFatal()` を立てる
    template <class T>
    bool RequiredMember(StringView key, T& value) {
      if (!RootIsUsable()) { return false; }
      const std::uint32_t index = LocateMember(key);
      if (index == kInvalidIndex) {
        // 上書き読み (2-3) では記録しない。**上書き断片に必須フィールドが
        // 無いのは正常**であり、必須かどうかは 1 回目(基底)が既に検査している
        if (IsOverlay()) { return false; }

        const Detail::PathScope scope(m_ctx->Path(), key, m_depth);
        m_ctx->Report(ArchiveErrorKind::MissingRequired,
                      StringView("a required field is missing"));
        return false;
      }
      return ReadMemberAt(index, key, value);
    }

    // -------------------------------------------------------------------------
    // 配列 (論点3)
    // -------------------------------------------------------------------------

    /**
     * @brief 配列を開き、**実際の**要素数を返す
     * @param hint 呼び出し側が持っている要素数。読み側では使わない
     * @note  配列でなければ `TypeMismatch` を記録して 0 を返す。
     *        どちらの場合も枠は積まれるので `EndArray()` は必ず対で呼べる
     */
    std::uint32_t BeginArray(std::uint32_t hint) {
      (void)hint;

      if (m_pending == nullptr || !m_pending->IsArray()) {
        if (m_pending != nullptr) {
          m_ctx->Report(ArchiveErrorKind::TypeMismatch, StringView("expected an array"));
        }
        ++m_unopenedArrays;
        return 0;
      }
      if (!PushFrame(m_pending, false)) {
        ++m_unopenedArrays;
        return 0;
      }
      return m_frames[m_depth - 1u].count;
    }

    bool EndArray() {
      if (m_unopenedArrays > 0u) {
        --m_unopenedArrays;
        return true;
      }
      assert(m_depth > 0u && !m_frames[m_depth - 1u].isObject
             && "ReadArchive::EndArray: not inside an array");
      if (m_depth > 0u) { --m_depth; }
      return true;
    }

    /**
     * @brief 直近の `BeginArray` が**実際に配列を開けたか** (2-3)
     *
     * @details
     *  `BeginArray` は「配列ではなかった」と「空配列 `[]` だった」の
     *  **どちらでも 0 を返す**ため、コンテナ側はこれを見ないと区別できない。
     *  ところが扱いは正反対である。
     *
     *   - `[]`      … 「空にする」という正当な指示。リサイズしてよい
     *   - 配列でない … 上書きに失敗した(`TypeMismatch` は記録済み)。
     *                  **中身を保つべき**で、リサイズすると直前の値が消える
     *
     *  レイヤ設定 (2-3) で `.local.json` に `"profiles": 42` と書いたときに、
     *  `.jsonc` から読んだ内容が消えないようにするために追加した。
     */
    [[nodiscard]] bool ArrayWasOpened() const noexcept { return m_unopenedArrays == 0u; }

    /// 配列の次の要素を読む。要素が尽きていれば `false`
    template <class T>
    bool Element(T& value) {
      if (m_depth == 0u) { return false; }

      Frame& frame = m_frames[m_depth - 1u];
      if (frame.isObject || frame.cursor >= frame.count) {
        return false;
      }

      const std::uint32_t index = frame.cursor++;
      const Detail::PathScope scope(m_ctx->Path(), index, m_depth);

      const Value* const saved = m_pending;
      m_pending = &(*frame.container)[index];
      const bool ok = Detail::ArchiveOne(*this, value);
      m_pending = saved;
      return ok;
    }

    // -------------------------------------------------------------------------
    // ディスパッチが呼ぶ面
    // -------------------------------------------------------------------------

    bool BeginObject() {
      if (m_pending == nullptr) { return false; }
      if (!m_pending->IsObject()) {
        m_ctx->Report(ArchiveErrorKind::TypeMismatch, StringView("expected an object"));
        return false;
      }
      // 積めなかった場合は EndObject も呼ばれないので、balance は崩れない
      return PushFrame(m_pending, true);
    }

    bool EndObject() {
      assert(m_depth > 0u && m_frames[m_depth - 1u].isObject
             && "ReadArchive::EndObject: not inside an object");
      if (m_depth == 0u) { return false; }

      ReportUnknownFields(m_frames[m_depth - 1u]);
      --m_depth;
      return true;
    }

    /**
     * @brief いま読み出そうとしている値が `null` か (1-6 で追加)
     * @note  `std::optional<T>` のように**値の不在を型で表す**型が必要とする。
     *        これが無いと「本当に null だった」と「型が違って読めなかった」を
     *        区別できず、`TypeMismatch` を出さずに無効値へ落とす経路が作れない
     */
    [[nodiscard]] bool IsNull() const noexcept {
      return m_pending != nullptr && m_pending->IsNull();
    }

    /**
     * @brief 組み込みスカラの読み出し
     * @note  数値は常に範囲チェックする。**サイレントな切り詰めをしない**(R3-11)。
     *        失敗時は `out` を変更しない(`Value::TryGet*` の保証をそのまま使う)
     */
    template <class T>
    bool Scalar(T& value) {
      if (m_pending == nullptr) { return false; }
      const Value& source = *m_pending;

      if constexpr (std::is_same_v<T, bool>) {
        if (!source.TryGetBool(value)) {
          return TypeMismatch("expected a boolean");
        }
        return true;
      }
      else if constexpr (std::is_same_v<T, StringView>) {
        StringView text;
        if (!source.TryGetString(text)) {
          return TypeMismatch("expected a string");
        }
        return CopyString(text, value);
      }
      else if constexpr (std::is_same_v<T, float>) {
        if (!source.TryGetFloat(value)) {
          return NumberFailure(source, "the value does not fit in a float");
        }
        return true;
      }
      else if constexpr (std::is_same_v<T, double>) {
        if (!source.TryGetDouble(value)) {
          return NumberFailure(source, "the value does not fit in a double");
        }
        return true;
      }
      else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        std::int64_t wide = 0;
        if (!source.TryGetInt64(wide)) {
          return NumberFailure(source, "the value is not a signed integer in range");
        }
        if (!Detail::NarrowSigned(wide, value)) {
          m_ctx->Report(ArchiveErrorKind::RangeOverflow,
                        StringView("the value does not fit in the target integer type"));
          return false;
        }
        return true;
      }
      else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
        std::uint64_t wide = 0;
        if (!source.TryGetUInt64(wide)) {
          return NumberFailure(source, "the value is not an unsigned integer in range");
        }
        if (!Detail::NarrowUnsigned(wide, value)) {
          m_ctx->Report(ArchiveErrorKind::RangeOverflow,
                        StringView("the value does not fit in the target integer type"));
          return false;
        }
        return true;
      }
      else {
        static_assert(Detail::kAlwaysFalse<T>, "ReadArchive::Scalar: unsupported scalar type");
        return false;
      }
    }

    /**
     * @brief コンテナ側から確保失敗を伝えるための口 (R3-13)
     *
     * @note detail に**リソース未設定の可能性**を書いてある。原因は
     *       「メモリ不足」と「`IMemoryResource` が一度も設定されていない」の
     *       2 つがあり、後者の方が現実には起こりやすい
     *       (`DynamicArray<T> a;` と書けてしまうため)。
     *       「リサイズに失敗」とだけ言われても、リソース未設定には気づけない。
     */
    void ReportContainerFailure() noexcept {
      m_ctx->Report(
        ArchiveErrorKind::ContainerFailure,
        StringView("the container could not be resized "
                   "(out of memory, or its IMemoryResource was never set)"));
    }

    /**
     * @brief 要素数が期待と違うことを伝える口 (1-6 で追加)
     * @note  `ContainerFailure` と分けてあるのは調査先が違うためである。
     *        要素数不一致は JSON 側を直す話、確保失敗はメモリ側を疑う話。
     *        `StrictTypes` の支配下にも置かない(型ではなく長さの話であり、
     *        「型には厳しく長さには寛容に」を選べなくなるため)
     */
    void ReportArrayLengthMismatch() noexcept {
      // `BeginArray` が配列を開けなかった場合は、その時点で `TypeMismatch` を
      // 記録済みである。長さの話は無意味なので二重に報告しない。
      // (`T[N]` は `n != N` を満たすだけで呼ぶため、配列でない値に対して
      //  必ずここへ来る。抑制はコンテナ側ではなく、開けたかどうかを知っている
      //  アーカイブ側の責務である)
      if (m_unopenedArrays != 0u) {
        return;
      }
      m_ctx->Report(ArchiveErrorKind::ArrayLengthMismatch,
                    StringView("the JSON array does not have the expected element count"));
    }

    /**
     * @brief 診断を記録するための一般の口
     *
     * @details
     *  `JsonSerializer<T>` 特殊化から任意の Issue を記録できるようにする。
     *  現在のパスは `ArchiveContext` が自動で添える。
     *  例: 列挙の文字列表現で未知の綴りを受け取ったときに `TypeMismatch` を残す。
     *  これが無いと、手書き config の綴り間違いが**何の診断も出さずに**
     *  既定値へ落ちることになり、R3-9 の目的を損なう。
     */
    [[nodiscard]] ArchiveContext& Context() noexcept { return *m_ctx; }

  private:
    static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

    struct Frame {
      const Value*   container = nullptr;
      std::uint64_t* consumed  = nullptr;   ///< ReportUnknown 時のみ確保する
      std::uint32_t  count     = 0;
      std::uint32_t  cursor    = 0;         ///< 配列の読み出し位置
      bool           isObject  = false;
    };

    // --- バージョン ---

    /**
     * @brief ルートの `"$version"` を読んで `ArchiveContext` へ格納する (R3-7)
     *
     * @note **`Overlay` のときは格納しない** (A-2 論点4)。上書き断片は独立した
     *       ドキュメントではなく、版は上書きされる側 (`.jsonc`) のものを採る
     *       (R2-18q)。呼び出し側が `ArchiveContext(arena, baseVersion, Overlay)` と
     *       作った版がそのまま `ar.Version()` になり、移行分岐 (R3-8) が
     *       **基底と同じ枝を通る**。
     *
     *       これが無いと `ar.Version()` は上書きパスで**常に 0** になる。
     *       `.local.json` は `$version` を書かないのが正常なので、
     *       「不在なら 0」がそのまま当たってしまう。基底が v2 でも 2 段目だけ
     *       v1 として読まれ、新しい名前で書いた上書きが `UnknownField` になる。
     *
     *       **`Overlay` を別の用途に使うときはここを見ること。**
     *       「自前の版を持たない断片」以外の意味で使うなら、この抑制は正しくない
     *
     * @note 抑えるのは**格納だけ**で、診断は抑えない。壊れた `$version` の
     *       `TypeMismatch` は `Overlay` でも出す(`Overlay` の既存 2 箇所と同じ方針)
     */
    void ReadVersion(const Value& root) noexcept {
      // 上書き断片は自前の版を持たない。呼び出し側が渡した版をそのまま保つ
      const bool inherit = IsOverlay();

      const Value* const found = root.Find(kVersionKey);
      if (found == nullptr) {
        if (!inherit) { m_ctx->SetVersion(0); }   // R3-7: 不在時は 0
        return;
      }

      std::uint64_t version = 0;
      if (!found->TryGetUInt64(version) || version > 0xFFFFFFFFull) {
        // 予約キーが壊れている。黙って 0 にすると誤った移行分岐を選びかねない
        // **唯一 +1 を渡す場所。** `$version` はルート枠を積む前に読むため、
        // ここだけ枠カウンタが 1 つ少ない(この行を写すときは理由も一緒に写すこと)
        const Detail::PathScope scope(m_ctx->Path(), kVersionKey, m_depth + 1u);
        m_ctx->Report(ArchiveErrorKind::TypeMismatch,
                      StringView("$version must be a non-negative integer that fits in 32 bits"));
        if (!inherit) { m_ctx->SetVersion(0); }
        return;
      }
      if (!inherit) { m_ctx->SetVersion(static_cast<std::uint32_t>(version)); }
    }

    // --- 枠 ---

    /// ルートの枠はコンストラクタで積まれ、二度と降ろされない。
    /// したがって深さ 0 はルートが読めなかったことと等価である
    [[nodiscard]] bool RootIsUsable() const noexcept { return m_depth != 0u; }

    [[nodiscard]] bool PushFrame(const Value* container, bool isObject) noexcept {
      if (m_depth == kMaxDepth) {
        m_ctx->Report(ArchiveErrorKind::ContainerFailure,
                      StringView("the archive nesting is too deep"));
        return false;
      }

      Frame& frame    = m_frames[m_depth++];
      frame.container = container;
      frame.isObject  = isObject;
      frame.cursor    = 0;
      frame.consumed  = nullptr;
      frame.count     = isObject ? container->MemberCount() : container->Size();

      if (isObject && frame.count != 0u
          && HasFlag(m_ctx->Flags(), ArchiveFlags::ReportUnknown)) {
        // 「Serialize が参照しなかったフィールド」を知るための作業領域。
        // **ReportUnknown 指定時のみ**確保する。既定のロード経路ではゼロコスト
        const std::uint32_t words = (frame.count + 63u) / 64u;
        frame.consumed = m_ctx->Arena().AllocateArray<std::uint64_t>(words);
        if (frame.consumed == nullptr) {
          // 診断機能の確保失敗でロードを失敗させるのは本末転倒。
          // 報告が欠けたことだけを伝える (Fatal にしない)
          m_ctx->MarkDiagnosticsTruncated();
        }
        else {
          for (std::uint32_t i = 0; i < words; ++i) { frame.consumed[i] = 0u; }
        }
      }
      return true;
    }

    // --- メンバの検索 ---

    /// 現在のオブジェクトから `key` の添字を引き、消費済みとして印を付ける
    [[nodiscard]] std::uint32_t LocateMember(StringView key) noexcept {
      assert(!IsReservedKey(key)
             && "ReadArchive::Member: keys starting with '$' are reserved (R3-10)");

      if (m_depth == 0u) { return kInvalidIndex; }
      Frame& frame = m_frames[m_depth - 1u];
      if (!frame.isObject || frame.container == nullptr) { return kInvalidIndex; }

      const ::GLFD::Json::Member* const members = frame.container->MemberBegin();
      for (std::uint32_t i = 0; i < frame.count; ++i) {
        if (members[i].key == key) {
          if (frame.consumed != nullptr) {
            frame.consumed[i / 64u] |= (std::uint64_t{ 1 } << (i % 64u));
          }
          return i;
        }
      }
      return kInvalidIndex;
    }

    template <class T>
    bool ReadMemberAt(std::uint32_t index, StringView key, T& value) {
      Frame& frame = m_frames[m_depth - 1u];
      const Detail::PathScope scope(m_ctx->Path(), key, m_depth);

      const Value* const saved = m_pending;
      m_pending = &frame.container->MemberBegin()[index].value;
      const bool ok = Detail::ArchiveOne(*this, value);
      m_pending = saved;
      return ok;
    }

    // --- 診断 ---

    void ReportUnknownFields(const Frame& frame) noexcept {
      if (frame.consumed == nullptr || frame.container == nullptr) { return; }

      const ::GLFD::Json::Member* const members = frame.container->MemberBegin();
      for (std::uint32_t i = 0; i < frame.count; ++i) {
        if ((frame.consumed[i / 64u] >> (i % 64u)) & std::uint64_t{ 1 }) { continue; }
        if (IsReservedKey(members[i].key)) { continue; }   // "$version" など

        // `EndObject` は枠を降ろす**前**にここへ来る(デストラクタからはルート枠)。
        // どちらも掃き出し中の枠がまだ数えられている
        const Detail::PathScope scope(m_ctx->Path(), members[i].key, m_depth);
        m_ctx->Report(ArchiveErrorKind::UnknownField,
                      StringView("the field is not read by Serialize"));
      }
    }

    bool TypeMismatch(const char* detail) noexcept {
      m_ctx->Report(ArchiveErrorKind::TypeMismatch, StringView(detail));
      return false;
    }

    /// 数値として読めなかった原因を「型違い」と「範囲外」に切り分ける
    bool NumberFailure(const Value& source, const char* detail) noexcept {
      if (!source.IsNumber()) {
        return TypeMismatch("expected a number");
      }
      m_ctx->Report(ArchiveErrorKind::RangeOverflow, StringView(detail));
      return false;
    }

    /// 読み取った文字列をアリーナへ複製する (§5.4)。DOM より長生きできるようにする
    bool CopyString(StringView text, StringView& out) noexcept {
      if (text.Empty()) {
        out = StringView(reinterpret_cast<const char*>(""), 0);
        return true;
      }
      char* const buffer = m_ctx->Arena().AllocateArray<char>(text.Size() + 1u);
      if (buffer == nullptr) {
        m_ctx->Report(ArchiveErrorKind::ContainerFailure,
                      StringView("could not copy the string into the archive arena"));
        return false;
      }
      std::memcpy(buffer, text.Data(), text.Size());
      buffer[text.Size()] = '\0';
      out = StringView(buffer, text.Size());
      return true;
    }

    ArchiveContext* m_ctx     = nullptr;
    const Value*    m_pending = nullptr;   ///< いま読み出そうとしている値

    Frame         m_frames[kMaxDepth]{};
    std::uint32_t m_depth          = 0;
    std::uint32_t m_unopenedArrays = 0;    ///< 枠を積めなかった BeginArray の数
  };

}
