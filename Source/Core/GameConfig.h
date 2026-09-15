#pragma once

/**
 * @file  GameConfig.h
 * @brief 起動時に JSON から読み込むチューニング設定(フェーズ 1-7 で移行)
 *
 * @details
 *  以前は `static constexpr` の定数群だった。JSON 化にあたり実行時の値へ変えている。
 *  実体は `Resource/GameConfig.jsonc`(JSONC。コメントと末尾カンマを許可)。
 *
 *  @note **既定構築を封じてある。** `boidProfiles`(`DynamicArray`)は既定構築すると
 *        `IMemoryResource` 未設定になり、読み込み時に `ContainerFailure` で
 *        静かに空配列になる(R3-29 の経路)。リソースを取るコンストラクタだけを
 *        残すことで、配線を忘れたらコンパイルエラーになる。
 *
 *  @note `StringView` のフィールドは、読み込みに使った `Json::Document` の
 *        アリーナ上にある(R0-5)。**`Document` を `GameConfig` と同じ寿命で
 *        保持すること。** `GameEngine` と `BoidDemoScene` はどちらもそうしている。
 *
 *  @note 追加した値には JSON 側に既定値を持たせてある(R3-4)。
 *        したがって**古い設定ファイルでも読める**。逆にフィールドを消しても
 *        未知フィールドとして無視される(R3-5)。
 */

#include <cassert>
#include <cstdint>

#include "DynamicArray.h"
#include "MemoryResource.h"
#include "StringView.h"

// 値域違反を `ArchiveIssue` として記録するために要る。**軽い**ヘッダで、
// `JsonReader.h` / `JsonDocument.h` / `JsonValue.h` のいずれも引き込まないので
// README §4(セーブだけする場合の最小 include)の約束は壊れない
#include "Json/JsonArchive.h"

namespace GLFD {

  /// 設定ファイルの既定の置き場所
  inline constexpr const char* kGameConfigPath = "Resource/GameConfig.jsonc";

  /**
   * @brief 個人の上書き設定 (2-3)。**Git 管理外で、無いのが普通**
   * @note  ここに書いた値だけが `kGameConfigPath` を上書きする。
   *        手書きの共有ファイルに機械が触らないので、コメントが失われない
   */
  inline constexpr const char* kGameConfigLocalPath = "Resource/GameConfig.local.json";

  /**
   * @brief このコードが読み書きする設定のバージョン (R3-7)
   *
   * @note **上げたら `Serialize` に移行分岐を足すこと** (A-2)。
   *       比較は必ず `<` で書く。`ar.Version() == 1` と書くと、v3 を足したときに
   *       v1 -> v3 の移行で漏れる。`< 2` なら「2 より前のすべて」なので、
   *       移行が累積して合成される
   *
   * @note v1 -> v2: `world.bounds` を `world.halfExtent` へ改名した (A-2)
   */
  inline constexpr std::uint32_t kGameConfigVersion = 2;

  /**
   * @brief `fileVersion` をこのビルドが読めるか (A-2 論点2)
   *
   * @details
   *  **未来の版は拒否する。** 黙って読むと、その版でフィールドの意味が変わって
   *  いた場合に**誤った値を静かに使う**ことになる。ロード失敗は既定値で継続
   *  できるが、誤った値での継続は説明できない(R1-1f と同じ判断)。
   *
   *  @note 判定を自由関数にしてあるのは、`ReloadGameConfig`(実行経路)と
   *        `ConfigDiagnosticsLogger`(ログ)とテストが**同じ 1 本**を呼ぶため。
   *        写した判定をテストしても意味がない
   */
  [[nodiscard]] inline constexpr bool
  IsSupportedGameConfigVersion(std::uint32_t fileVersion) noexcept {
    return fileVersion <= kGameConfigVersion;
  }

  /// 読んだファイルが古く、移行分岐を通ったか (A-2)。`$version` 不在 (`0`) も含む
  [[nodiscard]] inline constexpr bool
  GameConfigWasMigrated(std::uint32_t fileVersion) noexcept {
    return fileVersion < kGameConfigVersion;
  }

  /**
   * @brief `fileVersion` から現在の版へ上げたときに何が変わったかの 1 行 (A-2)
   *
   * @note 文言を**スキーマの持ち主側**に置いてある。ログを出すのは
   *       `GameConfigLog.h` だが、移行を足す人が 2 つのファイルを探さずに済む。
   *       多段になったら `fileVersion` から現在までの各段を連ねる形へ変えること
   */
  [[nodiscard]] inline constexpr const char*
  GameConfigMigrationNote(std::uint32_t fileVersion) noexcept {
    if (fileVersion < 2u) { return "world.bounds -> world.halfExtent"; }
    return "";
  }

  // ---------------------------------------------------------------------------

