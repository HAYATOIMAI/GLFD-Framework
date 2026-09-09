#pragma once

#include "Registry.h"

#include "../Core/DynamicArray.h"
#include "../Core/MemoryResource.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

namespace GLFD::ECS {

  /// 積まれた操作の種類。**診断のためだけに持つ**(適用は関数ポインタが行う)
  enum class CommandKind : std::uint8_t {
    Destroy,
    Add,
    Remove,
  };

  /**
   * @brief 適用時にコマンドを捨てた理由 (R-20)
   *
   * @note `AlreadyDestroyed` だけは**異常ではない**。2 つのシステムが同じ敵に
   *       破棄を積むのは VS 型では普通に起きる。理由を分けてあるのは、
   *       出力する側が「流していい報告」と「調べるべき報告」を選り分けられる
   *       ようにするためである。**捨てたこと自体は必ず数える。**
   */
  enum class DropReason : std::uint8_t {
    DeadEntity,        ///< Add / Remove の相手が適用時に生きていなかった
    AlreadyDestroyed,  ///< Destroy の相手が既に死んでいた(二重積み)
    AlreadyPresent,    ///< Add の相手が既にその型を持っていた
    AllocationFailed,  ///< Add がプールまたは dense を伸ばせなかった
  };

  struct DroppedCommand {
    Entity      entity{};
    CommandKind kind   = CommandKind::Destroy;
    DropReason  reason = DropReason::DeadEntity;
  };

  /**
   * @brief 1 回の適用で何が起きたかの報告 (R-20 / R-28)
   *
   * @details
   *  **構造は下層に置き、出力は上層で行う。** `Registry` はここへ書くだけで
   *  `Logger` を呼ばない。JSON の `ArchiveContext` と同じ分担である。
   *
   *  `applied + dropped` は適用したコマンドの総数に等しい。**すべてのコマンドが
   *  必ずどちらか一方に数えられる**(黙って消える経路が無いことが要件)。
   */
  class ApplyReport {
  public:
    /**
     * @brief 明細を記録する件数の上限
     *
     * @details
     *  **8 という数そのものに意味は無い。意味があるのは「固定長である」ことである。**
     *
     *  この報告書は**確保失敗から回復している最中に書かれる**。`Add` の適用が
     *  失敗する主な理由が確保失敗なので、そこで確保する診断は
     *  **いちばん必要なときに黙る**。だから固定長にしてある。
     *
     *  @warning **増やす提案が出たら、「確保しない範囲で」という制約ごと
     *           受け取ること。** 数を増やすこと自体は構わない(これは
     *           `CommandBuffer` の一部として 1 個だけ存在する)。
     *           `DynamicArray` に変える、`IMemoryResource` から取る、
     *           といった形にしてはならない。
     *
     *  件数そのものは `Dropped()` が正確に数えているので、上限で失われるのは
     *  **明細だけ**である。溢れは `Truncated()` が伝える。
     */
    static constexpr std::uint32_t kMaxRecorded = 8;

    [[nodiscard]] std::uint32_t Applied() const noexcept { return m_applied; }
    [[nodiscard]] std::uint32_t Dropped() const noexcept { return m_dropped; }

    /// 明細として残っている件数 (`Dropped()` 以下)
    [[nodiscard]] std::uint32_t RecordedCount() const noexcept { return m_recordedCount; }

    [[nodiscard]] const DroppedCommand& Recorded(std::uint32_t i) const noexcept {
      assert(i < m_recordedCount && "ApplyReport::Recorded: index out of range");
      return m_recorded[i];
    }

    /// 明細を取りこぼしたか。**件数 (`Dropped()`) は取りこぼしていない**
    [[nodiscard]] bool Truncated() const noexcept { return m_truncated; }

    /// 何も適用せず何も捨てなかったか。**空のバッファを適用した状態**
    [[nodiscard]] bool IsQuiet() const noexcept { return m_applied == 0u && m_dropped == 0u; }

  private:
    friend class Registry;

