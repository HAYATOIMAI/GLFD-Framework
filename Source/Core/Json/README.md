# GLFD JSON — 利用ガイド

外部ライブラリ依存ゼロ。例外・RTTI・STL コンテナを使わない。
型ごとに `Serialize` を **1 本**書けば、読み書き両方が手に入る。

```cpp
#include "Core/Json/Json.h"      // 集約ヘッダ(DOM も Reader も入る)
```

---

## 1. 構造体を JSON ファイルから読む

```cpp
#include "Core/Json/Json.h"

// Document は読み込んだ構造体と同じ寿命で持つ。
// 文字列フィールドは Document のアリーナ上にあるため(落とし穴 #1)
Json::Document doc(resource);
GameConfig     config(resource);

Json::ArchiveContext ctx(doc.Arena(), 0, Json::ArchiveFlags::ReportUnknown);

if (Json::LoadFromJsonFile(config, "Resource/GameConfig.jsonc", doc, ctx)) {
    LOG_INFO("config loaded (version %u)", ctx.Version());
}
else {
    // **起動不能にしない。** 既定値のまま続行する
    LOG_WARN("config could not be loaded. using built-in defaults");
}

// 診断は関数から戻った後も読める(doc / ctx が生きている限り)
for (uint32_t i = 0; i < ctx.IssueCount(); ++i) {
    const Json::ArchiveIssue& issue = ctx.Issues()[i];
    // path / detail はヌル終端されている契約なので %s に渡せる
    LOG_WARN("config: %s at '%s' (%s)",
             Json::ToString(issue.kind),
             issue.path.Empty() ? "<root>" : issue.path.Data(),
             issue.detail.Data());
}
```

診断が要らないなら 1 行で書ける。消えるのは診断だけで、`config` の文字列は無事。

```cpp
if (!Json::LoadFromJsonFile(config, path, doc)) { /* 既定値で継続 */ }
```

### 読み込みに失敗しても元の設定を保ちたい場合

`Load*` は同じ `Document` へ読み直すと、**パースを始めた時点で前回の文字列が死ぬ**。
「失敗したら元のまま」を成立させるには、新しい `Document` へ読んで成功時だけ差し替える。

```cpp
auto fresh  = std::make_unique<Json::Document>(resource);
auto loaded = std::make_unique<GameConfig>(resource);

Json::ArchiveContext ctx(fresh->Arena());
const bool ok = Json::LoadFromJsonFile(*loaded, path, *fresh, ctx);
LogIssues(ctx);            // fresh が生きているうちに出す

if (ok) {
    m_configDoc = std::move(fresh);    // 旧 Document はここで捨てられる
    m_config    = std::move(loaded);
}
// 失敗時は fresh が捨てられ、m_config は文字列まで含めて完全に無傷
```

| 形 | 確保 | 失敗時 |
|---|---|---|
| 同じ `Document` を使い回す | **増えない**(内部で `Clear()` = ブロック保持の `Reset()`) | 旧値は失われる |
| 成功時だけ差し替える | ロードごとに `Document` 1 本ぶん | **旧値が完全に保たれる** |

毎フレーム読むなら前者、シーン再入や F5 のような低頻度なら後者。

この形は `Source/Core/GameConfigLoad.h` の `ReloadGameConfig` に置いてある。
**手順を写さずにその関数を呼ぶこと。** 写した実装をテストしても、本番の経路は
一度も実行されない(`JsonRealDataTests` の T-41 は同じ関数を叩いている)。

---

## 2. 構造体を JSON ファイルへ書く

```cpp
if (!Json::SaveToJsonFile(save, "save/slot0.json", resource, kSaveVersion)) {
    // **セーブ失敗はプレイヤーの進行が消えることを意味する。** 握り潰さない
    LOG_ERROR("failed to save");
}
```

`SaveToJsonFile` は一時ファイルへ書いてから `MoveFileEx` で置き換える。
途中で失敗した場合は置き換えを行わず、**既存のセーブは無傷のまま残る**。

Writer を自分で回す場合は `Finish()` の戻り値を必ず見ること。