  /**
   * @brief Boid 1 体ぶんの「性格」
   * @note  **スカラのみで構成する。** `DynamicArray` メンバを持たせると
   *        `DynamicArray<BoidProfile>` に入れたときリソースが配れない(R3-32)
   */
  struct BoidProfile {
    StringView name;
    float      viewRadius       = 5.0f;
    float      separationWeight = 1.5f;
    float      alignmentWeight  = 1.0f;
    float      cohesionWeight   = 1.0f;
  };

  template <class Ar>
  void Serialize(Ar& ar, BoidProfile& v) {
    (void)ar.Member("name", v.name);
    (void)ar.Member("viewRadius", v.viewRadius, 5.0f);
    (void)ar.Member("separationWeight", v.separationWeight, 1.5f);
    (void)ar.Member("alignmentWeight", v.alignmentWeight, 1.0f);
    (void)ar.Member("cohesionWeight", v.cohesionWeight, 1.0f);
  }

  /// 起動時にしか効かない設定(ウィンドウは実行中に作り直さない)
  struct WindowConfig {
    std::int32_t width  = 1024;
    std::int32_t height = 768;
    StringView   title  = StringView("ECS Boids Engine");
  };

  template <class Ar>
  void Serialize(Ar& ar, WindowConfig& v) {
    (void)ar.Member("width", v.width, 1024);
    (void)ar.Member("height", v.height, 768);
    (void)ar.Member("title", v.title);
  }

  struct WorldConfig {
    /**
     * @brief 画面の**半分**の寸法。`{ x, y }`
     * @note  位置は `±halfExtent` でクランプされる(`BoidDemoScene` の折り返し)。
     *        v1 では `bounds` という名前だったが、**全体の幅と読めてしまう**。
     *        `85` と書いた人は 85 幅の箱を期待するが、実際は 170 幅になる。
     *        v2 で改名した (A-2)。`RenderSystem` が `1.0f / 85.0f` を直書きして
     *        いるのも、この値が半寸法であることの裏付けである
     */
    float halfExtent[2]    = { 85.0f, 64.0f };
    /// 初期配置の一様分布の範囲。`{ min, max }`
    float spawnRange[2]    = { -20.0f, 20.0f };
    /// 初期速度の一様分布の範囲。`{ min, max }`
    float velocityRange[2] = { -0.5f, 0.5f };
    float colliderRadius   = 0.3f;
  };

  template <class Ar>
  void Serialize(Ar& ar, WorldConfig& v) {
    // `T[N]` に既定値つきの Member は使えない(配列は代入できない)。
    // 既定値は構造体側の初期化子が持つ (R3-26)

    // --- v1 -> v2 の移行 (A-2) -----------------------------------------------
    // **比較は `<` で書く。`== 1` にしないこと。** v3 を足したとき、`== 1` の
    // 分岐は v1 -> v3 の移行で漏れる。`< 2` なら「2 より前のすべて」を意味する
    // ので、移行が累積して合成される。
    //
    // 移行は**一方向**である。読むときだけ旧名を見て、書くときは常に新名で出す。
    // したがって旧名は `IsReading()` の側にしか現れない
    if (ar.Version() < 2u) {
      if constexpr (Ar::IsReading()) { (void)ar.Member("bounds", v.halfExtent); }
    }
    else {
      (void)ar.Member("halfExtent", v.halfExtent);
    }

    (void)ar.Member("spawnRange", v.spawnRange);
    (void)ar.Member("velocityRange", v.velocityRange);
    (void)ar.Member("colliderRadius", v.colliderRadius, 0.3f);
  }

  /// `simulation.entityCount` の既定値。**3 箇所で要るので定数にしてある**
  /// (構造体の初期化子 / `Member` の既定値 / 値域違反からの復帰先)
  inline constexpr std::int32_t kDefaultEntityCount = 20000;

  /**
   * @brief `simulation.entityCount` に書ける上限 (ECS-0 1-3)
   *
   * @details
   *  **`ECS::MaxEntities`(疎配列の構造的な限界)そのものは使わない。**
   *  検査値を構造的限界に置くと、境界の実装が正しいことに依存してしまう。
   *
   *  32,768 の根拠 (1-2):
   *   - `MaxEntities` = 65,536 の**半分**。`MaxEntities` 自体が小さくなったので
   *     ECS-0b の「1 桁の余裕」は過剰で、半分でも境界の off-by-one には届かない
   *   - 出荷値 20,000 に対して 1.6 倍。実験で 3 万体まで試せる
   *
   *  @note **「1 桁の余裕」という以前の規約はここでは採らない。** 根拠が変わった
   *        のに検査だけ残すと、次の人が守れない規約を守ろうとする
   *
   *  @note `ECS::MaxEntities` との大小関係は `BoidDemoScene.cpp` の
   *        `static_assert` が守っている。`Core` から `ECS` へ依存させないため、
   *        両方を include している翻訳単位に置いてある
   */
  inline constexpr std::int32_t kMaxConfigurableEntityCount = 32768;

  struct SimulationConfig {
    std::int32_t entityCount = kDefaultEntityCount;
    float        timeStep    = 0.016f;
    float        maxSpeed    = 2.0f;
  };

