/**
 * @file  JsonWriter.cpp
 * @brief 出力ストリームの実装
 *
 * @details
 *  Win32 呼び出しは `JsonFileIO.h` の関数群へ委譲しており、
 *  この翻訳単位に `<windows.h>` は現れない (R2-10)。
 */

#include "Core/Json/JsonWriter.h"

#include <charconv>
#include <cstring>
#include <limits>

namespace GLFD::Json {

  // ---------------------------------------------------------------------------
  // 書式化 / UTF-8 デコード
  // ---------------------------------------------------------------------------

  namespace Detail {

    size_t FormatInt64(std::int64_t value, char* buffer, size_t capacity) noexcept {
      const auto result = std::to_chars(buffer, buffer + capacity, value);
      assert(result.ec == std::errc{} && "FormatInt64: buffer too small");
      return (result.ec == std::errc{}) ? static_cast<size_t>(result.ptr - buffer) : 0;
    }

    size_t FormatUInt64(std::uint64_t value, char* buffer, size_t capacity) noexcept {
      const auto result = std::to_chars(buffer, buffer + capacity, value);
      assert(result.ec == std::errc{} && "FormatUInt64: buffer too small");
      return (result.ec == std::errc{}) ? static_cast<size_t>(result.ptr - buffer) : 0;
    }

    size_t FormatDouble(double value, char* buffer, size_t capacity) noexcept {
      // 書式を指定しない to_chars は shortest round-trip を返す。
      // v145 で bit 一致 10/10 とロケール非依存を実測済み (R1-11)
      const auto result = std::to_chars(buffer, buffer + capacity, value);
      assert(result.ec == std::errc{} && "FormatDouble: buffer too small");
      if (result.ec != std::errc{}) {
        return 0;
      }

      auto length = static_cast<size_t>(result.ptr - buffer);

      // `.` も指数も無いと JSON の整数リテラルになり、再パースで Int64 に化ける。
      // `to_chars(1.0)` は "1" を返すので、そのままでは Double(1.0) が Int64(1) になる。
      // `-0.0` も "-0" -> "-0.0" となり符号が保たれる
      bool looksLikeInteger = true;
      for (size_t i = 0; i < length; ++i) {
        const char c = buffer[i];
        if (c == '.' || c == 'e' || c == 'E') {
          looksLikeInteger = false;
          break;
        }
      }
      if (looksLikeInteger && (length + 2) <= capacity) {
        buffer[length++] = '.';
        buffer[length++] = '0';
      }
      return length;
    }

    std::uint32_t DecodeUtf8(const char* p, const char* end, std::uint32_t& outCodepoint) noexcept {
      const auto lead = static_cast<unsigned char>(*p);

      if (lead < 0x80u) {
        outCodepoint = lead;
        return 1;
      }

      std::uint32_t length     = 0;
      unsigned char lowSecond  = 0x80u;
      unsigned char highSecond = 0xBFu;
      std::uint32_t value      = 0;

      if (lead >= 0xC2u && lead <= 0xDFu) {
        length = 2; value = lead & 0x1Fu;
      }
      else if (lead == 0xE0u) {
        length = 3; value = lead & 0x0Fu; lowSecond = 0xA0u;      // 過長符号化を弾く
      }
      else if (lead >= 0xE1u && lead <= 0xECu) {
        length = 3; value = lead & 0x0Fu;
      }
      else if (lead == 0xEDu) {
        length = 3; value = lead & 0x0Fu; highSecond = 0x9Fu;     // サロゲートを弾く
      }
      else if (lead >= 0xEEu && lead <= 0xEFu) {
        length = 3; value = lead & 0x0Fu;
      }
      else if (lead == 0xF0u) {
        length = 4; value = lead & 0x07u; lowSecond = 0x90u;      // 過長符号化を弾く
      }
      else if (lead >= 0xF1u && lead <= 0xF3u) {
        length = 4; value = lead & 0x07u;
      }
      else if (lead == 0xF4u) {
        length = 4; value = lead & 0x07u; highSecond = 0x8Fu;     // U+10FFFF 超を弾く
      }
      else {
        return 0;
      }

      if (static_cast<size_t>(end - p) < static_cast<size_t>(length)) {
        return 0;
      }

      const auto second = static_cast<unsigned char>(p[1]);
      if (second < lowSecond || second > highSecond) {
        return 0;
      }
      value = (value << 6) | (second & 0x3Fu);

      for (std::uint32_t i = 2; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(p[i]);
        if ((continuation & 0xC0u) != 0x80u) {
          return 0;
        }
        value = (value << 6) | (continuation & 0x3Fu);
      }

      outCodepoint = value;
      return length;
    }

  }

  // ---------------------------------------------------------------------------
  // JsonStringBuffer
  // ---------------------------------------------------------------------------

  void JsonStringBuffer::CopyBytes(char* destination, const char* source, size_t size) noexcept {
    std::memcpy(destination, source, size);
  }

