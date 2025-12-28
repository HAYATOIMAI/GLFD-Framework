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
}