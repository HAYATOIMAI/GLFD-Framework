/**
 * @file  JsonFileIO.cpp
 * @brief ReadEntireFile の Win32 実装
 *
 * @details
 *  `<windows.h>` をこの翻訳単位に閉じ込め、ヘッダへ漏らさない。
 */

#include "Core/Json/JsonFileIO.h"

#include <cassert>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

namespace GLFD::Json {

  namespace {

    /// 1 回の ReadFile で要求する最大バイト数(DWORD の上限に合わせる)
    constexpr DWORD kMaxChunk = 0x10000000u;   // 256 MiB

    /**
     * @brief 読み込みを許す最大ファイルサイズ
     * @note  Value は文字列長を uint32_t で持つため、2 GiB を超える JSON は
     *        そもそも DOM で表現できない。ここで明示的に弾いて
     *        「読めたのに後段で壊れる」状態を作らない
     */
    constexpr std::int64_t kMaxFileSize = 0x7FFFFFFF;   // 2 GiB - 1

    /// UTF-8 のパスを UTF-16 へ変換する。バッファはアリーナから取る
    [[nodiscard]] bool WidenPath(const char* utf8Path, JsonArena& arena,
                                 wchar_t*& outWide, ErrorCode& outError) noexcept {
      // MB_ERR_INVALID_CHARS を付けることで、不正な UTF-8 を黙って置換文字へ
      // 変換せずエラーにできる
      const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               utf8Path, -1, nullptr, 0);
      if (needed <= 0) {
        outError = ErrorCode::PathEncodingInvalid;
        return false;
      }

      wchar_t* const buffer = arena.AllocateArray<wchar_t>(static_cast<size_t>(needed));
      if (buffer == nullptr) {
        outError = ErrorCode::OutOfMemory;
        return false;
      }

      const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                utf8Path, -1, buffer, needed);
      if (written != needed) {
        outError = ErrorCode::PathEncodingInvalid;
        return false;
      }

