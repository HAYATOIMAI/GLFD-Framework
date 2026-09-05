#pragma once

/**
 * @file  JsonFileIO.h
 * @brief ファイル全体をアリーナ上へ読み込む単一の自由関数
 *
 * @details
 *  `Document::ParseFile` が使う唯一のファイル I/O 経路。**将来 `IFileSystem` 等の
 *  抽象が生まれたときの差し替え点**として、意図的にこの 1 関数へ隔離している。
 *
 *  抽象自体を今作らないのは、消費者がこの 1 つしかない状態でインタフェースを
 *  設計すると形を誤るためである。2 つ目の消費者が現れた時点で、
 *  この関数のシグネチャを出発点に抽象化すればよい。
 *
 *  既存の `GLFD::Core::FileManager` は使わない。`std::string` / `std::vector` /
 *  `std::filesystem` / `std::fstream` に依存しており N-1 / N-2 に抵触するため。
 */

#include "Core/StringView.h"
#include "Core/Json/JsonArena.h"
#include "Core/Json/JsonTypes.h"

namespace GLFD::Json {

  /**
   * @brief ファイルの内容を丸ごとアリーナ上へ読み込む
   *
   * @param utf8Path    ファイルパス。**UTF-8 かつヌル終端**であること
   * @param arena       読み込み先。内容はこのアリーナが生きている間だけ有効
   * @param outContents 成功時、アリーナ上の内容を指すビュー(**ヌル終端しない**)
   * @param outError    失敗時の理由。成功時は `ErrorCode::None`
   * @return 成功したら true
   *
   * @details
   *  ## パスの文字コード
   *  パスは **UTF-8 と規定する**。内部で `MultiByteToWideChar(CP_UTF8, ...)` により
   *  UTF-16 へ変換し、`CreateFileW` で開く。`CreateFileA` を使わないのは、
   *  あちらがパスを ANSI (システムロケール) として解釈するため、
   *  **日本語を含むパスを開けない**からである。
   *  変換バッファはアリーナから確保する(`new` / `malloc` を直接呼ばない)。
   *
   *  ## エラーの粒度
   *  | 状況 | outError |
   *  |---|---|
   *  | パスが UTF-8 として不正 | `PathEncodingInvalid`(直すのは呼び出し側のコード) |
   *  | パスが存在しない | `FileNotFound` |
   *  | 開けたが読めない / 権限・共有違反・I/O エラー | `FileReadFailed` |
   *  | アリーナの確保に失敗 | `OutOfMemory` |
   *
   *  ## 空ファイル
   *  成功として扱い、`outContents` は空のビューになる。
   *  「空ファイル = `DocumentEmpty`」の判断は上位(パーサ)の責務である。
   *
   *  @note 読み込んだ内容は**ファイルハンドルを閉じた後も有効**である
   *        (アリーナ上にあるため)。
   */
  [[nodiscard]] bool ReadEntireFile(const char* utf8Path, JsonArena& arena,
                                    StringView& outContents, ErrorCode& outError) noexcept;

  // ---------------------------------------------------------------------------
  // 書き込み側(R2-10 と同じく Win32 呼び出しは .cpp に隔離する)
  // ---------------------------------------------------------------------------

  /**
   * @brief 書き込み中のファイルを表す不透明なハンドル
   * @note  Win32 の型をヘッダへ漏らさないため `HANDLE` は `void*` で持つ。
   *        パスはアリーナ上の UTF-16 文字列を指す
   */
  struct FileWriteHandle {
    void*          osHandle  = nullptr;   ///< 実体は Win32 の HANDLE
    const wchar_t* tempPath  = nullptr;   ///< 実際に書いている一時ファイル
    const wchar_t* finalPath = nullptr;   ///< 置き換え先
    bool           open      = false;
  };

  /**
   * @brief 書き込みを開始する。**実際には一時ファイルを開く**
   *
   * @details
   *  ## なぜ一時ファイルへ書くのか
   *  目標ファイルを直接上書きすると、書き込み途中でクラッシュした場合に
   *  **既存のセーブデータごと失われる**。一時ファイルに書き切ってから
   *  `CommitFileWrite` で置き換えることで、失敗しても元のファイルが無傷で残る。
   *
   *  一時ファイル名は `<目標パス>.<pid>.<tick>.tmp`。**目標と同じディレクトリ**に
   *  作るのは、置き換えに使う `MoveFileEx` が同一ボリューム内でないと
   *  原子的に動作しないため。静的カウンタを使わず pid と tick で一意性を得ているのは
   *  グローバル可変状態を持たないため (N-3)。
   *
   * @return 失敗したら false。`outError` は `PathEncodingInvalid` /
   *         `FileNotFound`(ディレクトリが無い等)/ `FileWriteFailed` / `OutOfMemory`
   */
  [[nodiscard]] bool BeginFileWrite(const char* utf8Path, JsonArena& arena,
                                    FileWriteHandle& outHandle, ErrorCode& outError) noexcept;

  /// 一時ファイルへ書き込む。部分書き込みは内部でループして解消する
  [[nodiscard]] bool WriteFileChunk(FileWriteHandle& handle, const char* data, size_t size,
                                    ErrorCode& outError) noexcept;

  /**
   * @brief 一時ファイルを閉じ、目標ファイルへ置き換えて完了する
   * @note  失敗した場合は一時ファイルを削除する。**目標ファイルは無傷のまま残る**
   */
  [[nodiscard]] bool CommitFileWrite(FileWriteHandle& handle, ErrorCode& outError) noexcept;

  /**
   * @brief 書き込みを破棄する。一時ファイルを閉じて削除する
   * @note  `Commit` せずに破棄された場合の後始末。**一時ファイルを残さない**。
   *        既に閉じているハンドルに対しては何もしない
   */
  void AbortFileWrite(FileWriteHandle& handle) noexcept;

}
