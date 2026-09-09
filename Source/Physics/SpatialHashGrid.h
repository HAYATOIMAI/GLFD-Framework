#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include "../Core/MemoryResource.h"
#include "../Core/DynamicArray.h"
#include "../ECS/Components.h"

namespace GLFD::Physics {
  class SpatialHashGrid {
  public:
    // グリッドのセルサイズ（パーティクルの直径より少し大きくするのが定石）
    static constexpr float CELL_SIZE = 1.8f;

    // ハッシュテーブルのサイズ
    // 100万個のエンティティに対して衝突率を下げるため大きめに確保
    static constexpr size_t TABLE_SIZE = 2097152;
    static constexpr size_t TABLE_MASK = TABLE_SIZE - 1;

    // 無効なインデックス
    static constexpr uint32_t NULL_INDEX = 0xFFFFFFFF;

    explicit SpatialHashGrid(Memory::IMemoryResource* resource, size_t maxEntities);

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

    // 座標ハッシュ関数
    size_t GetHash(const GLFD::Components::Position& pos) const;

    // 空間座標からハッシュ値を計算 (素数を使ったハッシュ)
    size_t HashCoords(int x, int y, int z) const;
  };
}