    void CountApplied() noexcept { ++m_applied; }

    void Record(CommandKind kind, DropReason reason, Entity entity) noexcept {
      ++m_dropped;
      if (m_recordedCount >= kMaxRecorded) {
        m_truncated = true;
        return;
      }
      DroppedCommand& slot = m_recorded[m_recordedCount++];
      slot.entity = entity;
      slot.kind   = kind;
      slot.reason = reason;
    }

    std::uint32_t  m_applied       = 0;
    std::uint32_t  m_dropped       = 0;
    std::uint32_t  m_recordedCount = 0;
    bool           m_truncated     = false;
    DroppedCommand m_recorded[kMaxRecorded]{};
  };

  /**
   * @brief 構造変更を積み、フレーム境界でまとめて適用する (ECS 1-4 / §5)
   *
   * @details
   *  ## なぜ遅延するのか
   *  VS / EDF 型では「**敵を反復している最中に、その敵を破棄して経験値を
   *  生成する**」が通常処理になる。即時に適用すると:
   *   - 生成による `DynamicArray` の再確保で、**反復中の生ポインタがダングリング**する
   *   - 削除の swap-and-pop で、**飛ばしと重複**が起きる
   *
   *  積むだけなら `SparseSet` は 1 ミリも動かないので、反復は安全なまま進む。
   *
   *  ## 受け入れる制約 (§5.3)
   *  **「敵を倒した直後に経験値を取得する」は 1 フレーム遅れる。**
   *
   *  @warning **この 2 つは対で読むこと。** 遅れが目に付いて「即時適用に戻そう」
   *           という提案が出たときに、上の「なぜ遅延するのか」を一緒に読めば、
   *           **その提案は反復の安全性を手放す提案である**ことが分かる。
   *           VS 型では 1 フレームの遅れは許容範囲であり、代わりに反復の安全性が
   *           **構造的に**保証される(規約ではなく構造で)。
   *
   *  ## 生成は即時である (R-17)
   *  `Registry::CreateEntity` はその場で `Entity` を返す。返せないと
   *  「生成した弾に速度を設定する」が書けないためである。
   *
   *  @note **その帰結として、同じフレーム内に「生きているが成分を 1 つも持たない
   *        エンティティ」が存在する。** 成分の反復には現れないので実害は無いが、
   *        **`Registry::AliveCount()` には数えられる。** 生成した直後に
   *        `AliveCount()` を見て「成分を持つ数」だと解釈しないこと。
   *
   *  ## スレッド境界 (R-18 / N-ECS-1)
   *  **1 本のバッファは 1 本のスレッドのものである。** 構築したスレッド以外から
   *  積むこと・適用することを Debug で `assert` が捕まえる。
   *  フェーズ2で並列からの構造変更が要るようになったら、**ワーカごとに 1 本ずつ
   *  持たせ、適用はメインスレッドで行う**形にできる(この設計はそれを妨げない)。
   *
   *  ## メモリ (§4 論点3)
   *  **フレームアロケータからは取らない。** `StackResource::Deallocate` は no-op
   *  なので、`DynamicArray` が 1.5 倍で伸びるたびに旧領域がフレーム内で死蔵される。
   *  加えて毎フレーム容量ゼロから作り直すことになり、毎フレーム走る経路で
   *  避けられる確保を繰り返す。**寿命の長いリソースから取り、`Clear()` で容量を
   *  保って使い回す**ほうが、最高水位に達したあと確保がゼロになる。
   *  起動時に `TryReserve` で一度に取っておけばさらに確実である。
   */
  class CommandBuffer {
  public:
    explicit CommandBuffer(Memory::IMemoryResource* resource) noexcept
      : m_commands(resource)
      , m_payload(resource)
      , m_owner(std::this_thread::get_id()) {
    }

