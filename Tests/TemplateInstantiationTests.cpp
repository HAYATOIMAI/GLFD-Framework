/**
 * @file  TemplateInstantiationTests.cpp
 * @brief E-1: 未実体化テンプレートの型チェック監査
 *
 * @details
 *  **テンプレートは実体化されるまで型チェックされない。** JSON の 1-5 で `HashMap` を
 *  使おうとしたとき、3 件の欠陥が同時に見つかった。当時 `HashMap.h` を
 *  `#include` していた 2 箇所(`ShaderManager.h` と `main.cpp`)は include して
 *  いるだけで、リポジトリ内に実体化が 1 件も無かったためである。
 *  **その 2 箇所は第1段・第2段で無くなり、`HashMap.h` の include は 0 件になった。**
 *
 *  **このファイルはコンパイルが通ること自体が検査である。** 実行時の振る舞いは
 *  見ていない。`main()` の各ケースは「その実体化を含む翻訳単位がコンパイルできた」
 *  ことを 1 件記録するだけで、値を確かめてはいない(**そう読めるように書かないこと**)。
 *
 *  @note **見つかった欠陥をここで直さない** (E-1 §7)。直すと「どこまで直したか」が
 *        曖昧になり、報告の価値が下がる。コンパイルできない型は削除せず
 *        `#if 0` + エラーの実物 + 原因を残す (E-1 §2.3)。削除すると
 *        「そもそも監査対象に無かった」ように見え、次の人が同じ調査をやり直す。
 *
 *  @note **除外したもの。** `Source/Core/Json/` の 21 ヘッダは 19 スイートで
 *        実体化済みなので対象外。**JSON 由来だが `Json/` の外にある 2 件
 *        (`GameConfig.h` の `Serialize<Ar>` 6 本、`GameConfigLoad.h` の
 *        `ReloadGameConfig<OnIssues>`)も同じ理由で除外済み** —
 *        ディレクトリによる機械的な線引きでは拾えないので明記しておく。
 *
 *  @note `Thread::ThreadPool::Enqueue<F>` も未実体化だが**載せていない**。
 *        `ThreadPool` 自体の利用者が 0 件で `JobSystem` が別に存在するため、
 *        これは「未実体化テンプレート」ではなく未使用サブシステムの問題であり、
 *        性質が違う。§3 の報告側で扱う。
 *
 *  @note このスイートは `/EHsc` でビルドされる。**`/EH` 無しの C4530 は別途
 *        同じ TU をゲーム本体のフラグでコンパイルして採取する** (E-1 §2.4)。
 *        こちらのテストコード自身は `throw` を書かない(自前の例外で C4530 を
 *        出してしまうと、どのテンプレートが原因か分からなくなる)。
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。
 */

#include <cstddef>
#include <cstdio>

#include <functional>
#include <string>
#include <type_traits>

// --- エンジンのヘッダ ---------------------------------------------------------
//
// **`/W4 /WX` を通すために局所的に抑えている。直したのではない** (E-1 §7)。
// 監査そのものが走らなくなるので抑えるだけで、中身は §3 の報告に挙げてある。
// ゲーム本体は `/W3` なので、これらはこれまで一度も出ていなかった。
//
//   C4324 `Source/Threading/LockFreeQueue.h(135)`
//     `alignas(CacheLineSize)` を付けた `m_head` / `m_tail` でパディングが入る。
//     false sharing を避けるための**意図した**アラインメントなので実害は無い
//
// 抑制の範囲を include の前後だけに閉じてあるので、**このファイルが書く
// コードには効かない**(監査対象の警告を自分で握り潰さないため)。
//
// **抑制は直ったら外すこと。** 直った警告の抑制を残すと、同じ警告が再発しても
// ここで検出できなくなる。`Source/Core/StackResource.h` の C4100 ×3 は
// 第2段で `[[maybe_unused]]` を付けて直したので、その抑制はもう無い
#pragma warning(push)
#pragma warning(disable : 4324)   // structure was padded due to alignment specifier

