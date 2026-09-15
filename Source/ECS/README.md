# GLFD ECS — 利用ガイド

ヘッダだけで完結する。例外・RTTI・STL コンテナを使わない。確保はすべて
`Memory::IMemoryResource` 経由で、失敗は戻り値で返る(毎フレームの処理では投げない)。

```cpp
#include "ECS/Registry.h"        // エンティティと成分
#include "ECS/View.h"            // 反復
#include "ECS/CommandBuffer.h"   // 反復中の構造変更
```

> **ここに載っている例は `Tests/EcsGuideTests.cpp` で実際にコンパイルし、書いてあるとおりに
> 動くかを確かめている。** 節とテストのケースは 1 対 1 に対応する。**片方を変えたらもう片方も変えること。**
> 例は `namespace GLFD` の中で書いている。

---

## 1. エンティティを作って成分を足す

```cpp
struct Health { float current; };   // 成分は普通の構造体(ゲーム側で定義する)

ECS::Registry registry(resource);

const ECS::Entity e = registry.CreateEntity();
if (!e.IsValid()) {
    // 上限 (ECS::MaxEntities = 65,536) か確保失敗。**戻り値を必ず見る** (R-10)
}

// AddComponent は T* を返す。確保失敗・既に持っている・死んだハンドルで nullptr (R-25)
if (registry.AddComponent<Components::Position>(e, 1.0f, 2.0f, 0.0f, 0.0f) == nullptr
    || registry.AddComponent<Health>(e, 10.0f) == nullptr) {
    registry.DestroyEntity(e);   // 作りかけを残さない
}

Components::Position* const pos = registry.GetComponent<Components::Position>(e);   // 無ければ nullptr
const bool hasHealth = registry.HasComponent<Health>(e);
```

---

## 2. `View<Ts...>` で反復する

### 単一

```cpp
float sumX = 0.0f;
for (auto [e, pos] : registry.View<Components::Position>()) {
    (void)e;
    sumX += pos.x;
}
```

### 複数

**最小のプールを基準に回り、他の成分は存在を確認する** (R-22)。組み合わせを持たない
エンティティは飛ばされる。

```cpp
int moving = 0;
for (auto [e, pos, vel] : registry.View<Components::Position, Components::Velocity>()) {
    (void)e;
    pos.x += vel.vx;             // 参照なので書き換えられる
    ++moving;
}
```

### `Slice` で並列に投げる

**`View` はメインスレッドで作り、値でジョブへ渡す。** `Slice(first, last)` は
**その `View` の範囲の先頭からの相対位置**で、切った `View` をさらに切っても意味が保たれる。

```cpp
auto view = registry.View<Components::Position, Components::Velocity>();
const std::size_t half = view.BaseSize() / 2;

Thread::JobCounter counter;
auto handle = jobs.CreateHandle(counter);
jobs.KickJob([view, half]() {
    for (auto [e, pos, vel] : view.Slice(0, half)) { (void)e; pos.x += vel.vx; }
}, &handle);
jobs.KickJob([view, half]() {
    for (auto [e, pos, vel] : view.Slice(half, view.BaseSize())) { (void)e; pos.x += vel.vx; }
}, &handle);
jobs.WaitFor(handle);
```

**並列のジョブでは成分の値を書き換えるだけにする。** 構造(生成・破棄・成分の追加削除)は
変えない (R-18)。

---

## 3. 反復中に構造を変える(`CommandBuffer`)

**反復中に `Registry` の即時 API を呼ばない。** 破棄や追加はバッファへ積み、フレームの境界で
まとめて適用する。**積むだけなら dense 配列は 1 ミリも動かない**ので、反復は壊れない。

