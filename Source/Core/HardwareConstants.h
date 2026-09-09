#pragma once
#include <new>
#include <thread>

namespace GLFD::System {
  struct Constants {
    // 異なるスレッドが同じキャッシュライン上のデータを操作して競合するのを防ぐサイズ
    // C++17の機能だが、未実装コンパイラへのフォールバックを用意
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t CacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t CacheLineSize = 64;
#endif
  };

  /**
   * @brief ジョブを分割する本数。**0 を返さない**
   *
   * @note `std::thread::hardware_concurrency()` は「値が計算できない場合は 0 を
   *       返す」と規定されている。5 つのシステムが `count / threadCount` を
   *       無条件に計算していたため、**0 が返るとゼロ除算**になっていた
   *       (ECS-0 3-7)。1 へ丸める。
   *       各システムで書くと同じ判定が 5 箇所に散るのでここに 1 本置く
   */
  [[nodiscard]] inline unsigned int WorkerThreadCount() noexcept {
    const unsigned int detected = std::thread::hardware_concurrency();
    return (detected != 0u) ? detected : 1u;
  }
}