#include "Core/DynamicArray.h"
#include "Core/StackAllocator.h"
#include "Core/StackResource.h"
#include "ECS/Registry.h"
#include "ECS/View.h"
#include "Events/EventBus.h"
#include "Resource/ResourceManager.h"

#pragma warning(pop)

#include "MockMemoryResource.h"
#include "TestHarness.h"

// ---------------------------------------------------------------------------
// 実体化に使う 3 系統の型 (E-1 §2.2)
//
//   `int` だけで通すと多くの欠陥が隠れる。コピー / ムーブ経路と `noexcept` の
//   嘘は、ユーザー定義の特殊メンバを持つ型でしか出ない。
// ---------------------------------------------------------------------------

namespace AuditTypes {

  /// (1) トリビアルな POD
  struct Pod {
    int   a = 0;
    float b = 0.0f;
  };

  /// (2) 非トリビアル。コピー / ムーブ / デストラクタがユーザー定義
  class NonTrivial {
  public:
    NonTrivial() : m_owned(new int(0)) {}
    explicit NonTrivial(int v) : m_owned(new int(v)) {}

    NonTrivial(const NonTrivial& other) : m_owned(new int(*other.m_owned)) {}
    NonTrivial& operator=(const NonTrivial& other) {
      if (this != &other) { *m_owned = *other.m_owned; }
      return *this;
    }

    NonTrivial(NonTrivial&& other) noexcept : m_owned(other.m_owned) {
      other.m_owned = nullptr;
    }
    NonTrivial& operator=(NonTrivial&& other) noexcept {
      if (this != &other) {
        delete m_owned;
        m_owned       = other.m_owned;
        other.m_owned = nullptr;
      }
      return *this;
    }

    ~NonTrivial() { delete m_owned; }

    [[nodiscard]] int Value() const { return m_owned != nullptr ? *m_owned : -1; }

  private:
    int* m_owned;
  };

  /**
   * @brief (3) **ムーブが `noexcept` でない**型 (要件書 R3-28 の `Throwy` 相当)
   * @note  実際には投げない。**投げると自前の例外で C4530 が出てしまい**、
   *        どのテンプレートが原因か分からなくなる。`noexcept(false)` である
   *        ことだけが要る — コンテナはこれを見て強い保証の経路を選ぶ
   */
  class Throwy {
  public:
    Throwy() = default;
    explicit Throwy(int v) : m_value(v) {}

    Throwy(const Throwy&)            = default;
    Throwy& operator=(const Throwy&) = default;

    Throwy(Throwy&& other) noexcept(false) : m_value(other.m_value) {}
    Throwy& operator=(Throwy&& other) noexcept(false) {
      m_value = other.m_value;
      return *this;
    }

    ~Throwy() = default;

    [[nodiscard]] int Value() const { return m_value; }

  private:
    int m_value = 0;
  };

  static_assert(!std::is_nothrow_move_constructible_v<Throwy>,
                "Throwy must keep a throwing move; that is the whole point of it");
  static_assert(std::is_nothrow_move_constructible_v<NonTrivial>,
                "NonTrivial is the well-behaved counterpart");

}

