/**
 * @file  JsonStreamTests.cpp
 * @brief 出力ストリームの単体テスト(フェーズ 1-4 / 要件 T-19, T-20 の一部)
 */

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "TestHarness.h"
#include "MockMemoryResource.h"
#include "Core/Json/JsonWriter.h"
#include "Core/Json/JsonFileIO.h"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

using GLFD::StringView;
using GLFD::Json::ErrorCode;
using GLFD::Json::JsonArena;
using GLFD::Json::JsonFileStream;
using GLFD::Json::JsonStringBuffer;
using GLFD::Json::ReadEntireFile;
using GLFD::Json::ToString;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::MockMemoryResource;
using GLFD::Test::Summarize;

namespace {

  void Fail(int line, const char* message) {
    std::printf("    [FAIL] %s(%d): %s\n", __FILE__, line, message);
    std::fflush(stdout);
    ++GLFD::Test::g_failureCount;
  }

  const char* NameOf(ErrorCode code) {
    static char buffer[128];
    const StringView name = ToString(code);
    std::snprintf(buffer, sizeof(buffer), "%.*s",
                  static_cast<int>(name.Size()), name.Data());
    return buffer;
  }

  // -------------------------------------------------------------------------
  // ファイル操作のヘルパ(テストの前提を作るためだけのもの)
  // -------------------------------------------------------------------------

  bool ToWide(const char* utf8Path, wchar_t* out, int capacity) {
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 utf8Path, -1, out, capacity) > 0;
  }

  bool WriteFileUtf8Path(const char* utf8Path, const void* data, size_t size) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) {
      return false;
    }
    const HANDLE handle = ::CreateFileW(wide, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }
    bool ok = true;
    if (size != 0) {
      DWORD written = 0;
      ok = (::WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr) != 0)
        && (written == size);
    }
    ::CloseHandle(handle);
    return ok;
  }

  bool CreateDirectoryUtf8Path(const char* utf8Path) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) {
      return false;
    }
    return (::CreateDirectoryW(wide, nullptr) != 0) || (::GetLastError() == ERROR_ALREADY_EXISTS);
  }

  void DeleteFileUtf8Path(const char* utf8Path) {
    wchar_t wide[1024];
    if (ToWide(utf8Path, wide, 1024)) {
      ::DeleteFileW(wide);
    }
  }

  bool FileExistsUtf8Path(const char* utf8Path) {
    wchar_t wide[1024];
    if (!ToWide(utf8Path, wide, 1024)) {
      return false;
    }
    return ::GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
  }

  /// dir 直下の *.tmp を数える(一時ファイルが残っていないことの確認用)
  int CountTempFiles(const char* utf8Dir) {
    char pattern[1024];
    std::snprintf(pattern, sizeof(pattern), "%s\\*.tmp", utf8Dir);

    wchar_t wide[1024];
    if (!ToWide(pattern, wide, 1024)) {
      return -1;
    }

    WIN32_FIND_DATAW  data{};
    const HANDLE      find = ::FindFirstFileW(wide, &data);
    if (find == INVALID_HANDLE_VALUE) {
      return 0;
    }
    int count = 0;
    do {
      ++count;
    } while (::FindNextFileW(find, &data) != 0);
    ::FindClose(find);
    return count;
  }

  const char* TempRoot() {
    static char root[1024];
    static bool initialized = false;
    if (!initialized) {
      wchar_t     wide[512];
      const DWORD length    = ::GetTempPathW(512, wide);
      char        utf8[1024];
      const int   converted = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                    utf8, sizeof(utf8), nullptr, nullptr);
      std::snprintf(root, sizeof(root), "%.*sglfd_json_streams",
                    (converted > 0 ? converted : 0), utf8);
      (void)CreateDirectoryUtf8Path(root);
      initialized = true;
    }
    return root;
  }

}

// ---------------------------------------------------------------------------
// JsonStringBuffer
// ---------------------------------------------------------------------------

