#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include "StackAllocator.h"

namespace GLFD::Memory {
  class IMemoryResource {
  public:
    virtual ~IMemoryResource() = default;
    /**
     * @brief メモリを確保します。
     * @param sizeBytes 確保するサイズ
     * @param alignment アライメント
     * @return 確保されたメモリへのポインタ
     */
    [[nodiscard]] virtual void* Allocate(size_t size, size_t alignment) = 0;
    /**
     * @brief メモリを解放します。
     * @param p 解放するポインタ
     * @param sizeBytes (確保時に指定した) サイズ
     * @param alignment (確保時に指定した) アライメント
     */
    virtual void Deallocate(void* p, size_t size, size_t alignment) = 0;
  };

}