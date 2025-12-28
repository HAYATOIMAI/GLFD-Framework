#pragma once
#include "StackAllocator.h"
#include "MemoryResource.h"

namespace  GLFD::Memory {
  class StackResource : public IMemoryResource {
  public:
    explicit StackResource(StackAllocator& allocator) : m_stackAllocator(allocator) {}

    void* Allocate(size_t sizeBytes, size_t alignment = alignof(std::max_align_t)) override {
      return m_stackAllocator.Allocate(sizeBytes, alignment);
    }
    void Deallocate(void* p, size_t sizeBytes, size_t alignment = alignof(std::max_align_t)) override {
      // スタックアロケータでは個別解放はサポートしないため、何もしない
      // 必要に応じてFreeToMarkerを使用して一括解放を行う設計とする
    }
  private:
    StackAllocator& m_stackAllocator;
  };
}