static void Test_StringBufferBasics() {
  BeginCase("JsonStringBuffer: Put / Write / View / Clear");

  MockMemoryResource mock;
  JsonArena          arena(&mock);
  JsonStringBuffer   buffer(arena);

  CHECK(buffer.Size() == 0);
  CHECK(buffer.View().Empty());
  CHECK(!buffer.HasFailed());
  // 遅延確保: 何も書かないうちはリソースへ要求しない
  CHECK(mock.AllocateCalls() == 0);

  CHECK(buffer.Put('a'));
  CHECK(buffer.Write("bc", 2));
  CHECK(buffer.Put('d'));
  CHECK(buffer.Size() == 4);
  CHECK(buffer.View() == StringView("abcd"));
  CHECK(buffer.Flush());

  // size 0 の Write は何もせず成功する
  CHECK(buffer.Write(nullptr, 0));
  CHECK(buffer.Size() == 4);

  // Clear は内容だけ捨てて容量を残す
  const size_t capacityBefore = buffer.Capacity();
  const int    callsBefore    = mock.AllocateCalls();
  buffer.Clear();
  CHECK(buffer.Size() == 0);
  CHECK(buffer.View().Empty());
  CHECK(buffer.Capacity() == capacityBefore);

  CHECK(buffer.Write("xyz", 3));
  CHECK(buffer.View() == StringView("xyz"));
  CHECK(mock.AllocateCalls() == callsBefore);   // 再確保していない

  // 埋め込みヌルもそのまま保持する
  buffer.Clear();
  const char raw[5] = { 'a', '\0', 'b', '\0', 'c' };
  CHECK(buffer.Write(raw, 5));
  CHECK(buffer.Size() == 5);
  CHECK(buffer.View() == StringView(raw, 5));
}

static void Test_StringBufferGrowth() {
  BeginCase("JsonStringBuffer: growth keeps content and Reserve avoids it");

  // 伸長しながら 100 KiB 書いても内容が壊れないこと
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);

    constexpr size_t kTotal = 100 * 1024;
    for (size_t i = 0; i < kTotal; ++i) {
      if (!buffer.Put(static_cast<char>('0' + (i % 10)))) {
        Fail(__LINE__, "Put failed unexpectedly");
        break;
      }
    }
    CHECK(buffer.Size() == kTotal);

    bool intact = true;
    const StringView view = buffer.View();
    for (size_t i = 0; i < view.Size(); ++i) {
      if (view[i] != static_cast<char>('0' + (i % 10))) { intact = false; break; }
    }
    CHECK(intact);
    CHECK(!buffer.HasFailed());
  }

  // Reserve しておけば伸長が起きない
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena);

    CHECK(buffer.Reserve(8192));
    CHECK(buffer.Capacity() >= 8192);
    const int callsAfterReserve = mock.AllocateCalls();

    for (int i = 0; i < 8192; ++i) {
      CHECK_QUIET(buffer.Put('x'));
    }
    CHECK(buffer.Size() == 8192);
    // ブロック内に収まっているので、リソースへの追加要求は発生しない
    CHECK(mock.AllocateCalls() == callsAfterReserve);
  }

  // 初期容量を指定した構築
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena, 4096);
    CHECK(buffer.Capacity() >= 4096);
    CHECK(mock.AllocateCalls() == 1);
  }

  // バッファ境界をまたぐ Write
  {
    MockMemoryResource mock;
    JsonArena          arena(&mock);
    JsonStringBuffer   buffer(arena, 16);

    char payload[64];
    for (int i = 0; i < 64; ++i) { payload[i] = static_cast<char>('A' + (i % 26)); }

    CHECK(buffer.Write(payload, 10));
    CHECK(buffer.Write(payload + 10, 54));      // 容量 16 をまたぐ
    CHECK(buffer.Size() == 64);
    CHECK(buffer.View() == StringView(payload, 64));
  }
}

// ---------------------------------------------------------------------------
// T-19 確保失敗の網羅注入
// ---------------------------------------------------------------------------

