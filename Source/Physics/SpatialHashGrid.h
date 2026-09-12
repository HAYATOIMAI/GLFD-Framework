#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include "../Core/MemoryResource.h"
#include "../Core/DynamicArray.h"
#include "../ECS/Components.h"
#include "../ECS/Entity.h"

namespace GLFD::Physics {
  class SpatialHashGrid {
  public:
    // グリッドのセルサイズ（パーティクルの直径より少し大きくするのが定石）
    static constexpr float CELL_SIZE = 1.8f;

    /**
     * @brief ハッシュテーブルのサイズ (1-7 で 2,097,152 から変更)
     *
     * @details
     *  **以前は 100 万体想定の 2^21 (8 MB) だった。** 1-2 で `MaxEntities` を
     *  1,000,000 から 65,536 へ下げたとき、**この表が取り残された**。
     *  20,000 体に対してバケットが 210 万個、つまり 1 体あたり 105 個あった。
     *
     *  埋めるだけで 1 フレームに約 1,170μs 使っていた(8 MB を 3 箇所で
     *  埋め直していた。実測)。65,536 なら 256 KB で、約 3μs になる。
     *
     *  ## 減らしても近傍探索は遅くならない(実測)
     *  バケットを 4,096 まで減らしても探索費用はほぼ変わらなかった。
     *  1 セルに 35 体いるので、**バケットの衝突に行き当たる前に
     *  `MAX_NEIGHBORS` の打ち切りに当たる**ためである
     *  (20,000 体が実際に占めるセルは 572 個だった)。
     *
     *  ## なぜ 65,536 なのか
     *  速度ではない(16,384 との差は 3μs)。**同じ事故を繰り返さないため**である。
     */
    static constexpr size_t TABLE_SIZE = 65536;
    static constexpr size_t TABLE_MASK = TABLE_SIZE - 1;

    static_assert(TABLE_SIZE >= GLFD::ECS::MaxEntities,
                  "occupied cells can never outnumber the entities, so this keeps the "
                  "load factor at or below 1 even in the worst case. MaxEntities was "
                  "lowered from 1,000,000 to 65,536 in 1-2 and this table was left "
                  "behind for five phases - tie them together so it cannot drift again.");
    static_assert((TABLE_SIZE & TABLE_MASK) == 0,
                  "TABLE_MASK is used instead of a modulo, so the size must be a power of two");

    // 無効なインデックス
    static constexpr uint32_t NULL_INDEX = 0xFFFFFFFF;

    /**
     * @brief バケットと next 配列を確保する
     *
     * @note **確保に失敗しても投げない** (1-6 / R-14 / N-2)。以前は投擲版の
     *       `Resize` を呼んでおり、フレームメモリが尽きると `std::bad_alloc` が
     *       毎フレームの経路から飛んでいた。受ける側もいなかった。
     *       失敗したら `IsReady()` が false になる(`SparseSet` と同じ形)。
     */
    explicit SpatialHashGrid(Memory::IMemoryResource* resource, size_t maxEntities);

    /// 確保できたか。**false のグリッドを使ってはならない**
    [[nodiscard]] bool IsReady() const noexcept { return m_ready; }

    // 毎フレーム呼び出す初期化
    void Clear();

    // エンティティをグリッドに登録 (Thread-Safe / Lock-Free)
    void Insert(uint32_t entityId, const GLFD::Components::Position& pos);

    /**
     * @brief 組んだ時点の「構造版」を控える (ECS 1-5 / R-24)
     *
     * @details
     *  グリッドに入っているのは **dense 添字**であって `Entity` ではない
     *  (1-5 論点3)。dense 添字が有効なのは「組んでから ECS の構造が変わって
     *  いない間」だけである。1-4 で構造変更をフレーム境界へ遅延させたので
     *  この前提は制度として保証されているが、**保証は検査できる形にしておく。**
     *
     *  ここは値を預かるだけで、比較はしない。**`Registry` を知っている側
     *  (システム)が、並列ループへ入る前に 1 回だけ突き合わせる。**
     *  Physics から ECS への依存を作らないためである。
     */
    void SetBuildStamp(uint32_t structureVersion) noexcept { m_buildStamp = structureVersion; }

    /// @copydoc SetBuildStamp
    [[nodiscard]] uint32_t BuildStamp() const noexcept { return m_buildStamp; }

    // 周辺のパーティクルを取得して処理する
    template <typename Func>
    void Query(const GLFD::Components::Position& pos, Func&& func) {
      // **確保できていないグリッドは空として振る舞う** (1-6)。
      // m_buckets が空のまま添字を引くと範囲外になる
      if (!m_ready) {
        return;
      }

      // 自分のいるセルとその周辺(3x3x3)をチェック
      int cx = static_cast<int>(std::floor(pos.x / CELL_SIZE));
      int cy = static_cast<int>(std::floor(pos.y / CELL_SIZE));
      int cz = static_cast<int>(std::floor(pos.z / CELL_SIZE));

      for (int z = cz - 1; z <= cz + 1; ++z) {
        for (int y = cy - 1; y <= cy + 1; ++y) {
          for (int x = cx - 1; x <= cx + 1; ++x) {

            size_t hash = HashCoords(x, y, z);

            // バケットの先頭を取得
            std::atomic_ref<uint32_t> bucketAtomic(m_buckets[hash]);
            uint32_t currentId = bucketAtomic.load(std::memory_order_acquire);

            // リンクリストを辿る
            while (currentId != NULL_INDEX) {
              if (!func(currentId)) {
                return;
              }
              currentId = m_next[currentId];
            }
          }
        }
      }
    }

  private:
    // バケット配列: 各セルの先頭エンティティインデックス
    DynamicArray<uint32_t> m_buckets;

    // Next配列: あるエンティティの次のエンティティインデックス (SoA的なリンクリスト)
    DynamicArray<uint32_t> m_next;

    /// 組んだ時点の ECS の構造版 (1-5)。**このクラスは比較しない**
    uint32_t m_buildStamp = 0;

    /// 確保できたか (1-6)
    bool m_ready = false;

    // 座標ハッシュ関数
    size_t GetHash(const GLFD::Components::Position& pos) const;

    // 空間座標からハッシュ値を計算 (素数を使ったハッシュ)
    size_t HashCoords(int x, int y, int z) const;
  };
}
