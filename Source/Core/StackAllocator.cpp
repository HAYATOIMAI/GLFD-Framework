#include "StackAllocator.h"

namespace  GLFD::Memory {
  StackAllocator::StackAllocator(size_t sizeBytes) : m_totalSize(sizeBytes) {
   // std::malloc で生メモリ確保
#ifdef _MSC_VER
    void* ptr = _aligned_malloc(m_totalSize, 64);
#else
    void* ptr = std::aligned_alloc(64, m_totalSize);
#endif

    if (!ptr) throw std::bad_alloc();

    m_startPtr = static_cast<std::byte*>(ptr);
    m_offset = 0;
  }

  StackAllocator::~StackAllocator() {
    if (m_startPtr) {
#ifdef _MSC_VER
      _aligned_free(m_startPtr);
#else
      std::free(m_startPtr);
#endif
    }
  }

  void* StackAllocator::Allocate(size_t size, size_t alignment) {
    size_t currentAddress = reinterpret_cast<size_t>(m_startPtr) + m_offset;
    size_t padding = GetAlignmentPadding(currentAddress, alignment);

    if (m_offset + padding + size > m_totalSize) {
      // デバッグ時はアサート、リリース時はnullptr返却
      assert(false && "StackAllocator Overflow");
      return nullptr;
    }

    size_t nextAddress = currentAddress + padding;
    m_offset += padding + size;

    return reinterpret_cast<void*>(nextAddress);
  }

  void StackAllocator::FreeToMarker(Marker marker) noexcept {
    // 巻き戻すだけでメモリ内容は消さない（高速化のため）
    // 実際のデータは次の割り当てで上書きされる
    if (marker <= m_offset) {
      m_offset = marker;
    }
    else {
      assert(false && "Invalid marker passed to FreeToMarker");
    }
  }
}