static void Test_StringBufferAllocationFailure() {
  BeginCase("T-19: failing the Nth allocation makes Put/Write return false, never crash");

  // 最初の確保から失敗させる
  {
    MockMemoryResource mock;
    mock.SetFailAfter(0);
    JsonArena        arena(&mock);
    JsonStringBuffer buffer(arena);

    CHECK(!buffer.Put('a'));
    CHECK(buffer.HasFailed());
    CHECK(buffer.Size() == 0);
    CHECK(buffer.View().Empty());
    CHECK(!buffer.Write("abc", 3));
    CHECK(!buffer.Reserve(1024));
    CHECK(buffer.Flush());                      // メモリバッファなので常に成功
  }

  // N 回目の確保を失敗させる総当たり
  {
    constexpr size_t kTotal = 40 * 1024;

    // まず成功に必要な確保回数を測る
    int required = 0;
    {
      MockMemoryResource mock;
      JsonArena          arena(&mock);
      JsonStringBuffer   buffer(arena);
      for (size_t i = 0; i < kTotal; ++i) {
        CHECK_QUIET(buffer.Put('z'));
      }
      CHECK(buffer.Size() == kTotal);
      required = mock.AllocateCalls();
    }
    CHECK(required > 1);

    for (int n = 0; n <= required + 2; ++n) {
      MockMemoryResource mock;
      mock.SetFailAfter(n);
      {
        JsonArena        arena(&mock);
        JsonStringBuffer buffer(arena);

        size_t written = 0;
        bool   stopped = false;
        for (size_t i = 0; i < kTotal; ++i) {
          if (!buffer.Put('z')) { stopped = true; break; }
          ++written;
        }

        ++GLFD::Test::g_checkCount;
        if (stopped) {
          // 失敗した場合、それまでの内容は保たれていること
          if (buffer.Size() != written || !buffer.HasFailed()) {
            char what[128];
            std::snprintf(what, sizeof(what), "N=%d: buffer state inconsistent after failure", n);
            Fail(__LINE__, what);
          }
        }
        else if (buffer.Size() != kTotal) {
          char what[128];
          std::snprintf(what, sizeof(what), "N=%d: unexpected short write", n);
          Fail(__LINE__, what);
        }
      }
      // デストラクタ後に確保と解放が一致すること
      ++GLFD::Test::g_checkCount;
      if (mock.AllocateCalls() - mock.InjectedFailures() != mock.DeallocateCalls()) {
        char what[128];
        std::snprintf(what, sizeof(what), "N=%d: allocate/deallocate mismatch", n);
        Fail(__LINE__, what);
      }
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(mock.LiveBlockCount() == 0);
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(!mock.DoubleFree() && !mock.SizeMismatch());
    }
  }
}

// ---------------------------------------------------------------------------
// T-20 ファイル書き出し
// ---------------------------------------------------------------------------

