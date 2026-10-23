#pragma once

/**
 * @file  GamePaths.h
 * @brief ゲームが読み書きする 4 つのファイルのパスを、根から 1 か所で決める (ECS 2-16)
 *
 * @details
 *  根は `RootPath.h` の `FindRoot` で決める(exe の隣・1 つ上・2 つ上で `Resource\GameConfig.jsonc` を探す)。
 *  ここで、根から次の 4 つを組み立てる。起動時に 1 回だけ作り、`GameContext::paths` で配る。
 *  F5 の読み直しも同じものを読むので、起動時と別のファイルを読むことは無い。
 *
 *  | ファイル | 置き場所 | 形 |
 *  |---|---|---|
 *  | 設定 | `<根>\Resource\GameConfig.jsonc` | UTF-8(`JsonFileIO` が UTF-8 のパスを受け取り、ワイドで開く) |
 *  | ローカル設定 | `<根>\Resource\GameConfig.local.json` | UTF-8 |
 *  | シェーダー | `<根>\Source\Shaders\particle.hlsl` | ワイド(と、ログに書く UTF-8) |
 *  | ログ | `<根>\Game.log`。根が見つからなければ `<exe のフォルダ>\Game.log` | ワイド(と UTF-8) |
 *
 *  根が見つからないときに作業フォルダへ戻らないのは `RootPath.h` と同じ理由。ログだけは exe の隣に置き、
 *  探した場所をそこに書けるようにする。
 *
 *  @note パスは配列に入れる。入りきらなければ切らずに `filesFit` を false にする
 */

#include <cstddef>

#include "Core/RootPath.h"

namespace GLFD::Core {

  inline constexpr const wchar_t* kGameConfigLocalFile = L"Resource\\GameConfig.local.json";
  inline constexpr const wchar_t* kShaderFile          = L"Source\\Shaders\\particle.hlsl";
  inline constexpr const wchar_t* kLogFile             = L"Game.log";
  /// UTF-8 のパスの配列の大きさ。ワイドの 1 文字は UTF-8 で最大 3 バイト(サロゲートの組は 2 文字で 4 バイト)
  inline constexpr std::size_t kUtf8PathCapacity = kRootPathCapacity * 3;

  struct GamePaths {
    RootSearch search;                          ///< どこを探し、どこを根にしたか
    char    config[kUtf8PathCapacity];          ///< 設定(根が無ければ空)
    char    configLocal[kUtf8PathCapacity];     ///< ローカル設定(根が無ければ空)
    wchar_t shader[kRootPathCapacity];          ///< シェーダー(根が無ければ空)
    char    shaderUtf8[kUtf8PathCapacity];
    wchar_t log[kRootPathCapacity];             ///< ログ(exe のフォルダも分からなければ空)
    char    logUtf8[kUtf8PathCapacity];
    bool    filesFit;                           ///< 組み立てたパスがすべて配列に入った
  };

  /// ワイドのパスを UTF-8 にする。入りきらなければ false(`out` は空)
  [[nodiscard]] inline bool ToUtf8Path(char* out, std::size_t capacity, const wchar_t* wide) noexcept {
    if (capacity == 0) { return false; }
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(capacity), nullptr, nullptr);
    if (written <= 0) {
      out[0] = '\0';
      return false;
    }
    return true;
  }

  namespace Detail {
    /// `folder\relative` をワイドと UTF-8 の両方で。どちらかが入らなければ両方とも空にして false
    [[nodiscard]] inline bool JoinBoth(wchar_t (&wide)[kRootPathCapacity], char (&utf8)[kUtf8PathCapacity],
                                       const wchar_t* folder, const wchar_t* relative) noexcept {
      if (JoinPath(wide, kRootPathCapacity, folder, relative) && ToUtf8Path(utf8, kUtf8PathCapacity, wide)) {
        return true;
      }
      wide[0] = L'\0';
      utf8[0] = '\0';
      return false;
    }
  }

  /**
   * @brief 探した結果から 4 つのパスを組み立てる(ディスクは見ない。T-ECS-57 が差し込みで呼ぶ)
   * @return 根が見つかり、すべてのパスが入ったら true
   */
  [[nodiscard]] inline bool BuildGamePaths(const RootSearch& search, GamePaths& out) noexcept {
    out.search = search;
    out.config[0] = out.configLocal[0] = out.shaderUtf8[0] = out.logUtf8[0] = '\0';
    out.shader[0] = out.log[0] = L'\0';
    out.filesFit = true;

    const wchar_t* const root = out.search.Root();
    // ログは根の下。根が無ければ exe の隣(候補の 0 番)に置いて、探した場所を書けるようにする
    const wchar_t* const logFolder = (root != nullptr) ? root
                                   : (out.search.candidateCount > 0 ? out.search.candidates[0].folder : nullptr);
    if (logFolder != nullptr && !Detail::JoinBoth(out.log, out.logUtf8, logFolder, kLogFile)) {
      out.filesFit = false;
    }
    if (root == nullptr) { return false; }

    wchar_t wide[kRootPathCapacity];
    if (!JoinPath(wide, kRootPathCapacity, root, kRootMarker) || !ToUtf8Path(out.config, kUtf8PathCapacity, wide)) {
      out.config[0] = '\0';
      out.filesFit = false;
    }
    if (!JoinPath(wide, kRootPathCapacity, root, kGameConfigLocalFile)
        || !ToUtf8Path(out.configLocal, kUtf8PathCapacity, wide)) {
      out.configLocal[0] = '\0';
      out.filesFit = false;
    }
    if (!Detail::JoinBoth(out.shader, out.shaderUtf8, root, kShaderFile)) {
      out.filesFit = false;
    }
    return out.filesFit;
  }

  /// この exe の場所から根を探し、4 つのパスを組み立てる
  [[nodiscard]] inline bool ResolveGamePaths(GamePaths& out) noexcept {
    RootSearch search;
    FindRoot(search);
    return BuildGamePaths(search, out);
  }

}
