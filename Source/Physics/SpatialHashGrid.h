#pragma once
#include <atomic>
#include <vector>
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

    // 座標ハッシュ関数
    size_t GetHash(const GLFD::Components::Position& pos) const;

    // 空間座標からハッシュ値を計算 (素数を使ったハッシュ)
    size_t HashCoords(int x, int y, int z) const;
  };
}
