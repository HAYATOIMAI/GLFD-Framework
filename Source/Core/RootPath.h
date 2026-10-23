#pragma once

/**
 * @file  RootPath.h
 * @brief ゲームの根(設定・シェーダー・ログを置くフォルダ)を、作業フォルダに依らずに決める (ECS 2-16)
 *
 * @details
 *  2-15 までは 3 つのパスがどれも作業フォルダからの相対だった。`x64\Release` の exe を直接起動したり、
 *  エクスプローラーが作る形のショートカット(「開始」= exe のフォルダ)から起動したりすると、
 *  設定は既定値に落ち、シェーダーが見つからずに起動できなかった(2-16 手順1 で実測)。
 *
 *  ## 決め方
 *  exe の隣・1 つ上・2 つ上の 3 か所を調べ、目印(`Resource\GameConfig.jsonc`)があるフォルダを根にする。
 *  **exe に最も近いもの**を選ぶ。開発中は `<root>\x64\Release\exe`、テストは `<root>\Tests\build\exe` で、
 *  どちらも 2 つ上で見つかる。出荷で exe の隣に置けば、最初の候補で見つかる。
 *
 *  - **3 か所とも調べる。** 見つけた所で止めない。2 か所以上にあれば、呼び出し側が警告する。
 *    あとで「ビルドで `Resource\` を exe の隣へ写す」仕組みを足すと、開発中に写しを黙って読み、
 *    リポジトリの `GameConfig.jsonc` を書き換えて F5 を押しても変わらなくなる。その見張り
 *  - **見つからなければ作業フォルダへ戻らない。** 戻ると、どのファイルを読んだかが起動の仕方で
 *    変わる(手順1 の 6bs: 別のフォルダの exe が、リポジトリの設定を黙って読んで起動した)
 *  - **パスは切らない。** 配列に入らなければ失敗にする。`GetModuleFileNameW` も戻り値で切れたことを見る
 *
 *  `FindRootFrom` は exe のパスと「ファイルがあるか」を差し込める(T-ECS-57)。本物のディスクを見るのは
 *  `FindRoot` だけで、テスト(`Tests/RepositoryPath.h`)とゲーム(`Engine.cpp`)が同じ関数を使う。
 *
 *  @note 区切りは `\` だけを見る。`GetModuleFileNameW` は `\` で返す
 */