```cpp
Json::JsonStringBuffer buffer(arena);
Json::JsonWriter<Json::JsonStringBuffer> writer(buffer);
Json::ArchiveContext ctx(arena, kVersion);
{
    Json::WriteArchive ar(writer, ctx);
    Serialize(ar, save);
    if (!ar.Finish()) { /* ここを見落とすと静かに壊れた JSON が残る */ }
}
StringView json = buffer.View();   // ヌル終端ではない
```

エディタ向けの整形出力は `pretty = true`。

```cpp
(void)Json::SaveToJsonFile(data, path, resource, kVersion, /*pretty=*/true);
```

---

## 3. `Serialize` の書き方

読み書き共通で **1 本だけ**。`ar.Member(...)` の呼び出し順がそのまま出力順になる
(並べ替えると git の差分が不安定になる)。

```cpp
struct Weapon {
    int32_t    id     = 0;
    float      damage = 1.0f;
    StringView name;
};

template <class Ar>
void Serialize(Ar& ar, Weapon& v) {
    ar.Member("id",     v.id,     0);      // 第3引数 = 欠損時の既定値
    ar.Member("damage", v.damage, 1.0f);
    ar.Member("name",   v.name);           // 既定値なし = 欠損なら触らない
}
```

### ネスト・配列・列挙・オプション

```cpp
enum class Quality : int32_t { Low = 0, Medium = 10, High = 20 };   // 値を明示する

struct Player {
    Weapon                 weapon;         // ネストしたユーザー型(自動で {} に包まれる)
    float                  position[3] = { 0, 0, 0 };   // T[N] -> JSON 配列
    Quality                quality = Quality::Medium;   // 既定は基底整数型として保存
    std::optional<int32_t> checkpoint;                  // 値なしは null
    DynamicArray<Weapon>   inventory;                   // 可変長配列

    explicit Player(Memory::IMemoryResource* r) : inventory(r) {}
    Player() = delete;                     // 落とし穴 #2 を封じる
};

template <class Ar>
void Serialize(Ar& ar, Player& v) {
    ar.Member("weapon",     v.weapon);
    ar.Member("position",   v.position);   // T[N] に既定値つき Member は使えない
    ar.Member("quality",    v.quality, Quality::Medium);
    ar.Member("checkpoint", v.checkpoint);
    ar.Member("inventory",  v.inventory);
}
```

必須にしたいフィールドは `RequiredMember`。欠損すると `ctx.HasFatal()` が立つ。

```cpp
ar.RequiredMember("id", v.id);
```

バージョンで分岐できる。読み書き非対称な処理は `if constexpr` で。

```cpp
if (ar.Version() >= 3) { ar.Member("newField", v.newField, 0); }
if constexpr (Ar::IsReading()) { /* 旧フォーマットの吸収など */ }
```

**フィールドを改名するときの書き方は §8 にまとめてある。**
比較を `<` で書く規約があるので、自己流で書く前にそちらを読むこと。

列挙を文字列で保存したい場合は `JsonSerializer<E>` を完全特殊化する
(専用の機構は無い。特殊化は既定の部分特殊化より優先される)。

```cpp
namespace GLFD::Json {
  template <> struct JsonSerializer<Difficulty> {
    template <class Ar> static bool Serialize(Ar& ar, Difficulty& v) { /* ... */ }
  };
}
```

---

## 4. セーブだけする場合の最小 include

`Json.h` は集約ヘッダなので DOM も Reader も入る。**書き出ししかしない翻訳単位では
`Json.h` を include しないこと。**

```cpp
#include "Core/Json/JsonWriteArchive.h"   // WriteArchive<W>
#include "Core/Json/JsonArchiveTypes.h"   // スカラ / enum / T[N]
#include "Core/Json/JsonWriter.h"         // JsonWriter / ストリーム
// DynamicArray<T> / std::optional<T> を使うときだけ:
// #include "Core/Json/JsonArchiveContainers.h"
```

この形なら `JsonReader.h` / `JsonDocument.h` / `JsonValue.h` / `DynamicArray.h` /
`HashMap.h` のいずれも入らない(`/showIncludes` で検証済み)。
`float[3]` のような最頻出の型が `JsonArchiveTypes.h` 側にあるのはこのため。

---

## 5. 診断パスで値を引く (`Query`)