    CommandBuffer(const CommandBuffer&)            = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    /**
     * @brief 破棄を積む
     * @return 積めたか。**確保に失敗すると false** (R-20)
     * @note 同じ `Entity` に 2 回積んでも安全。2 回目は適用時に捨てられる
     */
    [[nodiscard]] bool Destroy(Entity entity) noexcept {
      AssertOwnerThread();
      return m_commands.TryPushBack(
          Command{ &ApplyDestroy, entity, 0u, CommandKind::Destroy });
    }

    /**
     * @brief 成分の追加を積む。**値はここで複製される**
     * @return 積めたか。**確保に失敗すると false** (R-20)
     *
     * @note `T` に `trivially copyable` を要求する (§4 論点2)。要求すれば複製が
     *       memcpy で済み、ムーブとデストラクタの型消去が丸ごと不要になる。
     *
     * @warning **`DynamicArray` をメンバに持つコンポーネントはここへ渡せない**
     *          (JSON の R3-32 と同型の制約)。`Inventory { DynamicArray<ItemId> }`
     *          のような型は**書けるが、遅延追加の経路には載らない**。
     *          逃げ道は 2 つある:
     *           1. 制約が掛かるのは**この経路だけ**である。`Registry::AddComponent`
     *              は従来どおりで、生成時にその場で足せる
     *           2. それでも足りなければ、本体を別に置いてハンドル(添字)を
     *              コンポーネントにする。最後の手段としてムーブ/デストラクタの
     *              トランポリンを足すことになるが、**そのときは N-2(投げるムーブを
     *              毎フレームの経路でどう扱うか)を先に決めること**
     */
    template <class T>
    [[nodiscard]] bool Add(Entity entity, const T& value) noexcept {
      static_assert(std::is_trivially_copyable_v<T>,
                    "CommandBuffer::Add requires a trivially copyable component. The deferred "
                    "path copies the value with memcpy so that it does not have to type-erase "
                    "the move constructor and the destructor. A component that owns memory "
                    "(a DynamicArray member, for instance) can still be added immediately "
                    "through Registry::AddComponent; it just cannot be queued.");
      static_assert(alignof(T) <= alignof(PayloadSlot),
                    "the payload is stored in 16-byte aligned slots, which is what every "
                    "current component needs (they are all alignas(16) for SIMD loads). "
                    "Widen PayloadSlot before adding a component with a stricter alignment.");

      AssertOwnerThread();

      constexpr std::size_t slots = (sizeof(T) + sizeof(PayloadSlot) - 1u) / sizeof(PayloadSlot);
      const std::size_t     base  = m_payload.GetSize();

      // 添字は 32 bit で持つ (Command を小さく保つため)。純粋な防御
      if (base > kMaxPayloadSlots - slots) {
        return false;
      }

      for (std::size_t i = 0; i < slots; ++i) {
        if (m_payload.TryEmplaceBack() == nullptr) {
          UnwindPayload(base);
          return false;
        }
      }
      std::memcpy(m_payload.GetData() + base, &value, sizeof(T));

      if (!m_commands.TryPushBack(Command{ &ApplyAdd<T>, entity,
                                           static_cast<std::uint32_t>(base), CommandKind::Add })) {
        UnwindPayload(base);   // **積めなかったら中身を戻す**(状態を変えない)
        return false;
      }
      return true;
    }

    /**
     * @brief 成分の削除を積む
     * @return 積めたか。**確保に失敗すると false** (R-20)
     * @note 持っていない成分の削除は**捨てない**。`Registry::RemoveComponent` が
     *       もともと安全な no-op であり、「意図を果たせなかった」わけではない
     */
    template <class T>
    [[nodiscard]] bool Remove(Entity entity) noexcept {
      AssertOwnerThread();
      return m_commands.TryPushBack(
          Command{ &ApplyRemove<T>, entity, 0u, CommandKind::Remove });
    }

    /**
     * @brief 容量を先に取っておく
     * @note  最高水位まで伸びたあとは `Clear()` が容量を保つので、確保は起きなくなる。
     *        起動時にここで取り切っておけば、毎フレームの経路から確保が完全に消える
     */
    [[nodiscard]] bool TryReserve(std::size_t commands, std::size_t payloadSlots) noexcept {
      return m_commands.TryReserve(commands) && m_payload.TryReserve(payloadSlots);
    }