static void Test_FileStreamBasics() {
  BeginCase("T-20: JsonFileStream writes through a temp file and replaces atomically");

  const char* root = TempRoot();

  char asciiPath[1024];
  std::snprintf(asciiPath, sizeof(asciiPath), "%s\\out.json", root);
  DeleteFileUtf8Path(asciiPath);

  MockMemoryResource mock;

  // --- 基本の書き出し ---
  {
    JsonArena arena(&mock);
    {
      JsonFileStream stream(asciiPath, arena);
      CHECK(stream.IsOpen());
      CHECK(stream.Error() == ErrorCode::None);

      // Finish するまで目標ファイルは作られない
      CHECK(!FileExistsUtf8Path(asciiPath));

      CHECK(stream.Write("{\"a\":", 5));
      CHECK(stream.Put('1'));
      CHECK(stream.Write("}", 1));
      CHECK(stream.Flush());
      CHECK(!FileExistsUtf8Path(asciiPath));   // Flush では置き換わらない

      CHECK(stream.Finish());
    }
    CHECK(FileExistsUtf8Path(asciiPath));
    CHECK(CountTempFiles(root) == 0);          // 一時ファイルが残らない

    StringView contents;
    ErrorCode  error = ErrorCode::None;
    CHECK(ReadEntireFile(asciiPath, arena, contents, error));
    CHECK(contents == StringView("{\"a\":1}"));
  }

  // --- 既存ファイルが正しく置き換わる ---
  {
    JsonArena arena(&mock);
    {
      JsonFileStream stream(asciiPath, arena);
      CHECK(stream.IsOpen());
      CHECK(stream.Write("[1,2,3]", 7));
      CHECK(stream.Finish());
    }
    StringView contents;
    ErrorCode  error = ErrorCode::None;
    CHECK(ReadEntireFile(asciiPath, arena, contents, error));
    CHECK(contents == StringView("[1,2,3]"));
    CHECK(CountTempFiles(root) == 0);
  }

  // --- 明示的に Abort すれば既存ファイルが壊れない ---
  {
    JsonArena arena(&mock);
    {
      JsonFileStream stream(asciiPath, arena);
      CHECK(stream.IsOpen());
      CHECK(stream.Write("BROKEN", 6));
      stream.Abort();
      CHECK(!stream.IsOpen());
      CHECK(!stream.Finish());               // 中止後の Finish は失敗する
    }
    StringView contents;
    ErrorCode  error = ErrorCode::None;
    CHECK(ReadEntireFile(asciiPath, arena, contents, error));
    CHECK(contents == StringView("[1,2,3]"));  // 前回の内容のまま
    CHECK(CountTempFiles(root) == 0);          // 一時ファイルも残らない
  }

#ifdef NDEBUG
  // --- Finish() も Abort() も呼ばずに破棄した場合 ---
  // Debug では「セーブの書き忘れ」を捕まえるためデストラクタが assert して停止する。
  // Release では安全側に倒れ、既存ファイルは無傷・一時ファイルも残らない
  {
    JsonArena arena(&mock);
    {
      JsonFileStream stream(asciiPath, arena);
      CHECK(stream.IsOpen());
      CHECK(stream.Write("FORGOTTEN", 9));
    }
    StringView contents;
    ErrorCode  error = ErrorCode::None;
    CHECK(ReadEntireFile(asciiPath, arena, contents, error));
    CHECK(contents == StringView("[1,2,3]"));
    CHECK(CountTempFiles(root) == 0);
  }
#endif

  // --- 内部バッファをまたぐ大きな書き込み ---
  {
    JsonArena arena(&mock);
    static char payload[300 * 1024];
    for (size_t i = 0; i < sizeof(payload); ++i) {
      payload[i] = static_cast<char>('a' + (i % 26));
    }
    {
      JsonFileStream stream(asciiPath, arena, 4096);   // わざと小さいバッファ
      CHECK(stream.IsOpen());
      // 1 バイトずつ / 小さい塊 / バッファより大きい塊 を混ぜる
      CHECK(stream.Put(payload[0]));
      CHECK(stream.Write(payload + 1, 100));
      CHECK(stream.Write(payload + 101, sizeof(payload) - 101));
      CHECK(stream.Finish());
    }
    StringView contents;
    ErrorCode  error = ErrorCode::None;
    CHECK(ReadEntireFile(asciiPath, arena, contents, error));
    CHECK(contents.Size() == sizeof(payload));
    CHECK(contents == StringView(payload, sizeof(payload)));
    CHECK(CountTempFiles(root) == 0);
  }

  DeleteFileUtf8Path(asciiPath);
}

static void Test_FileStreamJapanesePath() {
  BeginCase("T-20: JsonFileStream handles Japanese directory and file names");

  const char* root = TempRoot();

  // 非 ASCII は文字列リテラルに書けない (C5297) ためバイト配列で組み立てる。
  //   kDirNameUtf8  = "出力フォルダ"
  //   kFileNameUtf8 = "セーブ.json"
  static const unsigned char kDirNameUtf8[] = {
    0xE5, 0x87, 0xBA, 0xE5, 0x8A, 0x9B,
    0xE3, 0x83, 0x95, 0xE3, 0x82, 0xA9, 0xE3, 0x83, 0xAB, 0xE3, 0x83, 0x80, 0x00
  };
  static const unsigned char kFileNameUtf8[] = {
    0xE3, 0x82, 0xBB, 0xE3, 0x83, 0xBC, 0xE3, 0x83, 0x96,
    '.', 'j', 's', 'o', 'n', 0x00
  };
  // 内容にも非 ASCII を含める: {"名":"値"}
  static const unsigned char kContentUtf8[] = {
    '{', '"', 0xE5, 0x90, 0x8D, '"', ':', '"', 0xE5, 0x80, 0xA4, '"', '}', 0x00
  };

  char directory[1024];
  char path[1024];
  std::snprintf(directory, sizeof(directory), "%s\\%s",
                root, reinterpret_cast<const char*>(kDirNameUtf8));
  std::snprintf(path, sizeof(path), "%s\\%s",
                directory, reinterpret_cast<const char*>(kFileNameUtf8));

  CHECK(CreateDirectoryUtf8Path(directory));
  DeleteFileUtf8Path(path);

  MockMemoryResource mock;
  JsonArena          arena(&mock);

  const auto*  content       = reinterpret_cast<const char*>(kContentUtf8);
  const size_t contentLength = std::strlen(content);

  {
    JsonFileStream stream(path, arena);
    ++GLFD::Test::g_checkCount;
    if (!stream.IsOpen()) {
      char what[192];
      std::snprintf(what, sizeof(what), "failed to open japanese path: %s  (%s)",
                    path, NameOf(stream.Error()));
      Fail(__LINE__, what);
    }
    else {
      CHECK(stream.Write(content, contentLength));
      CHECK(stream.Finish());
    }
  }

  CHECK(FileExistsUtf8Path(path));
  CHECK(CountTempFiles(directory) == 0);

  StringView contents;
  ErrorCode  error = ErrorCode::None;
  CHECK(ReadEntireFile(path, arena, contents, error));
  CHECK(contents == StringView(content, contentLength));

  DeleteFileUtf8Path(path);
}

