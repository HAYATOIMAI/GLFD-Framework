#pragma once

/**
 * @file  RepositoryPath.h
 * @brief リポジトリ内のファイルを、**作業ディレクトリに依存せず**指す
 *
 * @details
 *  ## なぜ CWD ではなくモジュールパスから辿るのか
 *  `Tests/build_and_run.bat` は各 exe を
 *  `"%OUTDIR%\%NAME%.exe"`(= `<root>\Tests\build\*.exe`)と**フルパスで起動する**。
 *  `cd` は行わないので、**作業ディレクトリは bat を呼んだ場所のまま**である
 *  (リポジトリ直下から呼べば `<root>`、`Tests\` から呼べば `<root>\Tests`)。
 *  したがって相対パスや `current_path()` を起点にすると、**呼び出し方によって
 *  解決先が変わる**。実行ファイルの位置は常に `<root>\Tests\build` なので、
 *  そこから辿る形だけが安定する。
 *
 *  「`current_path()` の方が簡単では」と思ったらここを読むこと。簡単だが、
 *  **どこから実行しても同じ結果になる**という性質を失う。
 *
 *  ## ゲームと同じ関数で探す (ECS 2-16)
 *  2-15 までは、ここだけが「exe のパスから 3 段たどる」を持っていた(目印は見ず、
 *  `GetModuleFileNameW` で切れても気づかなかった)。2-16 でゲームも作業フォルダに依らずに
 *  根を決めるようになったので、探す処理は `Source/Core/RootPath.h` の 1 本にまとめ、
 *  ここはそれを呼ぶだけにした。テストの exe も `<root>\Tests\build` で、2 つ上の目印
 *  (`Resource\GameConfig.jsonc`)で根が見つかる。
 *
 *  ## 共有 static を返さない
 *  `RepositoryFile` は**呼び出し側のバッファ**へ書く。共有の static を返す形は
 *  2 本のパスを同時に持てず、2-3 の T-41 で実際に踏んだ
 *  (後から取った方が前の内容を上書きし、基底と上書きが同じファイルを指した)。
 *  `RepositoryRoot()` だけは一度組み立てたら変わらないので static でよい。
 *
 *  @note ここに集約する前は `JsonRealDataTests` と `JsonBenchmark` に同じ実装が
 *        複製されていた。2-4 で `LogConfigIssues` が 2 箇所に複製され、
 *        片方だけ機能が欠けていたのと同型なので 1 本に寄せてある。
 */

#include <cstddef>
#include <cstdio>

#include "Core/RootPath.h"

namespace GLFD::Test {

  /**
   * @brief リポジトリのルート(`<root>`)を UTF-8 で。末尾に区切りは付かない
   * @note  探し方は `Core::FindRoot`(ゲームと同じ)。見つからなければ空文字列を返す
   */
  inline const char* RepositoryRoot() {
    static char root[GLFD::Core::kRootPathCapacity * 3];
    static bool initialized = false;
    if (!initialized) {
      root[0] = 0;
      static GLFD::Core::RootSearch search;   // 大きいので static に置く
      GLFD::Core::FindRoot(search);
      if (search.Root() != nullptr) {
        const int converted = ::WideCharToMultiByte(CP_UTF8, 0, search.Root(), -1,
                                                    root, sizeof(root), nullptr, nullptr);
        if (converted <= 0) { root[0] = 0; }
      }
      initialized = true;
    }
    return root;
  }

  /**
   * @brief `<root>\relativePath` を組み立てる
   * @param out          出力先。**呼び出し側のバッファ**(共有 static を返さない理由は上記)
   * @param capacity     `out` の容量
   * @param relativePath `"Resource\\GameConfig.jsonc"` のようなリポジトリ相対パス
   */
  inline void RepositoryFile(char* out, size_t capacity, const char* relativePath) {
    std::snprintf(out, capacity, "%s\\%s", RepositoryRoot(), relativePath);
  }

}