    [[nodiscard]] bool        IsEmpty() const noexcept { return m_commands.IsEmpty(); }
    [[nodiscard]] std::size_t Size()    const noexcept { return m_commands.GetSize(); }

    /**
     * @brief 積んだものを**適用せずに**捨てる
     * @note  **報告も初期化される。** 適用の結果を読むのは `ApplyCommands` の直後で、
     *        そこでは `ApplyCommands` が中身だけを空にしている
     */
    void Clear() noexcept {
      m_commands.Clear();
      m_payload.Clear();
      m_report = ApplyReport{};
    }

    /// 直前の `Registry::ApplyCommands` で何が起きたか (R-20)
    [[nodiscard]] const ApplyReport& Report() const noexcept { return m_report; }

    /**
     * @brief 呼んでいるスレッドがこのバッファの所有者か (R-18)
     *
     * @note **`assert` と同じ判定を公開している。** `assert` は発火すると
     *       `abort()` するのでテストから確かめられない。境界が機械的に
     *       検出できることをテストで固定するために、判定そのものを外へ出す
     *       (T-ECS-14)。Release でも同じ答えを返す
     */
    [[nodiscard]] bool IsOwnerThread() const noexcept {
      return std::this_thread::get_id() == m_owner;
    }

  private:
    friend class Registry;

    /**
     * @brief 値の置き場。**アライメントを算術ではなく粒度で解く**
     *
     * @details
     *  `DynamicArray::TryReallocate` は `Allocate(n * sizeof(T), alignof(T))` を
     *  呼ぶので、この型の配列は**先頭が 16 バイト境界**になり、各スロットも
     *  16 の倍数だけ離れる。したがって `alignof(T) <= 16` である限り、
     *  **詰め物を手で計算する必要がない**。
     *
     *  現在のコンポーネントは 4 型すべて `alignas(16)`(`Position` の `w` は
     *  SIMD ロードのためのパディング)なので、この粒度がそのまま要求に一致する。
     */
    struct alignas(16) PayloadSlot {
      unsigned char bytes[16];
    };

    /// 適用の実体。**型ごとに実体化される**(`ISparseSet` には追加の経路が無い)
    using ApplyFn = bool (*)(Registry&, Entity, const void*, DropReason&) noexcept;

    struct Command {
      ApplyFn       apply;
      Entity        entity;
      /**
       * 値の位置。**ポインタではなく添字である。**
       * バッファは伸長で再配置されるので、生ポインタを持つと
       * 「反復中のダングリングを避けるための仕組み」自身がダングリングする
       */
      std::uint32_t payloadSlot;
      CommandKind   kind;
    };

    static constexpr std::size_t kMaxPayloadSlots = 0xFFFFFFFFull;

    template <class T>
    static bool ApplyAdd(Registry& registry, Entity entity, const void* payload,
                         DropReason& reason) noexcept {
      if (registry.AddComponent<T>(entity, *static_cast<const T*>(payload)) != nullptr) {
        return true;
      }
      // **成功した経路では余分な探索をしない。** 失敗したときだけ理由を分ける
      // (`FindPool` はプール一覧の線形探索なので、毎回引くと積み重なる)
      reason = registry.HasComponent<T>(entity) ? DropReason::AlreadyPresent
                                                : DropReason::AllocationFailed;
      return false;
    }

    template <class T>
    static bool ApplyRemove(Registry& registry, Entity entity, const void*,
                            DropReason&) noexcept {
      registry.RemoveComponent<T>(entity);
      return true;
    }

    static bool ApplyDestroy(Registry& registry, Entity entity, const void*,
                             DropReason&) noexcept {
      registry.DestroyEntity(entity);
      return true;
    }

    void UnwindPayload(std::size_t base) noexcept {
      while (m_payload.GetSize() > base) {
        m_payload.PopBack();
      }
    }