static void Test_FileStreamErrors() {
  BeginCase("T-20: bad paths report the right code and never crash");

  const char* root = TempRoot();
  MockMemoryResource mock;
  JsonArena          arena(&mock);

  // --- 不正 UTF-8 パス -> PathEncodingInvalid ---
  {
    static const unsigned char kBadPathBytes[][8] = {
      { 0x80, '.', 'j', 's', 'n', 0, 0, 0 },
      { 0xC0, 0x80, '.', 'j', 's', 'n', 0, 0 },
      { 0xED, 0xA0, 0x80, '.', 'j', 's', 'n', 0 },
      { 0xE3, 0x81, '.', 'j', 's', 'n', 0, 0 },
      { 0xF5, 0x80, 0x80, 0x80, '.', 'j', 's', 0 },
    };
    for (const unsigned char* rawPath : kBadPathBytes) {
      const auto*    badPath = reinterpret_cast<const char*>(rawPath);
      JsonFileStream stream(badPath, arena);
      ++GLFD::Test::g_checkCount;
      if (stream.IsOpen() || stream.Error() != ErrorCode::PathEncodingInvalid) {
        char what[160];
        std::snprintf(what, sizeof(what), "bad utf-8 path: expected PathEncodingInvalid, got %s",
                      NameOf(stream.Error()));
        Fail(__LINE__, what);
      }
      // 開けていないストリームへの書き込みは失敗するだけ
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(!stream.Write("x", 1));
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(!stream.Put('x'));
      ++GLFD::Test::g_checkCount;
      CHECK_QUIET(!stream.Finish());
    }
  }

  // --- 存在しないディレクトリ -> FileNotFound ---
  {
    char path[1024];
    std::snprintf(path, sizeof(path), "%s\\no_such_dir\\out.json", root);
    JsonFileStream stream(path, arena);
    CHECK(!stream.IsOpen());
    CHECK(stream.Error() == ErrorCode::FileNotFound);
    CHECK(!stream.Finish());
  }

  // --- ディレクトリ自身を指定 ---
  // 一時ファイル方式では、書き込むのは <目標>.<pid>.<tick>.tmp という別のパスなので
  // **開く時点は成功する**。失敗するのは Finish() の置き換え(MoveFileEx)であり、
  // そのときディレクトリは無傷のまま一時ファイルだけが削除される
  {
    JsonFileStream stream(root, arena);
    CHECK(stream.IsOpen());
    CHECK(stream.Write("{}", 2));
    CHECK(!stream.Finish());
    CHECK(stream.Error() == ErrorCode::FileWriteFailed);
    // ディレクトリが消えたり壊れたりしていないこと
    CHECK(FileExistsUtf8Path(root));
    CHECK(CountTempFiles(root) == 0);
  }

  // --- 内部バッファの確保に失敗 -> OutOfMemory ---
  {
    MockMemoryResource failMock;
    failMock.SetFailAfter(0);
    JsonArena failArena(&failMock);

    char path[1024];
    std::snprintf(path, sizeof(path), "%s\\oom.json", root);

    JsonFileStream stream(path, failArena);
    CHECK(!stream.IsOpen());
    CHECK(stream.Error() == ErrorCode::OutOfMemory);
    CHECK(!FileExistsUtf8Path(path));
  }

  CHECK(CountTempFiles(root) == 0);
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("JsonStream");

  Test_StringBufferBasics();
  Test_StringBufferGrowth();
  Test_StringBufferAllocationFailure();
  Test_FileStreamBasics();
  Test_FileStreamJapanesePath();
  Test_FileStreamErrors();

  return Summarize();
}
