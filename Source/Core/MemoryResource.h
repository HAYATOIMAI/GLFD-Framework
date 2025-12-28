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

  /**
   * @brief 標準的なヒープ割り当て（new/delete）を行うリソース
   * @details シングルトンとして利用することを想定
   */
  class HeapResource : public IMemoryResource {
  public:
    [[nodiscard]] void* Allocate(size_t size, size_t alignment) override {
      // 必ずアライメント付きで確保する
#ifdef _MSC_VER
      return _aligned_malloc(size, alignment);
#else
      return std::aligned_alloc(alignment, size);
#endif
    }

    void Deallocate(void* p, size_t, size_t /*alignment*/) override {
      if (p) {
#ifdef _MSC_VER
        _aligned_free(p);
#else
        std::free(p);
#endif
      }
    }

    // シングルトンインスタンスの取得
    static HeapResource* GetInstance() {
      static HeapResource instance;
      return &instance;
    }
  };
  /**
   * @brief デフォルトのメモリリソースを取得する関数
   */
  inline IMemoryResource* GetDefaultResource() { return HeapResource::GetInstance(); }
}