```cpp
ECS::CommandBuffer commands(resource);   // メインスレッドで作る。作ったスレッドが所有者 (R-18)

for (auto [e, hp, pos] : registry.View<Health, Components::Position>()) {
    if (hp.current > 0.0f) { continue; }
    if (!commands.Destroy(e)) { continue; }            // 積めなかった: 次のフレームで再挑戦する

    const ECS::Entity drop = registry.CreateEntity();  // **生成だけは即時** (R-17)。反復は壊れない
    if (drop.IsValid()) {
        (void)commands.Add(drop, Components::Position{ pos.x, pos.y, 0.0f, 0.0f });
        (void)commands.Add(drop, Pickup{ 1.0f });      // 成分の追加は遅延
    }
}

registry.ApplyCommands(commands);                      // フレーム境界でまとめて効く
const ECS::ApplyReport& report = commands.Report();    // 適用した数 / 捨てた数と理由
```

- `CommandBuffer::Add` に渡せるのは **trivially copyable** な成分だけ(`memcpy` で積むため)。
  そうでない成分は反復の外で `Registry::AddComponent` を使う
- 適用時に捨てられたもの(死んだハンドルへの追加、二重破棄など)は **黙って消えず**
  `Report()` に残る (R-20)

---

## 4. 破棄と、死んだハンドルの扱い

```cpp
registry.DestroyEntity(target);                              // 反復の外なら即時でよい

const bool alive = registry.IsAlive(target);                 // false
const bool valid = target.IsValid();                         // **true のまま**(無効値ではないが、死んでいる)
const Health* const hp = registry.GetComponent<Health>(target);   // nullptr

const ECS::Entity next = registry.CreateEntity();             // 空いた index は再利用される
// next.Index() == target.Index() でも世代が違う。**古いハンドルでは新しい住人に触れない**
```

**生きているかを答えるのは `Registry::IsAlive` だけ。** `Entity::IsValid()` は
「無効値ではない」ことしか言わない (R-3)。

---

## 5. 落とし穴

- **反復中に `Registry` の即時 API(`DestroyEntity` / `AddComponent` / `RemoveComponent`)を呼ばない。**
  swap-and-pop で dense の並びが動き、反復が飛ばしと重複を起こす。Debug では
  `View::IsStale()` の `assert` が検出する (R-24 / R-36)。反復中は `CommandBuffer` へ積む
- **`Entity` を `Registry` の外で組み立てない。** `Entity::Make` は公開されているが、
  空き枠の現世代で組み立てたハンドルは `IsAlive` で区別できない (§15.4)
- **構造変更はメインスレッドのみ** (R-18)。`CommandBuffer` は作ったスレッドが所有者で、
  `IsOwnerThread()` が同じ判定を公開している
- **`View::BaseSize()` は構築時の範囲を覚える** (§18.7)。「いま何件あるか」を知りたければ
  `View` を取り直す。また **返る件数ではなく基準プールの範囲**で、組み合わせを持たない
  ものも数に入る
- **グリッドの寿命はフレーム内** (R-43)。`SpatialHashGrid` は組んだ時点の構造版を
  `BuildStamp` に控える。**組んだ後に即時 API で構造を変えると照合が発火する** —
  生成は `GridBuild` より前に置くか、`CommandBuffer` 経由にする
- **グリッドの番号は「組んだ `View` の基準プールの dense 添字」。** 引く側は**同じ型の並びの
  `View`** で持ち主を引く。基準プールが違うと別のエンティティとして読み、症状は
  **誤った命中ではなく命中の見逃し**として出る(1-8 の変異 M7b)
- **`CollisionSystem` では完全に同じ位置の 2 体は衝突しない** (§20.7)。押し返しの
  `dx / dist` を守るイプシロン(`distSq > 0.0001f`)のため。敵を固定の 1 点に湧かせると
  重なったまま離れない。`Game/HitSystem` は割り算をしないのでこの条件を持たない
- **購読者が積んだ破棄は、配信を適用の前に置かないと 2 フレーム遅れる** (§17.1)。
  `Game/SurvivorLoop.h` は `DispatchEvents` を `ApplyCommands` の前に置いている
- **遅延の帰結として、倒した直後の経験値は 1 フレーム後にしか拾えない** (§5.3)。
  成分が付くのは `ApplyCommands` の後で、同じフレームの回収処理からは見えない。
  **欠陥ではなく設計の帰結**である(T-ECS-23 が固定している)