      outWide = buffer;
      return true;
    }

    /// 符号なし整数を 10 進で書き足す。CRT の書式化に頼らない
    [[nodiscard]] size_t AppendDecimal(wchar_t* dst, size_t position,
                                       unsigned long long value) noexcept {
      wchar_t digits[24];
      size_t  count = 0;
      do {
        digits[count++] = static_cast<wchar_t>(L'0' + (value % 10u));
        value /= 10u;
      } while (value != 0 && count < 24);

      for (size_t i = 0; i < count; ++i) {
        dst[position + i] = digits[count - 1 - i];
      }
      return position + count;
    }

    /// CreateFileW の失敗理由を ErrorCode へ写す
    [[nodiscard]] ErrorCode ClassifyOpenFailure() noexcept {
      const DWORD code = ::GetLastError();
      if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
          || code == ERROR_INVALID_NAME || code == ERROR_BAD_NETPATH) {
        return ErrorCode::FileNotFound;
      }
      // 権限不足・共有違反・I/O エラーはまとめて FileReadFailed。
      // パスが存在するかどうかで調べる先が変わるので、そこだけを分けている
      return ErrorCode::FileReadFailed;
    }

  }

  bool ReadEntireFile(const char* utf8Path, JsonArena& arena,
                      StringView& outContents, ErrorCode& outError) noexcept {
    outContents = StringView();
    outError    = ErrorCode::None;

    assert(utf8Path != nullptr && "ReadEntireFile: path must not be null");
    if (utf8Path == nullptr) {
      outError = ErrorCode::FileNotFound;
      return false;
    }

    wchar_t* widePath = nullptr;
    if (!WidenPath(utf8Path, arena, widePath, outError)) {
      return false;
    }

    const HANDLE handle = ::CreateFileW(widePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      outError = ClassifyOpenFailure();
      return false;
    }

    LARGE_INTEGER fileSize{};
    if (::GetFileSizeEx(handle, &fileSize) == 0) {
      ::CloseHandle(handle);
      outError = ErrorCode::FileReadFailed;
      return false;
    }
    if (fileSize.QuadPart < 0 || fileSize.QuadPart > kMaxFileSize) {
      ::CloseHandle(handle);
      outError = ErrorCode::FileReadFailed;
      return false;
    }

    const auto total = static_cast<size_t>(fileSize.QuadPart);

    // 空ファイルは成功。DocumentEmpty と判断するのはパーサの責務
    if (total == 0) {
      ::CloseHandle(handle);
      outContents = StringView();
      return true;
    }

    char* const buffer = arena.AllocateArray<char>(total);
    if (buffer == nullptr) {
      ::CloseHandle(handle);
      outError = ErrorCode::OutOfMemory;
      return false;
    }

    // ReadFile は要求より少ないバイト数を返し得るので、必ずループで読み切る
    size_t read = 0;
    while (read < total) {
      const size_t remaining = total - read;
      const DWORD  request   = (remaining > kMaxChunk)
                                 ? kMaxChunk
                                 : static_cast<DWORD>(remaining);
      DWORD received = 0;
      if (::ReadFile(handle, buffer + read, request, &received, nullptr) == 0) {
        ::CloseHandle(handle);
        outError = ErrorCode::FileReadFailed;
        return false;
      }
      if (received == 0) {
        // サイズを取得した後にファイルが切り詰められた等
        ::CloseHandle(handle);
        outError = ErrorCode::FileReadFailed;
        return false;
      }
      read += received;
    }

    ::CloseHandle(handle);

    // 内容はアリーナ上にあるので、ハンドルを閉じた後も有効
    outContents = StringView(buffer, total);
    return true;
  }

  // ---------------------------------------------------------------------------
  // 書き込み
  // ---------------------------------------------------------------------------

  bool BeginFileWrite(const char* utf8Path, JsonArena& arena,
                      FileWriteHandle& outHandle, ErrorCode& outError) noexcept {
    outHandle = FileWriteHandle{};
    outError  = ErrorCode::None;

    assert(utf8Path != nullptr && "BeginFileWrite: path must not be null");
    if (utf8Path == nullptr) {
      outError = ErrorCode::FileNotFound;
      return false;
    }

    wchar_t* finalPath = nullptr;
    if (!WidenPath(utf8Path, arena, finalPath, outError)) {
      return false;
    }

    // 一時ファイル名 = <目標パス>.<pid>.<tick>.tmp
    // 目標と同じディレクトリに作る(MoveFileEx は同一ボリュームでないと原子的でない)
    size_t finalLength = 0;
    while (finalPath[finalLength] != L'\0') {
      ++finalLength;
    }

    constexpr size_t kSuffixReserve = 64;   // ".<pid>.<tick>.tmp" に十分な余裕
    wchar_t* const   tempPath = arena.AllocateArray<wchar_t>(finalLength + kSuffixReserve);
    if (tempPath == nullptr) {
      outError = ErrorCode::OutOfMemory;
      return false;
    }

    for (size_t i = 0; i < finalLength; ++i) {
      tempPath[i] = finalPath[i];
    }
    size_t position = finalLength;
    tempPath[position++] = L'.';
    position = AppendDecimal(tempPath, position, ::GetCurrentProcessId());
    tempPath[position++] = L'.';
    position = AppendDecimal(tempPath, position, ::GetTickCount64());
    tempPath[position++] = L'.';
    tempPath[position++] = L't';
    tempPath[position++] = L'm';
    tempPath[position++] = L'p';
    tempPath[position]   = L'\0';

    const HANDLE handle = ::CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const DWORD code = ::GetLastError();
      // ディレクトリが無い場合はパス指定の問題なので FileNotFound、
      // それ以外(権限・ロック・ディスク満杯等)は書き込み固有の失敗
      outError = (code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_NAME
                  || code == ERROR_BAD_NETPATH)
                   ? ErrorCode::FileNotFound
                   : ErrorCode::FileWriteFailed;
      return false;
    }

    outHandle.osHandle  = handle;
    outHandle.tempPath  = tempPath;
    outHandle.finalPath = finalPath;
    outHandle.open      = true;
    return true;
  }

  bool WriteFileChunk(FileWriteHandle& handle, const char* data, size_t size,
                      ErrorCode& outError) noexcept {
    outError = ErrorCode::None;

    if (!handle.open || handle.osHandle == nullptr) {
      outError = ErrorCode::FileWriteFailed;
      return false;
    }
    if (size == 0) {
      return true;
    }
    assert(data != nullptr && "WriteFileChunk: data must not be null when size > 0");

    // WriteFile は要求より少ないバイト数しか書かないことがあるのでループする
    size_t written = 0;
    while (written < size) {
      const size_t remaining = size - written;
      const DWORD  request   = (remaining > kMaxChunk)
                                 ? kMaxChunk
                                 : static_cast<DWORD>(remaining);
      DWORD accepted = 0;
      if (::WriteFile(handle.osHandle, data + written, request, &accepted, nullptr) == 0
          || accepted == 0) {
        outError = ErrorCode::FileWriteFailed;
        return false;
      }
      written += accepted;
    }
    return true;
  }

  bool CommitFileWrite(FileWriteHandle& handle, ErrorCode& outError) noexcept {
    outError = ErrorCode::None;

    if (!handle.open) {
      outError = ErrorCode::FileWriteFailed;
      return false;
    }

    const bool flushed = (::FlushFileBuffers(handle.osHandle) != 0);
    ::CloseHandle(handle.osHandle);
    handle.osHandle = nullptr;
    handle.open     = false;

    if (!flushed) {
      ::DeleteFileW(handle.tempPath);
      outError = ErrorCode::FileWriteFailed;
      return false;
    }

    // 一時ファイルを目標へ置き換える。ここまで来て初めて既存ファイルが変わる
    if (::MoveFileExW(handle.tempPath, handle.finalPath,
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
      // 置き換えに失敗しても目標ファイルは無傷。一時ファイルだけ片付ける
      ::DeleteFileW(handle.tempPath);
      outError = ErrorCode::FileWriteFailed;
      return false;
    }
    return true;
  }

  void AbortFileWrite(FileWriteHandle& handle) noexcept {
    if (!handle.open) {
      return;
    }
    ::CloseHandle(handle.osHandle);
    handle.osHandle = nullptr;
    handle.open     = false;

    // 一時ファイルを残さない
    if (handle.tempPath != nullptr) {
      ::DeleteFileW(handle.tempPath);
    }
  }

}
