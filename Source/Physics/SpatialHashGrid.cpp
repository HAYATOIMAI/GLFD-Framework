#include "SpatialHashGrid.h"


namespace GLFD::Physics {
  SpatialHashGrid::SpatialHashGrid(Memory::IMemoryResource* resource, size_t maxEntities) :
    m_buckets(resource)
    , m_next(resource) {
    // スタックアロケータからメモリ確保。**投げない** (1-6 / R-14 / N-2)

     // バケット配列（各セルの先頭エンティティID）
     //
     // **最初から番兵で埋める** (1-7)。以前は 0 埋めのままで、番兵は
     // 0xFFFFFFFF だった。つまり構築直後は**全バケットの末尾にエンティティ 0 が
     // ぶら下がった状態**で、Clear() を呼ばずに引くと嘘の近傍が返っていた
     // (ECS-0 1-4)。1-7 で構築が 1 回になり Clear() の呼び出しが消えたので、
     // ここが正しくないと即座に表に出る
    if (!m_buckets.TryResize(TABLE_SIZE, NULL_INDEX)) {
      return;                       // m_ready は false のまま
    }

    // リンクリスト用のNext配列（エンティティ数分）
    if (!m_next.TryResize(maxEntities)) {
      return;
    }
    m_ready = true;
  }

  void SpatialHashGrid::Clear() {
    // **確保できていなければ何もしない** (1-6)。以前の形のままだと
    // m_buckets が空のときに nullptr へ 8 MB の memset をかけていた
    if (!m_ready) {
      return;
    }

    // バケットを全て「空」にする
    void* ptr = m_buckets.GetData();
    std::memset(ptr, 0xFF, TABLE_SIZE * sizeof(uint32_t));
  }

  void SpatialHashGrid::Insert(uint32_t entityId, const GLFD::Components::Position& pos) {
    // 境界チェック
    if (entityId >= m_next.GetSize()) {
      // IDが範囲外なら無視する（あるいはログを出してアサート）
      // 範囲外書き込みによる隣接メモリ(EventBus等)の破壊を防ぐ
      return;
    }

    size_t hash = GetHash(pos);

    std::atomic_ref<uint32_t> bucketAtomic(m_buckets[hash]);

    // Atomicなリンクリスト挿入
    uint32_t prevHead = bucketAtomic.exchange(entityId, std::memory_order_acq_rel);
    m_next[entityId] = prevHead;
  }

  size_t SpatialHashGrid::GetHash(const GLFD::Components::Position& pos) const {
    int x = static_cast<int>(std::floor(pos.x / CELL_SIZE));
    int y = static_cast<int>(std::floor(pos.y / CELL_SIZE));
    int z = static_cast<int>(std::floor(pos.z / CELL_SIZE));
    return HashCoords(x, y, z);
  }

  size_t SpatialHashGrid::HashCoords(int x, int y, int z) const {
    // 大きな素数を掛けてXORをとる
    const size_t h1 = 73856093;
    const size_t h2 = 19349663;
    const size_t h3 = 83492791;

    size_t n = (static_cast<size_t>(x) * h1) ^
      (static_cast<size_t>(y) * h2) ^
      (static_cast<size_t>(z) * h3);

    return n & TABLE_MASK;
  }
}