// ===========================================================================
// 1. GLFD::HashMap<K,V> — **コンパイルできない**
// ===========================================================================
//
// 要件書 §5.4.2 の (a)(b)(c) と相互参照。実体化が 1 件も無いためコンパイラを
// 一度も通っていない。以下は `template class GLFD::HashMap<int, int>;` を
// `/W4 /WX /EHsc /GR-` でコンパイルしたときの実物である。
//
//   HashMap.h(1): error C2220: the following warning is treated as an error
//   HashMap.h(1): warning C4819: The file contains a character that cannot be
//                 represented in the current code page (932)
//
//   HashMap.h(370): warning C4189: 'commit': local variable is initialized but
//                   not referenced
//     -> Rehash() 内。要件書 §5.4.2 (d)(無意味な try/catch)の残骸
//
//   HashMap.h(158): error C2662:
//     'HashMap<int,int,...>::ForwardIterator<false> HashMap<...>::begin(void)':
//     cannot convert 'this' pointer from 'const GLFD::HashMap<int,int,...>'
//     to 'GLFD::HashMap<int,int,...> &'
//   HashMap.h(158): note: Conversion loses qualifiers
//     -> 同じエラーが end() でも出る。発生箇所は HashMap.h(142) の
//        **コピーコンストラクタ** `HashMap(const HashMap& other)`。
//        原因: begin() const / end() const が無い(const_iterator の
//        **型別名だけ**があり、それを返すメンバが存在しない)。
//        したがって `const HashMap&` は一切イテレートできず、
//        コピーコンストラクタが実体化できない -> 要件書 §5.4.2 (b)
//
//        HashMap.h(191) の operator=(const HashMap&) がコピーコンストラクタを
//        呼ぶため、代入も巻き添えで壊れている。
//
// ゲーム本体のフラグ(`/EH` 無し)では、上記に加えて
//
//   HashMap.h(362): warning C4530: C++ exception handler used, but unwind
//                   semantics are not enabled
//     -> Rehash() の try/catch。要件書 §5.4.2 (d) と同じ場所
//
// **直したらこの #if 0 を外すこと。** 外して通れば修正が本物である証拠になる。
#if 0
template class GLFD::HashMap<int, int>;
template class GLFD::HashMap<int, AuditTypes::NonTrivial>;
#endif

// ===========================================================================
// 2. GLFD::ECS::View<Ts...>
// ===========================================================================
//
// **E-1 時点の状況(記録として残す)**: `ECS::View<Components...>` は
// クラステンプレートとして一度も実体化されていなかった。当時コード中にあった
// `registry.View<Components::Position>()` 10 箇所は、**戻り値が
// `SparseSet<T>&` の同名の別物**(`Registry::View<T>`)であり、
// E-1 の棚卸しで実際に誤判定しかけた。
//
// **1-5 で同名衝突は解消した (R-21)。** `Registry::View<Ts...>` はここにある
// クラステンプレートを返すようになり、`SparseSet<T>&` を返す API は消えた。
// したがって「同名の別物」はもう存在しない。
//
// エンジン側は `View<Position, Velocity>` などで実体化するようになったが、
// **3 系統の監査型では通らない**ので、ここで通し続ける。
// 明示的実体化は全ての非テンプレートメンバを実体化し、入れ子の `Iterator` と、
// そこから呼ばれる私有のメンバ関数テンプレート
// (`Assure` / `SmallestPool` / `FetchImpl` / `FetchOne`)も巻き込む。
// `SparseSet<T>::GetEntityList()` もここで通る(呼び出し元は `View` だけ)。

template class GLFD::ECS::View<AuditTypes::Pod, AuditTypes::NonTrivial>;
template class GLFD::ECS::View<AuditTypes::Throwy>;

// ===========================================================================
// 3. GLFD::ECS::SparseSet<T> / GLFD::DynamicArray<T>
// ===========================================================================
//
// どちらもエンジン内で実体化済みだが、**3 系統の型では通っていない**。
// `DynamicArray<T>::Resize` はエンジン内のどの実体化からも呼ばれておらず、
// C4530 の筆頭候補である(要件書 R3-28 と同じ経路)。

template class GLFD::ECS::SparseSet<AuditTypes::Pod>;
template class GLFD::ECS::SparseSet<AuditTypes::NonTrivial>;
template class GLFD::ECS::SparseSet<AuditTypes::Throwy>;

template class GLFD::DynamicArray<AuditTypes::Pod>;
template class GLFD::DynamicArray<AuditTypes::NonTrivial>;
template class GLFD::DynamicArray<AuditTypes::Throwy>;

// ===========================================================================
// 4. GLFD::Events::EventChannel<T>
// ===========================================================================
//
// `EventBus::Subscribe<T>` が唯一の呼び出し元である `EventChannel<T>::Subscribe`
// を巻き込むため、2 段で未実体化のものが 2 つ分通る。