    /**
     * @brief 積み込みと適用が所有スレッドから行われていることを確かめる (R-18)
     *
     * @note **`m_owner` は Debug / Release どちらにも置き、`assert` だけを
     *       Debug に限る。** `#ifdef` でメンバを増減させるとヘッダオンリーの
     *       クラスの `sizeof` が構成ごとに変わり、Debug のテストと Release の
     *       本体が混ざったときに静かに壊れる(ODR)。8 バイトのために
     *       踏む種類の橋ではない
     */
    void AssertOwnerThread() const noexcept {
      assert(IsOwnerThread()
             && "CommandBuffer: one buffer belongs to one thread (R-18). Structural changes "
                "are main-thread only in phase 1; parallel systems may only write component "
                "values. If a worker needs to queue changes, give it its own buffer and apply "
                "it on the main thread.");
    }

    DynamicArray<Command>     m_commands;
    DynamicArray<PayloadSlot> m_payload;
    ApplyReport               m_report{};
    std::thread::id           m_owner;
  };

  // ---------------------------------------------------------------------------
  // Registry::ApplyCommands
  //
  //  **ここに置く理由**: トランポリンの中の `registry.AddComponent<T>(...)` は
  //  非依存名なので、テンプレートの定義時点で解決される。つまり
  //  `CommandBuffer` の定義より前に `Registry` が完全型でなければならない。
  //  一方 `Registry` の側は `CommandBuffer` を前方宣言で受けられる。
  //  したがって依存は **CommandBuffer.h -> Registry.h の一方向**にし、
  //  両方が完全になったこの位置で本体を書く。
  // ---------------------------------------------------------------------------

  /**
   * @brief 積まれたコマンドを**積んだ順に**適用する (R-34)
   *
   * @details
   *  ## 順序を変えない (R-34)
   *  `Add(e, T)` -> `Destroy(e)` と `Destroy(e)` -> `Add(e, T)` は**別の意味**を
   *  持つ。**並べ替えによる最適化を行わないこと。**
   *
   *  ## 途中で失敗しても止めない (§4 論点4)
   *  `Add` の適用は `SparseSet` の伸長を伴うので失敗し得る。**それでも残りを
   *  適用する。** 途中で止めると、フレームの状態が「どこで失敗したか」に依存する。
   *  さらに悪いことに、後ろに積まれた `Destroy`(メモリを**手放す**操作)を
   *  実行しないことになり、確保が苦しいときに限って解放を取りやめる — 逆向きである。
   *
   *  ## 適用したバッファは空になる
   *  **二重適用を構造的に防ぐため。** 続けてもう一度呼んでも何も起きない。
   *  容量は保たれるので、次のフレームで確保は起きない。
   *
   *  @note 出力はしない (R-28)。何が捨てられたかは `CommandBuffer::Report()` に
   *        あり、**ログに出すのは上層の仕事**である。
   */
  inline void Registry::ApplyCommands(CommandBuffer& buffer) noexcept {
    buffer.AssertOwnerThread();

    ApplyReport report;

    for (std::size_t i = 0; i < buffer.m_commands.GetSize(); ++i) {
      const CommandBuffer::Command& command = buffer.m_commands[i];

      // **生存の検査は入口で 1 回だけ** (§3.1: 門番は Registry ただ 1 つ)
      if (!IsAlive(command.entity)) {
        report.Record(command.kind,
                      (command.kind == CommandKind::Destroy) ? DropReason::AlreadyDestroyed
                                                             : DropReason::DeadEntity,
                      command.entity);
        continue;
      }

      const void* const payload =
          (command.kind == CommandKind::Add)
              ? static_cast<const void*>(buffer.m_payload.GetData() + command.payloadSlot)
              : nullptr;

      DropReason reason = DropReason::AllocationFailed;
      if (command.apply(*this, command.entity, payload, reason)) {
        report.CountApplied();
      }
      else {
        report.Record(command.kind, reason, command.entity);
      }
    }

    buffer.m_commands.Clear();
    buffer.m_payload.Clear();
    buffer.m_report = report;
  }

}
