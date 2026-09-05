#pragma once

/**
 * @file  MockMemoryResource.h
 * @brief テスト用の IMemoryResource モック(確保失敗の注入 / リーク検出)
 *
 * @details
 *  もともと Tests/JsonArenaTests.cpp の中に定義されていたものを、フェーズ 1-2 で
 *  共有ヘッダへ切り出した。切り出しにあたり **JsonArenaTests.cpp のテスト本体は
 *  1 行も変更しておらず、チェック数も Debug 844 / Release 847 のまま**である。
 *  この不変性が「純粋な移動であって挙動を変えていない」ことの根拠になっている。
 *
 *  このモックは 1-3 以降の全フェーズのメモリ検証(確保失敗の注入・リーク検出・
 *  確保回数の計測)で使われる共有資産である。
 *
 *  @warning **改変する場合は、必ず 1-1 のテストが同一チェック数
 *           (Debug 844 / Release 847) で通ることを確認すること。**
 *           チェック数が変われば、それは移動ではなく挙動の変更を意味する。
 *
 *  変更履歴:
 *  - 1-5a: `Deallocate` のポインタ照合を「先頭一致」から「生存記録を優先」に修正した。
 *    `_aligned_malloc` は解放済みのアドレスを再利用するため、同一アドレスの記録が
 *    複数並び得る。旧実装は解放済みの古い記録を先に掴んで**偽の二重解放**を報告し、
 *    生きている記録を解放しないためリークにも見えていた。
 *    JSON 各層は解放をほとんど行わないため露見していなかったが、
 *    `DynamicArray` の伸長 (確保 -> 旧領域の解放を繰り返す) で表面化した。
 *    修正後も 1-1 は Debug 844 / Release 847 のまま不変であることを確認済み。
 *
 *  対応する要件: T-5(確保失敗の注入 / Reset 後のリーク無し)、
 *  T-6(不正入力時の OutOfMemory 伝播)、T-6a(深度スタック移送時の確保失敗)。
 */

#include <cstddef>
#include <malloc.h>   // _aligned_malloc / _aligned_free (MSVC)

#include "Core/MemoryResource.h"

namespace GLFD::Test {

  using GLFD::Memory::IMemoryResource;

  /**
   * @brief 確保/解放を記録し、任意のタイミングで失敗を注入できる IMemoryResource
   * @note  STL コンテナを使わないため記録は固定長配列。溢れたら overflow フラグを立てる
   */
  class MockMemoryResource final : public IMemoryResource {
  public:
    static constexpr size_t kMaxRecords = 512;

    struct Record {
      void*  ptr;
      size_t size;
      size_t alignment;
      bool   live;
    };

    void* Allocate(size_t size, size_t alignment) override {
      ++m_allocateCalls;

      // 失敗注入: failAfter 回目以降の要求を全て失敗させる
      if (m_failAfter >= 0 && m_allocateCalls > m_failAfter) {
        ++m_injectedFailures;
        return nullptr;
      }
      if (m_recordCount >= kMaxRecords) {
        m_recordOverflow = true;
        return nullptr;
      }

      // アライメント要求が守れていることを実測で確かめるため実メモリを使う
      void* const p = _aligned_malloc(size, alignment);
      if (p == nullptr) {
        return nullptr;
      }

      Record& rec  = m_records[m_recordCount++];
      rec.ptr       = p;
      rec.size      = size;
      rec.alignment = alignment;
      rec.live      = true;

      m_liveBytes  += size;
      m_totalBytes += size;
      return p;
    }

    void Deallocate(void* p, size_t size, size_t alignment) override {
      ++m_deallocateCalls;
      if (p == nullptr) {
        return;
      }

      // 同じアドレスが複数の記録に現れ得る (解放済みの領域を _aligned_malloc が
      // 再利用するため)。**生存している記録を優先して探す**。
      // 先頭一致で打ち切ると、再利用のたびに解放済みの古い記録を掴んで
      // 偽の二重解放を報告し、生きている記録が解放されずリークに見えてしまう
      Record* stale = nullptr;

      for (size_t i = 0; i < m_recordCount; ++i) {
        Record& rec = m_records[i];
        if (rec.ptr != p) {
          continue;
        }
        if (!rec.live) {
          if (stale == nullptr) { stale = &rec; }
          continue;
        }
        // Deallocate はサイズとアライメントを要求するインタフェースなので、
        // 確保時と厳密に一致していることを検証する
        if (rec.size != size)           { m_sizeMismatch = true; }
        if (rec.alignment != alignment) { m_alignmentMismatch = true; }

        rec.live     = false;
        m_liveBytes -= rec.size;
        _aligned_free(p);
        return;
      }

      if (stale != nullptr) {
        m_doubleFree = true;   // 生存記録が無く解放済み記録だけがある = 二重解放
        return;
      }

      m_unknownFree = true;   // このリソースが渡していないポインタ
    }

    ~MockMemoryResource() override {
      // 取りこぼしがあってもプロセスを汚さないよう後始末する
      for (size_t i = 0; i < m_recordCount; ++i) {
        if (m_records[i].live) {
          _aligned_free(m_records[i].ptr);
          m_records[i].live = false;
        }
      }
    }

    /// count 回目までの Allocate は成功し、それ以降は全て失敗する
    void SetFailAfter(int count) { m_failAfter = count; }
    void ClearFailure()          { m_failAfter = -1; }

    int    AllocateCalls()    const { return m_allocateCalls; }
    int    DeallocateCalls()  const { return m_deallocateCalls; }
    int    InjectedFailures() const { return m_injectedFailures; }
    size_t LiveBytes()        const { return m_liveBytes; }
    size_t TotalBytes()       const { return m_totalBytes; }

    size_t LiveBlockCount() const {
      size_t n = 0;
      for (size_t i = 0; i < m_recordCount; ++i) {
        if (m_records[i].live) { ++n; }
      }
      return n;
    }

    bool DoubleFree()        const { return m_doubleFree; }
    bool UnknownFree()       const { return m_unknownFree; }
    bool SizeMismatch()      const { return m_sizeMismatch; }
    bool AlignmentMismatch() const { return m_alignmentMismatch; }
    bool RecordOverflow()    const { return m_recordOverflow; }

  private:
    Record m_records[kMaxRecords] = {};
    size_t m_recordCount = 0;

    int m_allocateCalls    = 0;
    int m_deallocateCalls  = 0;
    int m_injectedFailures = 0;
    int m_failAfter        = -1;

    size_t m_liveBytes  = 0;
    size_t m_totalBytes = 0;

    bool m_doubleFree        = false;
    bool m_unknownFree       = false;
    bool m_sizeMismatch      = false;
    bool m_alignmentMismatch = false;
    bool m_recordOverflow    = false;
  };

}