  template <class Ar>
  void Serialize(Ar& ar, SimulationConfig& v) {
    (void)ar.Member("entityCount", v.entityCount, kDefaultEntityCount);

    // --- entityCount だけの値域検査 (ECS-0 1-3) --------------------------------
    // Release では `SparseSet::Emplace` と `DynamicArray::operator[]` の `assert`
    // が消えるため、ここで弾かないと**設定ファイルから疎配列の範囲外書き込みに
    // 到達できる**。今日の Release で到達可能な唯一の欠陥だったので塞いである。
    //
    // **汎用の値域検証機構は作らない。** この 1 件に限定する。
    // `RangeOverflow` は Fatal ではない (R3-11) ので、ロード自体は成功し、
    // 他のフィールドはそのまま読まれる
    if constexpr (Ar::IsReading()) {
      if (v.entityCount < 0 || v.entityCount > kMaxConfigurableEntityCount) {
        // `detail` は静的な文字列リテラルを指す契約なので、**フィールド名を
        // 文言に入れる**。診断パスの方は `simulation` になる —
        // `Member` が戻った時点で `entityCount` の `PathScope` は降りている
        ar.Context().Report(
            Json::ArchiveErrorKind::RangeOverflow,
            StringView("simulation/entityCount is outside the supported range"));
        v.entityCount = kDefaultEntityCount;
      }
    }

    (void)ar.Member("timeStep", v.timeStep, 0.016f);
    (void)ar.Member("maxSpeed", v.maxSpeed, 2.0f);
  }

  struct InteractionConfig {
    float explosionRadius = 20.0f;
    float explosionForce  = 50.0f;
    /// スクリーン座標 → ワールド座標の倍率。RenderSystem と合わせる
    float screenScale     = 6.0f;
  };

  template <class Ar>
  void Serialize(Ar& ar, InteractionConfig& v) {
    (void)ar.Member("explosionRadius", v.explosionRadius, 20.0f);
    (void)ar.Member("explosionForce", v.explosionForce, 50.0f);
    (void)ar.Member("screenScale", v.screenScale, 6.0f);
  }

  // ---------------------------------------------------------------------------

  struct GameConfig {
    /**
     * @param resource `boidProfiles` の確保元。**`GameConfig` より長生きすること**
     * @note  既定構築は封じてある(ファイル冒頭の @note を参照)
     */
    explicit GameConfig(Memory::IMemoryResource* resource) : boidProfiles(resource) {}
    GameConfig() = delete;

    WindowConfig              window;
    WorldConfig               world;
    SimulationConfig          simulation;
    InteractionConfig         interaction;
    /// `boidProfiles` が空のときに使う性格
    BoidProfile               defaultBoid;
    /// エンティティへ順番に配る性格。空なら `defaultBoid` を全体に使う
    DynamicArray<BoidProfile> boidProfiles;

    /**
     * @brief 起動するシーン (ECS 1-8)。`"boids"`(既定)か `"survivor"`
     * @note  **起動時にしか効かない**(`window.*` と同じ)。実行中のシーン切り替えは
     *        無い(`EventBus` に購読解除が無く、抜けたシーンを購読者が呼ぶ。1-8 §1-A)。
     *        個人で切り替えるなら `GameConfig.local.json` に書く。共有の既定を変えずに済む
     */
    StringView                startScene = StringView("boids");
  };

  /**
   * @brief 設定全体の列挙 (R3-1)。読み書き共通でこの1本だけ
   * @warning `Ar::IsWriting()` のとき `v` を変更してはならない。
   *          書き出しのエントリポイントは `const GameConfig&` を受け取り
   *          内部で `const_cast` するため、破ると未定義動作になる
   */
  template <class Ar>
  void Serialize(Ar& ar, GameConfig& v) {
    // **書き出しは常に最新版で行うこと** (A-2 §2.1)。古い版を渡すと移行分岐の
    // どちらの枝も通らず、`world` のフィールドが 1 つ**静かに落ちる**。
    // コメントだけでは足りない(実際に 1 箇所で踏んでいた)ので機械的に止める。
    // 読み側は逆で、**古い版が来るのが正常**なので何も言わない
    if constexpr (Ar::IsWriting()) {
      assert(ar.Version() >= kGameConfigVersion
             && "GameConfig must be written with kGameConfigVersion; an older version "
                "silently drops the fields that migration renamed");
    }

    // R3-3: この呼び出し順がそのまま出力順になる。並べ替えないこと
    (void)ar.Member("window", v.window);
    (void)ar.Member("world", v.world);
    (void)ar.Member("simulation", v.simulation);
    (void)ar.Member("interaction", v.interaction);
    (void)ar.Member("defaultBoid", v.defaultBoid);
    (void)ar.Member("boidProfiles", v.boidProfiles);
    // **既定値を渡さない。** キーが無ければ初期化子 ("boids") のまま触らない (R3-4)。
    // したがって古い設定ファイルも、この欄を持たない JSON のテストもそのまま読める
    (void)ar.Member("startScene", v.startScene);
  }

}
