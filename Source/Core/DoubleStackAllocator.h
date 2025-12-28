#pragma once
#include <utility>
#include "StackAllocator.h"

namespace GLFD::Memory {
  class DoubleStackAllocator {
  public:
   explicit DoubleStackAllocator(size_t sizeBytes) 
     : m_stackA(sizeBytes)
     , m_stackB(sizeBytes)
     , m_current(&m_stackA)
     , m_previous(&m_stackB) {
   }

   // 現在のフレーム用アロケータを取得
   StackAllocator& GetCurrent() { return *m_current; }

   // 前のフレーム用アロケータを取得（必要なら）
   StackAllocator& GetPrevious() { return *m_previous; }

   // フレームの境界で呼び出す
   void SwapAndReset() {
     // ポインタを入れ替える (Ping-Pong)
     std::swap(m_current, m_previous);

     // 新しく現在になった方をリセット（空にする）
     m_current->Clear();
   }

  private:
    StackAllocator m_stackA;
    StackAllocator m_stackB;

    StackAllocator* m_current;
    StackAllocator* m_previous;
  };
}