template class GLFD::Events::EventChannel<AuditTypes::Pod>;
template class GLFD::Events::EventChannel<AuditTypes::NonTrivial>;

// ===========================================================================
// 5. GLFD::Resource::ResourceStorage<T>
// ===========================================================================
//
// `ResourceManager::GetByKey<T>` / `Release<T>` の先にある。どちらの
// メンバテンプレートも呼び出し 0 件だった。

template class GLFD::Resource::ResourceStorage<AuditTypes::Pod>;
template class GLFD::Resource::ResourceStorage<AuditTypes::NonTrivial>;

// ---------------------------------------------------------------------------
// メンバ関数テンプレート
//
// **明示的実体化では展開されない** (E-1 §2.1)。ODR 使用して実体化させる。
// アドレスを取るだけで定義が実体化される
// ---------------------------------------------------------------------------

namespace {

  using AuditTypes::NonTrivial;
  using AuditTypes::Pod;
  using AuditTypes::Throwy;

  // --- Memory::StackAllocator::New<T, Args...> ---
  // メモリ層。壊れていたときの影響が最も広い
  Pod*        (GLFD::Memory::StackAllocator::* const kNewPod)()          = &GLFD::Memory::StackAllocator::New<Pod>;
  NonTrivial* (GLFD::Memory::StackAllocator::* const kNewNonTrivial)()   = &GLFD::Memory::StackAllocator::New<NonTrivial>;
  Throwy*     (GLFD::Memory::StackAllocator::* const kNewThrowy)()       = &GLFD::Memory::StackAllocator::New<Throwy>;
  // 引数転送の経路も通す(`Args...` が空でない形)
  Pod*        (GLFD::Memory::StackAllocator::* const kNewPodArgs)(int&&, float&&)
    = &GLFD::Memory::StackAllocator::New<Pod, int, float>;
  NonTrivial* (GLFD::Memory::StackAllocator::* const kNewNonTrivialArgs)(int&&)
    = &GLFD::Memory::StackAllocator::New<NonTrivial, int>;

  // --- Events::EventBus::Subscribe<T> ---
  // 2-1: the callback is a plain function pointer now, not std::function.
  // N-1 (18.5) targets types that own, allocate or throw, and std::function is one:
  // a capture larger than MSVC's inline buffer allocates and can throw. Measured
  // in the 2-2 audit, where no C4530 was produced by any of it.
  GLFD::Events::SubscriptionId (GLFD::Events::EventBus::* const kSubscribePod)(
      void*, GLFD::Events::EventChannel<Pod>::EventCallback)
    = &GLFD::Events::EventBus::Subscribe<Pod>;
  GLFD::Events::SubscriptionId (GLFD::Events::EventBus::* const kSubscribeNonTrivial)(
      void*, GLFD::Events::EventChannel<NonTrivial>::EventCallback)
    = &GLFD::Events::EventBus::Subscribe<NonTrivial>;

  // --- Resource::ResourceManager::GetByKey<T> / Release<T> ---
  Pod* (GLFD::Resource::ResourceManager::* const kGetByKeyPod)(const std::string&)
    = &GLFD::Resource::ResourceManager::GetByKey<Pod>;
  NonTrivial* (GLFD::Resource::ResourceManager::* const kGetByKeyNonTrivial)(const std::string&)
    = &GLFD::Resource::ResourceManager::GetByKey<NonTrivial>;
  void (GLFD::Resource::ResourceManager::* const kReleasePod)(GLFD::Resource::ResourceHandle<Pod>)
    = &GLFD::Resource::ResourceManager::Release<Pod>;
  void (GLFD::Resource::ResourceManager::* const kReleaseNonTrivial)(GLFD::Resource::ResourceHandle<NonTrivial>)
    = &GLFD::Resource::ResourceManager::Release<NonTrivial>;