  bool JsonStringBuffer::Grow(size_t required) noexcept {
    if (required <= m_capacity) {
      return true;
    }

    // 倍率 2 (R2-5b と同じ根拠)。既定初期容量から始める
    size_t next = (m_capacity == 0) ? kDefaultInitialCapacity : m_capacity;
    while (next < required) {
      if (next > ((std::numeric_limits<size_t>::max)() / 2u)) {
        m_failed = true;
        return false;
      }
      next *= 2u;
    }

    char* const buffer = m_arena->AllocateArray<char>(next);
    if (buffer == nullptr) {
      m_failed = true;
      return false;
    }
    if (m_size != 0) {
      // 旧領域はアリーナ上に放棄される(個別解放できないため)
      std::memcpy(buffer, m_data, m_size);
    }
    m_data     = buffer;
    m_capacity = next;
    return true;
  }

  bool JsonStringBuffer::Reserve(size_t capacity) noexcept {
    return Grow(capacity);
  }

  // ---------------------------------------------------------------------------
  // JsonFileStream
  // ---------------------------------------------------------------------------

  JsonFileStream::JsonFileStream(const char* utf8Path, JsonArena& arena,
                                 size_t bufferSize) noexcept {
    const size_t wanted = (bufferSize == 0) ? kDefaultBufferSize : bufferSize;

    m_buffer = arena.AllocateArray<char>(wanted);
    if (m_buffer == nullptr) {
      m_error = ErrorCode::OutOfMemory;
      return;
    }
    m_bufferSize = wanted;

    if (!BeginFileWrite(utf8Path, arena, m_handle, m_error)) {
      m_buffer     = nullptr;
      m_bufferSize = 0;
    }
  }

  JsonFileStream::~JsonFileStream() {
    // ここで open のままということは、Finish() も Abort() も呼ばれていない。
    // 結果としては「一時ファイルが消えて既存ファイルが無傷」で安全側だが、
    // それは意図した中止の場合の正しさであって、Finish() の書き忘れの場合は
    // 「セーブしたつもりで何も書かれていない」という最悪の失敗になる。
    // 両者はコードから区別できないので、明示的な Abort() でのみ assert を抑制する
    assert(!m_handle.open
           && "JsonFileStream: neither Finish() nor Abort() was called; nothing was written");
    AbortFileWrite(m_handle);
  }

  void JsonFileStream::Abort() noexcept {
    AbortFileWrite(m_handle);
    m_pending = 0;
  }

  bool JsonFileStream::FlushBuffer() noexcept {
    if (m_pending == 0) {
      return true;
    }
    ErrorCode error = ErrorCode::None;
    if (!WriteFileChunk(m_handle, m_buffer, m_pending, error)) {
      m_error = error;
      return false;
    }
    m_pending = 0;
    return true;
  }

  bool JsonFileStream::Put(char c) {
    if (!m_handle.open) {
      if (m_error == ErrorCode::None) {
        m_error = ErrorCode::FileWriteFailed;
      }
      return false;
    }
    if (m_pending == m_bufferSize && !FlushBuffer()) {
      return false;
    }
    m_buffer[m_pending++] = c;
    return true;
  }

  bool JsonFileStream::Write(const char* data, size_t size) {
    if (size == 0) {
      return true;
    }
    assert(data != nullptr && "JsonFileStream::Write: null data with size > 0");

    if (!m_handle.open) {
      if (m_error == ErrorCode::None) {
        m_error = ErrorCode::FileWriteFailed;
      }
      return false;
    }

    // バッファより大きい塊はバッファを経由させず直接書く。
    // 大きな文字列を書くときに無駄な二重コピーをしないため
    if (size >= m_bufferSize) {
      if (!FlushBuffer()) {
        return false;
      }
      ErrorCode error = ErrorCode::None;
      if (!WriteFileChunk(m_handle, data, size, error)) {
        m_error = error;
        return false;
      }
      return true;
    }

    if (m_bufferSize - m_pending < size && !FlushBuffer()) {
      return false;
    }
    std::memcpy(m_buffer + m_pending, data, size);
    m_pending += size;
    return true;
  }

  bool JsonFileStream::Flush() {
    if (!m_handle.open) {
      if (m_error == ErrorCode::None) {
        m_error = ErrorCode::FileWriteFailed;
      }
      return false;
    }
    return FlushBuffer();
  }

  bool JsonFileStream::Finish() noexcept {
    if (!m_handle.open) {
      if (m_error == ErrorCode::None) {
        m_error = ErrorCode::FileWriteFailed;
      }
      return false;
    }

    if (!FlushBuffer()) {
      // 書き切れていないので置き換えない。一時ファイルを片付ける
      AbortFileWrite(m_handle);
      return false;
    }

    ErrorCode error = ErrorCode::None;
    if (!CommitFileWrite(m_handle, error)) {
      m_error = error;
      return false;
    }
    return true;
  }

}
