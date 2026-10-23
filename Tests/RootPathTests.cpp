/**
 * @file  RootPathTests.cpp
 * @brief T-ECS-57: ゲームの根の決め方 (ECS 2-16)
 *
 * @details
 *  `Core::FindRootFrom` に exe のパスと偽のディスクを差し込み、本物のファイルの配置に頼らずに確かめる。
 *  本物のディスクを見る `FindRoot` は、このテストの exe(`<root>\Tests\build`)から根を見つけることだけを見る。
 *
 *  ## 確かめること
 *   - 57a: 目印が exe の隣 / 1 つ上 / 2 つ上にあれば、そこが根になる。3 つ上にしか無ければ見つからない。
 *          **2 つ上だけ**に置いた場合と **3 つ上だけ**に置いた場合で、たどる段数そのものを押さえる
 *          (隣に置いただけでは、たどる処理が動いていなくても通る。開発手法 §4.2)
 *   - 57b: 見つけた所で止めず、3 か所とも調べる。2 か所にあれば exe に近い方を選び、数は 2
 *   - 57c: 見つからないとき、根は無い(作業フォルダや最初の候補で代わりにしない)。探した場所は残る
 *   - 57d: 日本語を含むパス、MAX_PATH (260) を超えるパスで動く
 *   - 57e: 入りきらないパスは切らずに失敗にする(exe のパス、候補 + 目印、`JoinPath`)。
 *          `GetModuleFileNameW` で切れたことを戻り値で見分ける
 *   - 57f: ドライブの直下の exe では、候補は 1 つだけ
 *   - 57g: 本物のディスクで、このテストの exe から根が見つかり、そこに目印がある
 *
 *  ## 押さえないもの
 *   - 260 を超えるパスに置いた exe の本物の起動(このマシンは `LongPathsEnabled` = 0、マニフェストに
 *     `longPathAware` が無い。2-16 手順1 §0 の 4)
 *   - ゲームが根を使って設定・シェーダー・ログを読むこと: 2-16 の T-ECS-58(実機)
 *
 *  @note テストコードに非 ASCII の文字列リテラルを書かない (C5297)。日本語は \u で書く。
 */

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <string>

#include "Core/RootPath.h"

#include "TestHarness.h"

namespace {

  using GLFD::Core::RootSearch;
  using GLFD::Core::RootStatus;

  /// 偽のディスク: ここに並べたパスだけが「ある」。何回聞かれたかも数える
  struct FakeDisk {
    const wchar_t* files[4] = {};
    int            count = 0;
    int            asked = 0;
  };

  bool FakeExists(const wchar_t* path, void* user) {
    FakeDisk& disk = *static_cast<FakeDisk*>(user);
    ++disk.asked;
    for (int i = 0; i < disk.count; ++i) {
      if (std::wcscmp(disk.files[i], path) == 0) { return true; }
    }
    return false;
  }

  void Search(const wchar_t* exe, FakeDisk& disk, RootSearch& out) {
    GLFD::Core::FindRootFrom(exe, &FakeExists, &disk, out);
  }

  bool RootIs(const RootSearch& search, const wchar_t* expected) {
    return search.Root() != nullptr && std::wcscmp(search.Root(), expected) == 0;
  }

  RootSearch g_search;   // 大きい (約 6 KB) ので static に置く

  void TestLevels() {
    GLFD::Test::BeginCase("T-ECS-57a: the marker next to the exe, one up, two up is found; three up is not");
    const wchar_t* exe = L"C:\\r\\a\\b\\game.exe";
    {
      FakeDisk disk;
      disk.files[disk.count++] = L"C:\\r\\a\\b\\Resource\\GameConfig.jsonc";
      Search(exe, disk, g_search);
      CHECK(g_search.status == RootStatus::Found);
      CHECK(RootIs(g_search, L"C:\\r\\a\\b"));
      CHECK(g_search.chosen == 0);
    }
    {
      FakeDisk disk;
      disk.files[disk.count++] = L"C:\\r\\a\\Resource\\GameConfig.jsonc";
      Search(exe, disk, g_search);
      CHECK(RootIs(g_search, L"C:\\r\\a"));
      CHECK(g_search.chosen == 1);
    }
    {
      // 開発中の形: <root>\x64\Release\exe。たどる処理が動いていないと、ここで落ちる
      FakeDisk disk;
      disk.files[disk.count++] = L"C:\\r\\Resource\\GameConfig.jsonc";
      Search(exe, disk, g_search);
      CHECK(g_search.status == RootStatus::Found);
      CHECK(RootIs(g_search, L"C:\\r"));
      CHECK(g_search.chosen == 2);
      CHECK(g_search.markerCount == 1);
    }
    {
      // 3 つ上(C:\)は探さない。段数を 1 つ増やすと、ここで落ちる
      FakeDisk disk;
      disk.files[disk.count++] = L"C:\\Resource\\GameConfig.jsonc";
      Search(exe, disk, g_search);
      CHECK(g_search.status == RootStatus::NotFound);
      CHECK(g_search.Root() == nullptr);
      CHECK(g_search.candidateCount == 3);
      CHECK(disk.asked == 3);
    }
  }