  // --- ECS::View<...> ---
  // **`View<...>::Get<T>` は 1-5 で無くなった。** 成分は反復子が参照で返すように
  // なり(R-23)、`Registry::GetComponent` 経由の二度引きも無くなったため。
  // 新しい `View` に**公開のメンバ関数テンプレートは 1 つも無い**ので、
  // ここで名指しする対象も無い。私有のもの
  // (`Assure` / `SmallestPool` / `PoolAt` / `FetchImpl` / `FetchOne`)は
  // 非テンプレートメンバから呼ばれるので、上の明示的実体化で通っている。
  //
  // 代わりに**反復子まわりを名指しで固定**する。ここが通らなければ
  // 範囲 for が壊れている
  using AuditView = GLFD::ECS::View<Pod, NonTrivial>;
  AuditView::Iterator (AuditView::* const kViewBegin)() const = &AuditView::begin;
  AuditView::Iterator (AuditView::* const kViewEnd)()   const = &AuditView::end;
  AuditView (AuditView::* const kViewSlice)(size_t, size_t) const = &AuditView::Slice;
  AuditView::Entry (AuditView::Iterator::* const kViewDeref)() const
    = &AuditView::Iterator::operator*;

  // --- DynamicArray<T>::Resize ---
  // 非テンプレートメンバなので上の明示的実体化で既に通っているが、**エンジン内の
  // どの実体化からも呼ばれていない**ことが分かっているので名指しで残す
  void (GLFD::DynamicArray<Pod>::* const kResizePod)(size_t)               = &GLFD::DynamicArray<Pod>::Resize;
  void (GLFD::DynamicArray<NonTrivial>::* const kResizeNonTrivial)(size_t) = &GLFD::DynamicArray<NonTrivial>::Resize;
  void (GLFD::DynamicArray<Throwy>::* const kResizeThrowy)(size_t)         = &GLFD::DynamicArray<Throwy>::Resize;

  // --- const 版のアクセス ---
  // `HashMap` の欠陥はこれで出た。`const` の経路は別物として通す必要がある
  void AuditConstAccess() {
    GLFD::Test::MockMemoryResource mock;

    const GLFD::DynamicArray<Pod>        pods(&mock);
    const GLFD::DynamicArray<NonTrivial> nonTrivials(&mock);
    const GLFD::DynamicArray<Throwy>     throwies(&mock);

    // const な参照からイテレートできること。**コピーもできること**
    std::size_t seen = 0;
    for (const auto& item : pods)        { (void)item; ++seen; }
    for (const auto& item : nonTrivials) { (void)item; ++seen; }
    for (const auto& item : throwies)    { (void)item; ++seen; }

    const GLFD::DynamicArray<Pod>        podsCopy(pods);
    const GLFD::DynamicArray<NonTrivial> nonTrivialsCopy(nonTrivials);

    (void)seen;
    (void)podsCopy.GetSize();
    (void)nonTrivialsCopy.GetSize();
  }

  /// 監査で実体化した対象の数(**コンパイルが通ったこと**の記録にしか使わない)
  void RecordCompiled(const char* what) {
    GLFD::Test::BeginCase(what);
    // ここで確かめているのは値ではない。**この翻訳単位がコンパイルできたこと**
    // そのものが検査である (E-1 §4)
    CHECK(true);
  }

}