診断が出すパスは、**そのまま `Query` に貼れる形**にしてある(R3-9)。
ログを見て「その値が実際に何だったのか」を確かめるまでが 1 ステップで済む。

```
[WARN] config: TypeMismatch at 'window/width' (expected an integer)
```

```cpp
for (uint32_t i = 0; i < ctx.IssueCount(); ++i) {
    const Json::ArchiveIssue& issue = ctx.Issues()[i];

    if (const Json::Value* v = doc.Query(issue.path)) {   // ← パスをそのまま渡す
        StringView text;
        if (v->TryGetString(text)) {
            LOG_WARN("  it was the string \"%.*s\"", (int)text.Size(), text.Data());
        }
    }
}
```

決め打ちで引くこともできる。**見つからなければ `nullptr`**、`assert` はしない。

```cpp
float speed = 2.0f;                                   // 既定値は呼び出し側が持つ
if (const Json::Value* v = doc.Query("simulation/maxSpeed")) { (void)v->TryGetFloat(speed); }

// 部分木からの相対クエリは自由関数で
const Json::Value* profiles = doc.Query("boidProfiles");
const Json::Value* second   = profiles ? Json::Query(*profiles, "1/name") : nullptr;
```

### 解決規則

**現在のノードの型がセグメントの解釈を決める**ので、曖昧さは生じない。

| 現在のノード | セグメントの扱い |
|---|---|
| Object | **キー**として引く。重複キーでは最初の一致 (R2-3) |
| Array | **10進の添字**。`"01"` / `"+1"` / `"-1"` / `" 3"` / `"3x"` / 範囲外はすべて `nullptr` |
| スカラ / Null | 解決不能 → `nullptr` |

キー `"3"` を持つオブジェクトは `"3"` で引けるし、配列に `"name"` を渡せば `nullptr` になる。
**空パスはルート自身**を指す(診断がルート直下の問題に対して空のパスを出すため)。
先頭 `/` / 末尾 `/` / `"//"` はすべて `nullptr` — 受理する形を診断の出力と 1 対 1 にしてある。

### `MissingRequired` だけは `nullptr` になる

必須フィールドの欠損は「**JSON に無い**メンバ」のパスを報告する。したがって `Query` は
`nullptr` を返す。これは欠点ではなく、**「引けない = そのフィールドが無い」と読める**性質。

### 引けないキー(意図的な制約。バグとして直さないこと)

**引けなかったことは `nullptr` で分かる。**

| キー | 位置 | 結果 |
|---|---|---|
| `""`(空キー) | ルート直下 | 診断パスの出力が空文字列でルートと同じ。**ルートが返る** |
| `""`(空キー) | ネストした先 | `{"a":{"":1}}` の出力は `"a/"`。末尾の空セグメントで `nullptr` |
| `"..."` | どこでも | 截断の印(64段を超えたパスの末尾)と衝突する。`nullptr` |

`Query` ではなく**診断パス形式そのものの制約**で、`Query` はそれを引き継いでいる。
RFC 6901 のエスケープは、必要になった時点で検討する。

### `/` を含むキーだけは別 — **静かに間違う**

```
{"a/b": 1, "a": {"b": 2}}      診断が "a/b" を報告 -> Query は 2 を返す
```

`nullptr` にならないので、**引けなかったことに気付けない**。パスの側にエスケープが
無い以上、2つを区別する情報が存在せず、検出する手段も無い。

> **キー名に `/` を使わないこと。** 特に手書きの config ではここが唯一の防波堤になる。

### 性質と範囲