  void TestAllCandidatesChecked() {
    GLFD::Test::BeginCase("T-ECS-57b: all three are checked; with two markers the nearest wins and both are counted");
    FakeDisk disk;
    disk.files[disk.count++] = L"C:\\r\\Resource\\GameConfig.jsonc";                // リポジトリの実物
    disk.files[disk.count++] = L"C:\\r\\x64\\Release\\Resource\\GameConfig.jsonc";  // ビルドで写した写し
    Search(L"C:\\r\\x64\\Release\\game.exe", disk, g_search);
    CHECK(g_search.status == RootStatus::Found);
    CHECK(RootIs(g_search, L"C:\\r\\x64\\Release"));
    CHECK(g_search.markerCount == 2);              // 呼び出し側はこれで警告する
    CHECK(disk.asked == 3);                        // 見つけた所で止めていない
    CHECK(g_search.candidates[0].hasMarker && !g_search.candidates[1].hasMarker && g_search.candidates[2].hasMarker);
  }

  void TestNotFound() {
    GLFD::Test::BeginCase("T-ECS-57c: not found means no root, and the searched folders are kept");
    FakeDisk disk;
    Search(L"D:\\games\\copy\\only\\game.exe", disk, g_search);
    CHECK(g_search.status == RootStatus::NotFound);
    CHECK(g_search.Root() == nullptr);
    CHECK(g_search.chosen == -1);
    CHECK(g_search.markerCount == 0);
    CHECK(g_search.candidateCount == 3);
    CHECK(std::wcscmp(g_search.candidates[0].folder, L"D:\\games\\copy\\only") == 0);
    CHECK(std::wcscmp(g_search.candidates[1].folder, L"D:\\games\\copy") == 0);
    CHECK(std::wcscmp(g_search.candidates[2].folder, L"D:\\games") == 0);
  }

  void TestJapaneseAndLong() {
    GLFD::Test::BeginCase("T-ECS-57d: paths with Japanese and paths longer than MAX_PATH");
    {
      // C:\Users\ゆうしゃ\日本語のフォルダ\x64\Release\game.exe
      const wchar_t* exe =
          L"C:\\Users\\\u3086\u3046\u3057\u3083\\\u65E5\u672C\u8A9E\u306E\u30D5\u30A9\u30EB\u30C0\\x64\\Release\\game.exe";
      FakeDisk disk;
      disk.files[disk.count++] =
          L"C:\\Users\\\u3086\u3046\u3057\u3083\\\u65E5\u672C\u8A9E\u306E\u30D5\u30A9\u30EB\u30C0\\Resource\\GameConfig.jsonc";
      Search(exe, disk, g_search);
      CHECK(RootIs(g_search, L"C:\\Users\\\u3086\u3046\u3057\u3083\\\u65E5\u672C\u8A9E\u306E\u30D5\u30A9\u30EB\u30C0"));
    }
    {
      // 根だけで 400 文字: MAX_PATH の配列では入らない
      std::wstring root = L"C:\\";
      while (root.size() < 400) { root += L"long_folder_name\\"; }
      root += L"end";
      const std::wstring exe    = root + L"\\x64\\Release\\game.exe";
      const std::wstring marker = root + L"\\Resource\\GameConfig.jsonc";
      FakeDisk disk;
      disk.files[disk.count++] = marker.c_str();
      Search(exe.c_str(), disk, g_search);
      CHECK(root.size() > MAX_PATH);
      CHECK(g_search.status == RootStatus::Found);
      CHECK(RootIs(g_search, root.c_str()));
    }
  }