// ===========================================================================
// 監査の棚卸し (E-1 §1-A) と、その後の処置
// ===========================================================================
//
// **消して無かったことにしない。** 次に監査する人が「昔あったものが消えたのか、
// 最初から無かったのか」を区別できるようにするためである (§2.3 と同じ理由)。
//
// --- この TU が実体化して型チェックしているもの -------------------------------
//
//   GLFD::ECS::View<Components...>          完全未実体化だった。ここで通した
//   GLFD::ECS::SparseSet<T>                 3 系統の型で通した
//   GLFD::DynamicArray<T>                   3 系統の型で通した
//   GLFD::Events::EventChannel<T>           EventBus::Subscribe<T> ごと通した
//   GLFD::Resource::ResourceStorage<T>      GetByKey<T> / Release<T> ごと通した
//   GLFD::Memory::StackAllocator::New<T,..> メンバ関数テンプレート
//   GLFD::ECS::View<...>::begin/end/Slice   1-5 で Get<T> の代わりに置いた
//   GLFD::ECS::View<...>::Iterator::operator*
//
// --- 1-6 で消えたもの(**消して無かったことにしない**)-------------------------
//
//   GLFD::Core::Instrumentor        Profiler.h / Profiler.cpp ごと削除 (1-6)。
//   GLFD::Core::InstrumentationTimer  BeginSession の呼び出し 0 件、PROFILE_SCOPE /
//   PROFILE_FUNCTION (マクロ)         PROFILE_FUNCTION の使用も 0 件だった。
//   GAMELIB_PROFILE (マクロ)          include だけが 4 ファイルに残っていた
//                                     (Engine.cpp / BoidSystems.h / RenderSystem.h /
//                                      CollisionSystem.h)。R-27 の実例。
//
//     **証拠**: リポジトリにあった results.json は 14 種類のスコープ名 x 697
//     フレームで、**そのどれも現在のソースに存在しなかった**。うち 1 つが
//     "GDI Present (StretchDIBits)" で、**DX11 化より前の計測**だと分かる。
//     リファクタのたびに PROFILE_SCOPE が消え、マクロ定義と include が残った。
//
//     **残す知見(実装は消すが、知見は消さない)**:
//      - 出力は **Chrome Tracing 形式** ({"traceEvents":[{cat,dur,name,ph,pid,tid,ts}]})。
//        chrome://tracing または Perfetto でそのまま開ける
//      - 実装例は Instrumentor::WriteProfile の 20 行だった
//      - **EcsBenchmark (1-5) では代替できない**部分がある: あちらはヘッドレスで
//        決定的な「分布」(中央値/最小/p95)を出すが、**スレッド別のタイムライン**
//        (tid 付き)は出せない。BoidSystem の p95 がなぜ跳ねるかは
//        タイムラインにしか出ない。必要になったら作り直すこと
//      - **作り直すときは N-1〜N-4 に合わせること**。旧実装は全部に抵触していた
//        (std::string / std::ofstream / 関数ローカル static / 生の new)
//
// --- 1-5 で消えたもの(**消して無かったことにしない**)-------------------------
//
//   GLFD::ECS::View<...>::Get<T>   1-5 で廃止。成分は反復子が参照で返す (R-23)。
//                                  代わりに begin/end/Slice/operator* を固定した
//   GLFD::Registry::View<T>        1-5 で廃止 (R-19 / R-21)。`SparseSet<T>&` を
//                                  返す API そのものが無くなり、E-1 で誤判定
//                                  しかけた「同名の別物」は解消した
//
// --- コンパイルできないため #if 0 で記録しているもの --------------------------
//
//   GLFD::HashMap<K,V>   ファイル上部の #if 0 とエラーの実物を参照。
//                        **削除しない。** R3-13a により「将来 IMemoryResource 上へ
//                        作り替える」対象として残す。要件書 §5.4.2 に欠陥の記録が
//                        あり、消すとその記録が宙に浮く
//
// --- 削除済み(第2段)---------------------------------------------------------
//
// いずれも **include 0 件**だった。利用者が存在しないものを維持する価値は低い
// (HashMap をスコープ外にしたときと同じ基準)。通したところで、次に使おうとする
// ときには要求が変わっていて書き直しになる。
//
//   Source/Graphics/ShaderManager.h   削除。対応する .cpp が無く、メンバは宣言
//     だけで定義が存在しなかった。「未実体化」以前に実装されていない。
//     <string>/<vector>/<unordered_map>/<filesystem> に依存 (N-1 抵触)
//
//   Source/Core/ObjectPool.h          削除。**監査ではコンパイルが通っていた**
//     (`ObjectPool<Pod>` / `<NonTrivial>` とも、出るのは DynamicArray 由来の
//     C4530 だけ)。壊れていたから消したのではなく、利用者が 0 件だから消した。
//     IMemoryResource を経由するかどうかは N-1 に照らして再検討が要る
//
//   Source/Template/Singleton.h       削除。**監査ではコンパイルが通っていた**。
//     グローバル状態を持つパターンで N-3 と方向が逆。
//     (`GLFD::Template::Singleton<T>`。`GLFD::Singleton` ではない —
//      名前空間を間違えると「壊れている」と誤判定する。実際に一度やった)
//
//   Source/main.cpp:3 の #include "Core/HashMap.h"   削除。main() は HashMap を
//     使っていなかった。削除後も main.cpp は単体でコンパイルできる
//
// --- 直したもの(第1段)-------------------------------------------------------
//
//   Source/Core/StackResource.h   Deallocate の C4100 x3。[[maybe_unused]] を付けた
//   Source/Core/HashMap.h         C4819。BOM 無し UTF-8 だったので BOM を足した
//                                 (cp932 ではない。再エンコードは不要だった)
//
//   Source/Core/DynamicArray.h    `EmplaceBack` の多重定義を 1 本にした。
//     制約なし版と `requires std::constructible_from<T, Args...>` 版が同じ
//     シグネチャで並んでいた。**「無条件に到達不能」ではなかった** —
//     制約が満たされないときだけ制約なし版が選ばれ、`std::construct_at` で
//     分かりにくく落ちていた。削除の効果はデッドコードの除去ではなく、
//     **誤用が制約違反として直接報告されるようになったこと**である
//
// --- C4530 の読み方(重要)-----------------------------------------------------
//
// **MSVC は C4530 を翻訳単位につき 1 回しか出さない**(最初に当たった箇所だけ)。
// この TU 全体をゲームのフラグでコンパイルすると 1 件しか出ないが、
// 「1 箇所しかない」という意味ではない。対象ごとに TU を分けて測ると、
// `DynamicArray.h` の中だけで 3 箇所ある(行番号は第1段の削除後):
//
//   DynamicArray.h  Resize(size_type)          — T を問わず出る
//   DynamicArray.h  値埋めの経路               — 非トリビアルな T で出る
//   DynamicArray.h  TryReallocate(size_type)   — **ムーブが noexcept でない T**
//
// 最後の 1 つは `if constexpr` で「ムーブが nothrow でなく、コピーできる」場合に
// だけ選ばれる枝である。`noexcept(kNothrowRelocate)` は
// `is_nothrow_move_constructible_v<T>` そのものなので、**noexcept の宣言は嘘では
// ない**(1-5a / R3-28 の設計どおり)。出るのは「例外前提のコードが `/EH` 無しで
// コンパイルされている」という事実だけである。