#include <cstddef>
#include <cwchar>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace GLFD::Core {

  /// パスを入れる配列の大きさ(終端を含む wchar_t の数)。MAX_PATH (260) より長いパスも入る
  inline constexpr std::size_t kRootPathCapacity = 1024;
  /// 調べる場所の数: exe の隣・1 つ上・2 つ上
  inline constexpr int kRootCandidateCount = 3;
  /// 根の目印(根からの相対)
  inline constexpr const wchar_t* kRootMarker = L"Resource\\GameConfig.jsonc";

  enum class RootStatus : unsigned char {
    Found,              ///< 目印が見つかった
    NotFound,           ///< どの候補にも目印が無い
    ExePathTooLong,     ///< exe のパスが配列に入らない(`GetModuleFileNameW` で切れた場合も)
    CandidateTooLong,   ///< 候補に目印を足したパスが配列に入らない。遠い候補で代わりにしない
  };

  struct RootCandidate {
    wchar_t folder[kRootPathCapacity];   ///< 末尾に区切りは付かない
    bool    hasMarker;
  };

  /// 探した結果。どこを探したかも残す(見つからないときの知らせに書く)
  struct RootSearch {
    RootStatus    status         = RootStatus::NotFound;
    int           candidateCount = 0;    ///< 調べた候補の数(ドライブの直下なら 3 より少ない)
    int           chosen         = -1;   ///< 選んだ候補。見つからなければ -1
    int           markerCount    = 0;    ///< 目印があった候補の数。2 以上なら警告する
    RootCandidate candidates[kRootCandidateCount] = {};

    /// 根。見つからなければ nullptr(作業フォルダで代わりにしない)
    [[nodiscard]] const wchar_t* Root() const noexcept {
      return (status == RootStatus::Found && chosen >= 0) ? candidates[chosen].folder : nullptr;
    }
  };

  /// 「このパスのファイルがあるか」。テストは偽のディスクを差し込む
  using FileExistsFn = bool (*)(const wchar_t* path, void* user);

  /**
   * @brief `folder\relative` を `out` に書く
   * @return 入りきらなければ false(`out` は空になる。途中で切ったパスは返さない)
   */
  [[nodiscard]] inline bool JoinPath(wchar_t* out, std::size_t capacity,
                                     const wchar_t* folder, const wchar_t* relative) noexcept {
    if (capacity == 0) { return false; }
    const std::size_t a = std::wcslen(folder);
    const std::size_t b = std::wcslen(relative);
    if (a + 1 + b + 1 > capacity) {
      out[0] = L'\0';
      return false;
    }
    std::wmemcpy(out, folder, a);
    out[a] = L'\\';
    std::wmemcpy(out + a + 1, relative, b);
    out[a + 1 + b] = L'\0';
    return true;
  }

  /**
   * @brief exe のパスから根を探す(ディスクは `exists` で見る)
   * @param exePath exe のフルパス(`C:\...\x64\Release\Game.exe`)
   */
  inline void FindRootFrom(const wchar_t* exePath, FileExistsFn exists, void* user, RootSearch& out) noexcept {
    out = RootSearch{};
    const std::size_t length = std::wcslen(exePath);
    if (length >= kRootPathCapacity) {
      out.status = RootStatus::ExePathTooLong;
      return;
    }
    wchar_t folder[kRootPathCapacity];
    std::wmemcpy(folder, exePath, length + 1);
    wchar_t marker[kRootPathCapacity];
    for (int i = 0; i < kRootCandidateCount; ++i) {
      // 1 回目で exe の名前、2 回目で exe のフォルダ、3 回目でその上を落とす
      wchar_t* const slash = std::wcsrchr(folder, L'\\');
      if (slash == nullptr || slash == folder) { break; }   // もう上が無い
      *slash = L'\0';
      RootCandidate& candidate = out.candidates[out.candidateCount++];
      std::wmemcpy(candidate.folder, folder, static_cast<std::size_t>(slash - folder) + 1);
      if (!JoinPath(marker, kRootPathCapacity, folder, kRootMarker)) {
        out.status = RootStatus::CandidateTooLong;
        out.chosen = -1;
        return;
      }
      candidate.hasMarker = exists(marker, user);
      if (candidate.hasMarker) {
        ++out.markerCount;
        if (out.chosen < 0) { out.chosen = i; }
      }
    }
    out.status = (out.chosen >= 0) ? RootStatus::Found : RootStatus::NotFound;
  }

  /// 本物のディスク: ファイルがあるか(フォルダは数えない)
  [[nodiscard]] inline bool FileExistsOnDisk(const wchar_t* path, void* /*user*/) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  }

  /**
   * @brief この exe のフルパス
   * @return 取れなければ、または配列に入りきらず切れたら false。
   *         `GetModuleFileNameW` は切れても成功を返し、そのときの戻り値は `capacity` に等しい
   */
  [[nodiscard]] inline bool ExePath(wchar_t* out, DWORD capacity) noexcept {
    if (capacity == 0) { return false; }
    const DWORD n = ::GetModuleFileNameW(nullptr, out, capacity);
    if (n == 0 || n >= capacity) {
      out[0] = L'\0';
      return false;
    }
    return true;
  }

  /// この exe の場所から根を探す
  inline void FindRoot(RootSearch& out) noexcept {
    wchar_t exe[kRootPathCapacity];
    if (!ExePath(exe, static_cast<DWORD>(kRootPathCapacity))) {
      out = RootSearch{};
      out.status = RootStatus::ExePathTooLong;
      return;
    }
    FindRootFrom(exe, &FileExistsOnDisk, nullptr, out);
  }

}