  void TestTooLong() {
    GLFD::Test::BeginCase("T-ECS-57e: paths that do not fit fail instead of being cut");
    const std::size_t capacity = GLFD::Core::kRootPathCapacity;
    {
      // exe のパスが配列に入らない
      std::wstring exe = L"C:\\";
      while (exe.size() < capacity) { exe += L"x"; }
      exe += L"\\game.exe";
      FakeDisk disk;
      Search(exe.c_str(), disk, g_search);
      CHECK(g_search.status == RootStatus::ExePathTooLong);
      CHECK(g_search.Root() == nullptr);
      CHECK(disk.asked == 0);
    }
    {
      // exe は入るが、exe のフォルダに目印を足すと入らない。遠い候補で代わりにしない
      std::wstring folder = L"C:\\r\\";
      while (folder.size() < capacity - 12) { folder += L"y"; }
      const std::wstring exe = folder + L"\\g.exe";
      CHECK(exe.size() < capacity);
      FakeDisk disk;
      disk.files[disk.count++] = L"C:\\Resource\\GameConfig.jsonc";
      Search(exe.c_str(), disk, g_search);
      CHECK(g_search.status == RootStatus::CandidateTooLong);
      CHECK(g_search.Root() == nullptr);
      CHECK(disk.asked == 0);
    }
    {
      wchar_t out[8];
      CHECK(GLFD::Core::JoinPath(out, 8, L"C:\\a", L"bc"));            // "C:\a\bc" は 7 文字 + 終端
      CHECK(std::wcscmp(out, L"C:\\a\\bc") == 0);
      CHECK(!GLFD::Core::JoinPath(out, 8, L"C:\\a", L"bcd"));          // 1 文字溢れる
      CHECK(out[0] == L'\0');                                           // 切ったパスを残さない
    }
    {
      // GetModuleFileNameW は切れても 0 以外を返す。戻り値が容量に等しいことで見分ける
      wchar_t small[6];
      CHECK(!GLFD::Core::ExePath(small, 6));
      CHECK(small[0] == L'\0');
      wchar_t full[GLFD::Core::kRootPathCapacity];
      CHECK(GLFD::Core::ExePath(full, static_cast<DWORD>(GLFD::Core::kRootPathCapacity)));
      const wchar_t* name = std::wcsrchr(full, L'\\');
      CHECK(name != nullptr && _wcsicmp(name, L"\\RootPathTests.exe") == 0);
    }
  }

  void TestDriveRoot() {
    GLFD::Test::BeginCase("T-ECS-57f: an exe at the top of a drive has one candidate");
    FakeDisk disk;
    disk.files[disk.count++] = L"C:\\Resource\\GameConfig.jsonc";
    Search(L"C:\\game.exe", disk, g_search);
    CHECK(g_search.candidateCount == 1);
    CHECK(RootIs(g_search, L"C:"));
    CHECK(disk.asked == 1);
  }

  void TestRealDisk() {
    GLFD::Test::BeginCase("T-ECS-57g: on the real disk this test finds the repository root");
    GLFD::Core::FindRoot(g_search);
    CHECK(g_search.status == RootStatus::Found);
    CHECK(g_search.chosen == 2);                 // <root>\Tests\build\RootPathTests.exe
    CHECK(g_search.markerCount == 1);            // Tests\build や Tests に写しは無い
    wchar_t marker[GLFD::Core::kRootPathCapacity];
    CHECK(g_search.Root() != nullptr
          && GLFD::Core::JoinPath(marker, GLFD::Core::kRootPathCapacity, g_search.Root(), GLFD::Core::kRootMarker)
          && GLFD::Core::FileExistsOnDisk(marker, nullptr));
    // フォルダは「ファイルがある」に数えない
    CHECK(g_search.Root() != nullptr && !GLFD::Core::FileExistsOnDisk(g_search.Root(), nullptr));
  }

}

int main() {
  GLFD::Test::BeginSuite("RootPath (ECS 2-16)");
  TestLevels();
  TestAllCandidatesChecked();
  TestNotFound();
  TestJapaneseAndLong();
  TestTooLong();
  TestDriveRoot();
  TestRealDisk();
  return GLFD::Test::Summarize();
}