int main() {
  GLFD::Test::BeginSuite("TemplateInstantiation (E-1)");

  // 実体化はすべて翻訳単位のスコープで済んでいる。ここは通ったことの記録である
  RecordCompiled("E-1/1-5: ECS::View<Ts...> instantiates for the audit types");
  RecordCompiled("E-1: ECS::SparseSet<T> instantiates for POD / non-trivial / throwing-move");
  RecordCompiled("E-1: DynamicArray<T>::Resize instantiates for all three families");
  RecordCompiled("E-1: Events::EventChannel<T> and EventBus::Subscribe<T> instantiate");
  RecordCompiled("E-1: Resource::ResourceStorage<T>, GetByKey<T> and Release<T> instantiate");
  RecordCompiled("E-1: Memory::StackAllocator::New<T, Args...> instantiates");

  // const 経路だけは実行しないと ODR 使用にならないメンバがあるので一度通す
  AuditConstAccess();
  RecordCompiled("E-1: const access (copy construction and iteration) instantiates");

  // GLFD::HashMap<K,V> は **コンパイルできない**。ファイル上部の #if 0 と
  // そこに貼ったエラーの実物を参照すること (E-1 §2.3)
  RecordCompiled("E-1: HashMap<K,V> is recorded as NOT compilable (see the #if 0 block)");

  return GLFD::Test::Summarize();
}