- **確保を一切行わない。** `StringView` のスライスだけで降りる
- 返り値の寿命は `Document` に従う(落とし穴 #1)。`Clear()` / 再 `Parse()` / 破棄で無効になる
- **読み取り専用。** パス指定の代入・ノードの生成は範囲外(レイヤ設定のマージで扱う)
- ワイルドカードやフィルタは**恒久的に範囲外**。JSON パーサが小さなクエリ言語を抱える形になる

---

## 6. 診断ログの読み方

**正常なら 1 行も出ません。** 出ているなら何かがおかしい、と読んでよい設計です。

### パースに失敗したとき

```
[ERR ] Resource/GameConfig.jsonc(4,23): ObjectMissingColon
[WARN] BoidDemoScene: config reload failed. keeping the previous settings
```

MSVC の診断と同じ `file(line,col)` 形式です。**桁はコードポイント単位**なので、
日本語コメントを含む行でもエディタの桁とずれません(バイト単位だとずれます)。
`Logger` は `OutputDebugStringA` にも流すので、デバッガ実行なら
Visual Studio の出力ウィンドウからこの行をダブルクリックで辿れます。

### 読めたが一部おかしいとき(**これが本命**)

型違い・要素数違い・未知フィールド・範囲外は **Fatal ではないのでロードは成功します**。
黙って既定値に落ちるのを防ぐため、必ず出します。

```
[WARN] Resource/GameConfig.jsonc: 3 issue(s), 0 fatal
[WARN]   TypeMismatch         window/width            expected a number
[WARN]   ArrayLengthMismatch  world/bounds            the JSON array does not have the expected element count
[WARN]   UnknownField         simulation/entityCont   the field is not read by Serialize
```

| 読み方 | 意味 |
|---|---|
| `[ERR ]` | **読めなかった** — 必須フィールドの欠損、確保失敗、`StrictTypes` 下の型違い |
| `[WARN]` | **起動はしたが一部おかしい** — その項目だけ既定値のまま |
| 2 列目のパス | **そのまま `doc.Query()` に貼れる**(`<root>` だけは空文字列を渡す) |

`UnknownField` は**綴り間違いの検出そのもの**です。上の例は `entityCount` を
`entityCont` と書いた場合で、値を変えても効かない原因がこの 1 行で分かります。
なお未知フィールドの記録には `ArchiveFlags::ReportUnknown` が要ります
(ゲームのロード経路は既定でゼロコスト、config は明示的に有効化)。

### F5 — 読み直しと差分

```
[INFO] BoidDemoScene: F5 pressed. reloading Resource/GameConfig.jsonc
[INFO] reload: 4 change(s)
[INFO]   window/width                 number -> string
[INFO]   world/bounds                 [2] -> [1]
[INFO]   simulation/maxSpeed          2 -> 6.5
[INFO]   + simulation/entityCont      5
```

DOM 同士の比較なので、整形の違いやキーの順には引きずられません。
変化が無ければ `reload: no change` の 1 行だけです。

### F6 — 実効値のダンプ

**ファイルに書いてある値ではなく、エンジンが実際に使っている値**が出ます。
欠損は既定値で埋まり、未知フィールドは捨てられ、範囲外は拒否されて既定のまま残る
ので、「書いたのに効かない」ときはここを見るのが最短です。

```
[INFO] config in effect (1259 bytes):
[INFO]   {
[INFO]     "$version": 1,
[INFO]     "window": {
[INFO]       "width": 1280,
...
```

> `Game.log` は **UTF-8** です。日本語を含む値(プロファイル名など)を読むときは
> UTF-8 として開いてください。cp932 として開くと文字化けします。

### 自分のコードから使う

`Core/Json/JsonDiagnostics.h` は**行を組み立てるだけ**で、`Logger` を知りません
(出力先は呼び出し側の関心事)。ゲーム側の配線は `Core/GameConfigLog.h` にあります。

```cpp
#include "Core/Json/JsonDiagnostics.h"

// パースの失敗を 1 行にする(source が空なら位置は省かれる)
char         line[Json::kDiagnosticLineCapacity];
const size_t size = Json::FormatParseError(line, sizeof(line), "save.json",
                                           doc.Error().code, doc.Error().offset, doc.Source());

// Issue を 1 行ずつ。**0 件なら 1 回も呼ばれない**
Json::ForEachIssueLine(ctx, "save.json", [](StringView text, bool isFatal) {
    if (isFatal) { LOG_ERROR("%.*s", (int)text.Size(), text.Data()); }
    else         { LOG_WARN ("%.*s", (int)text.Size(), text.Data()); }
});

// 2 つの DOM の差分
Json::DiffValues(before.Root(), after.Root(), [](StringView text) {
    LOG_INFO("%.*s", (int)text.Size(), text.Data());
});
```

**1 行ずつ渡す形になっているのは `Logger` の都合**です — 1 回の呼び出しが
1023 バイトで切れ、時刻とレベルの前置きが付くのは行頭だけなので、
`\n` を詰めた 1 本の文字列は渡せません。確保は一切行いません。

`Document::Source()` は **`ParseFile` で読んだ場合だけ**中身を返します
(`Parse(StringView)` の入力は呼び出し側のバッファなので保持しません)。
`Clear()` / 再 `Parse()` で無効になります。

---

## 7. レイヤ設定(個人の上書き)

```
Resource/GameConfig.jsonc        共有。手書き。日本語コメント付き。Git 管理下
Resource/GameConfig.local.json   個人の上書き値だけ。Git 管理外(.gitignore 済み)
```

`.local.json` に**書いた値だけ**が `.jsonc` を上書きします。得られるものは 2 つ。

1. **コメントが失われない。** 機械が手書きファイルに触らないため(書き戻しで
   コメントを保持する CST は恒久的にスコープ外)
2. **自分のチューニングを commit しない。** `maxSpeed` を弄ったまま push する事故が
   構造的に消える

```jsonc
// GameConfig.local.json — 雛形は GameConfig.local.json.sample
{
  "simulation": { "maxSpeed": 6.5 },
  "window":     { "width": 1600 }
}
```

### 仕組み — DOM はマージしない

**同じ構造体へ 2 回読みます。** 2 回目に `ArchiveFlags::Overlay` を立てるだけです。

```cpp
ConfigDiagnosticsLogger diagnostics;
const bool ok = ReloadGameConfig(baseDoc, localDoc, config,
                                 kGameConfigPath, kGameConfigLocalPath,
                                 resource, diagnostics);
```

R3-4 により既定値なしの `Member` はキーが無ければ値に触らないので、上書きの意味論は
**自然に**成立します。`Overlay` が抑えるのは邪魔をする 2 つだけです。

| | `Overlay` 無し(1 回目) | `Overlay` あり(2 回目) |
|---|---|---|
| 既定値つき `Member` でキーが無い | `v = def` を代入 | **何もしない**(1 回目の値が残る) |
| `RequiredMember` でキーが無い | `MissingRequired` で Fatal | **記録しない**(断片に無いのは正常) |
| `UnknownField` / `TypeMismatch` など | 記録する | **記録する**(綴り間違いは検出したい) |

### 上書きの規則

| 対象 | 規則 |
|---|---|
| オブジェクト | **深くマージ**。`"window"` に `"width"` だけ書けば `"height"` は `.jsonc` の値が残る |
| `DynamicArray` | **丸ごと置換**。書いていない要素のフィールドは**構造体の既定値**になる |
| `T[N]`(固定長) | **全要素を書いたときだけ**上書きされる。数が合わなければ上書きは無視され `ArrayLengthMismatch` が出る |
| 型が違う | 上書きは失敗し、**`.jsonc` の値が残る**。`TypeMismatch` が出る |
| `$version` | **`.jsonc` 側を採用。** `.local.json` に書いても使われず、警告が 1 行出る |

配列を「添字ごとにマージ」しないのは、**配列の添字が要素の同一性を表さない**ためです
(並び替えれば 1 番目は別の要素になる)。部分的に混ぜると「なぜこの値が来たのか」を
実装の内部事情なしに説明できなくなります。

### 失敗したとき

| 状況 | 挙動 |
|---|---|
| `.local.json` が**無い** | **正常**。診断も出ない(これが既定の状態) |
| `.local.json` が壊れている | `.jsonc` の結果を保ち、`file(line,col)` 付きで診断を出す |
| `.jsonc` が壊れている | 従来どおり。差し替えを行わず、直前の設定を保つ |

診断は**ファイルごとに `ArchiveContext` が分かれている**ので、ログのヘッダ行を見れば
どちらの由来かが分かります。

```
[WARN] Resource/GameConfig.local.json: 1 issue(s), 0 fatal
[WARN]   UnknownField         simulation/maxSpeeed    the field is not read by Serialize
```

F5 の差分も**ファイルごと**に出ます(方式 B は「マージ後の DOM」を持たないため)。

---

## 8. バージョン移行の書き方

**動いている実例が `Source/Core/GameConfig.h` にある**(v1 -> v2 の改名)。
次にやる人はそこと `Tests/JsonVersionMigrationTests.cpp` を見るのが早い。

`$version` はルートの予約キーで、読み込み時に `ar.Version()` で引ける (R3-7)。
これを見て `Serialize` の中で分岐する (R3-8)。

### 手順

1. **`Serialize` に分岐を足す。** 旧名は読み方向にしか現れない
2. **`kGameConfigVersion` を上げる**
3. **出荷ファイルの `$version` を上げ、新しい形へ書き換える**
4. **旧版のフィクスチャをテストに残す**(消すと移行の経路が二度と走らない)

```cpp
template <class Ar>
void Serialize(Ar& ar, WorldConfig& v) {
  if (ar.Version() < 2u) {
    // v1 は "bounds" という名前だった。**読み方向でのみ意味がある**
    if constexpr (Ar::IsReading()) { (void)ar.Member("bounds", v.halfExtent); }
  }
  else {
    (void)ar.Member("halfExtent", v.halfExtent);
  }
  // ...
}
```

### 規約 — 比較は必ず `<` で書く

`ar.Version() == 1` と書いてはいけない。将来 v3 を足したとき、
**`== 1` の分岐は v1 -> v3 の移行で漏れる**。`< 2` なら「2 より前のすべて」を
意味するので、**移行が累積して合成される**。

1 回目の移行では違いが出ないが、規約を先に決めておかないと後から全部書き直す
ことになる。加えて `$version` を書き忘れたファイルは `0` として読まれるので、
`== 1` はその場合も外れる(実際、`<` を `==` に変える変異は T-62 の
「`$version` 不在」と「壊れた `$version`」の 2 ケースで落ちる)。

### 書き出しは常に最新版

移行は**一方向**である。`SaveToJson` / `SaveToJsonFile` には常に
`kGameConfigVersion` を渡すこと。古い版を渡すと**移行分岐のどちらの枝も通らず、
フィールドが静かに落ちる**。Debug ビルドでは `GameConfig` の `Serialize` が
`assert` で止める。

### 版の境界

| ファイルの `$version` | 挙動 |
|---|---|
| 現在より大きい | **読み込まない。** `ReloadGameConfig` が `false` を返し、既定値で継続する |
| 現在と同じ | そのまま読む |
| 現在より小さい | 移行して読む。`LOG_WARN` が 1 行出る |
| 不在 (`0`) | 最も古い形式として読む。`LOG_WARN` が 1 行出る |
| 型が違う / 32 ビットに収まらない | `TypeMismatch` を記録し `0` として扱う(Fatal ではない) |

未来の版を拒否するのは、**その版でフィールドの意味が変わっていた場合に誤った値を
静かに使う**ことになるため。ロード失敗は既定値で継続できるが、誤った値での継続は
説明できない (R1-1f)。実機の出力はこうなる。

```
[WARN] Resource/GameConfig.jsonc: migrated $version 1 -> 2 (world.bounds -> world.halfExtent)
[WARN] Resource/GameConfig.jsonc: writing this file out will produce the v2 form
[INFO] config loaded: Resource/GameConfig.jsonc (version 1)

[ERR ] Resource/GameConfig.jsonc: $version 99 is newer than this build (max 2). the file is not loaded
[WARN] config could not be loaded (Resource/GameConfig.jsonc). falling back to built-in defaults
```

### `.local.json` との関係

上書き断片は**自前の版を持たない**。基底 (`.jsonc`) と**同じ版で読む** (R2-18q)。
したがって `.local.json` には、その `.jsonc` と同じ名前で書く。基底が v1 のままなら
`.local.json` にも旧名で書くことになる。`.local.json` に `$version` を書いても
使われず、警告が 1 行出る。

### 現在の設計で書けないこと

`Serialize` 内の分岐で書けるのは、**同じ親の中の**改名・追加・削除である。

**フィールドの親を移動する**移行は、旧親を開き直す補助構造体を書けば形の上では
可能だが、`ReportUnknown` と噛み合わない。未消費フィールドを覚えるビットマスクは
枠を積むたびに新しく確保され `EndObject` で掃き出されるため、**2 回目に開いた枠は
1 回目の消費を知らず、旧親の他のキーを全部 `UnknownField` として報告する**。
回避するには旧親の全フィールドを持つ v1 構造体をコードに残すことになる。

同じ理由で、**旧キーの読み捨て**(削除の移行)も捨て先のダミー変数が要る。
また `$` 始まりの予約キーは移行分岐からも触れない(`LocateMember` が `assert` する)。

## 踏みやすい落とし穴

### #1 `Document` を破棄すると文字列も診断も死ぬ

読み取った `StringView`・`Value`・`ArchiveIssue::path` は**すべて `Document` の
アリーナ上**にある。`Document` を関数ローカルにして構造体だけ返すと、
文字列フィールドがまとめてダングリングする。**`Document` は読み込んだ構造体と
同じ寿命で保持すること。**

同じ `Document` へ読み直した場合も同じ。`Load*` は内部で `Document::Clear()` を
行うため(繰り返し読んでも確保が積み上がらないようにするため)、
**読み直した時点で前回の文字列は死ぬ。**

`Json.h` が `Document` を隠すエントリポイントを提供しないのは、隠す版があると
「簡単な方を使ったら文字列が壊れる」という最も踏まれやすい形が残るため。

### #2 `DynamicArray` メンバには `IMemoryResource` が要る

`DynamicArray` は既定構築するとリソース未設定になり、読み込み時に
`ContainerFailure` で静かに空配列になる。**リソースを取るコンストラクタだけを
残し、既定構築を封じる**こと。忘れたらコンパイルエラーになる。

```cpp
struct Config {
    explicit Config(Memory::IMemoryResource* r) : items(r) {}
    Config() = delete;
    DynamicArray<Item> items;
};
```

`DynamicArray<T>` の `T` に **`DynamicArray` メンバを持つ型を入れることはできない**
(フェーズ1 のスコープ外)。踏むと `ContainerFailure` になり、パスと detail で
原因が分かるようになっている。`DynamicArray<DynamicArray<T>>` は動く。

### #3 整数で保存した `enum` は列挙子の値を変えると壊れる

分かれ目はスコープ付きかどうかではなく、**値を明示しているか**。

```cpp
enum class LogLevel { Info, Warning, Error };     // 暗黙 0,1,2 -> 危険
enum KeyCode : int { Space = VK_SPACE, ... };     // 明示      -> 安全
```

前者は列挙子を途中に挿入しただけで、旧データの `1` が別の意味になる。
**セーブデータや config に乗る列挙は値を明示すること。**

### #4 `T[N]` は読み書きで長さが変わり得る

読みは短い配列を許容する(足りない分は触らない)が、書きは常にちょうど `N` 要素を出す。
手書き config の `float[4]` に `[1, 2]` と書くと、書き戻した時点で `[1, 2, 0, 0]` になる。
要素数が合わないときは `ArrayLengthMismatch` が記録される(Fatal ではない)。

### #5 `SkipDefaults` は `RequiredMember` に効かない

必須フィールドを既定値と一致するからと省略すると、読み戻したときに
`MissingRequired` で Fatal になる。**フラグ 1 つでセーブデータが壊れる罠**なので
明示的に除外してある。既定値つき `Member` で受けるフィールドだけが省略対象。

### #6 `Serialize` は書き方向で値を変更してはならない

`Serialize(Ar&, T&)` は読み書き共通のため非 const 参照を取る。書き出しの
エントリポイントは `const T&` を受け取り、内部で 1 回だけ `const_cast` する。
書き方向で `v` を書き換える `Serialize` を書くと、**本当に const なオブジェクトを
保存したときに未定義動作**になる。

### #7 `StringView` はヌル終端を保証しない

`buffer.View()` や `Value::GetString()` の結果を `const char*` として Win32 API や
`%s` に渡してはならない。`%.*s` に長さを添えて渡すこと。
例外は `ArchiveIssue::path` / `detail` で、こちらは診断専用なのでヌル終端を契約している。

---

## 計測

```
Tests\run_benchmark.bat release 2000
```

`Resource/GameConfig.jsonc` を使って、パース / ロード / セーブの時間と
アリーナ使用量・`IMemoryResource::Allocate` の回数を出す。
絶対値の目標は無く、最適化の前後で比較するためのもの(N-6)。
