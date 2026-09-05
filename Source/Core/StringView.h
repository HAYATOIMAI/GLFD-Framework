#pragma once

/**
 * @file  StringView.h
 * @brief 非所有の文字列ビュー(エンジン全体で使う汎用型)
 *
 * @warning **このビューはヌル終端を保証しない (R0-7)。**
 *  JSON の文字列はアリーナ上の連続領域のスライスとして表現されるため、
 *  Data() の指す先に終端バイトが存在しない。したがって、
 *
 *      -  Data() を C 文字列として扱ってはならない
 *      -  Data() を fopen / CreateFile / MessageBox 等の Win32 API や
 *         printf("%s") へ**そのまま渡してはならない**(バッファ外まで読む)
 *      -  printf 系へ渡すなら "%.*s" と (int)Size() を併用すること
 *
 *  この帰結として、**パス文字列を受け取る API は StringView を取らない (R0-8)。**
 *  Document::ParseFile / SaveToJsonFile 等はヌル終端が保証された const char* を
 *  受けるか、内部でアリーナ上にヌル終端コピーを作ってから OS API へ渡すこと。
 */

#include <cstddef>
#include <cstring>
#include <cassert>
#include <type_traits>

namespace GLFD {

  /**
   * @brief 連続した char 列への非所有ビュー
   *
   * @details
   *  ## 所有権 (R0-6)
   *  非所有。メモリの確保・解放・生存期間の管理を一切持たない。
   *  参照先より長生きした瞬間にダングリングになるのは呼び出し側の責任である。
   *
   *  ## ヌル終端 (R0-7)
   *  保証しない。ファイル先頭の @warning を参照すること。
   *
   *  ## 例外 (R0-9)
   *  throw しない。operator[] の範囲外アクセスは Debug で assert、
   *  Release では未チェック(未定義動作)。安全側が必要なら呼び出し側で
   *  Size() を確認すること。
   *
   *  ## constexpr (R0-10)
   *  全ての操作が constexpr。ヘッダオンリーで .cpp を持たない。
   *
   *  ## 埋め込みヌル
   *  内容として '\\0' を含んでよい。Size() は常に構築時に与えられた長さであり、
   *  最初の '\\0' までの長さではない。比較も Size() バイト全体を対象とする。
   */
  class StringView {
  public:
    /// 「見つからない」を表す位置
    static constexpr size_t npos = static_cast<size_t>(-1);

    /// 空のビュー。Data() は nullptr、Size() は 0
    constexpr StringView() noexcept = default;

    /**
     * @brief ポインタと長さから構築する
     * @param data 先頭。size が 0 なら nullptr でもよい
     * @param size バイト数。ヌル終端は不要
     */
    constexpr StringView(const char* data, size_t size) noexcept
      : m_data(data)
      , m_size(size) {
    }

    /**
     * @brief ヌル終端文字列から構築する(非 explicit)
     * @param nullTerminated ヌル終端文字列。nullptr なら空のビューになる
     * @note  終端の '\\0' は Size() に含まれない。文字列リテラルを
     *        そのまま渡せるようにするため意図的に非 explicit にしている
     */
    constexpr StringView(const char* nullTerminated) noexcept
      : m_data(nullTerminated)
      , m_size(Length(nullTerminated)) {
    }

    [[nodiscard]] constexpr const char* Data()  const noexcept { return m_data; }
    [[nodiscard]] constexpr size_t      Size()  const noexcept { return m_size; }
    [[nodiscard]] constexpr bool        Empty() const noexcept { return m_size == 0; }

    /**
     * @brief i 番目のバイトを返す
     * @note  範囲外は Debug で assert、Release では未定義 (R0-9)
     */
    [[nodiscard]] constexpr char operator[](size_t i) const noexcept {
      assert(i < m_size && "StringView::operator[]: index out of range");
      return m_data[i];
    }

    // range-for 用。規約に合わせず小文字なのは、言語機能が要求する名前であるため
    [[nodiscard]] constexpr const char* begin() const noexcept { return m_data; }
    [[nodiscard]] constexpr const char* end()   const noexcept { return m_data + m_size; }

    /**
     * @brief 部分ビューを取り出す
     * @param offset 開始位置
     * @param count  取り出すバイト数。既定 (npos) は「残り全部」
     * @return 部分ビュー
     *
     * @note
     *  **クランプ挙動 (R0-10 の要求により明記する)**:
     *   - offset が Size() より大きい場合は Size() へクランプする。
     *     結果は空のビューになる。**assert しない。**
     *   - offset == Size() は正当な使い方で、空のビューを返す。
     *   - count が残りの長さを超える場合は残りの長さへクランプする。
     *  範囲外を「未定義」ではなく定義された安全な結果にしているのは、
     *  部分文字列の切り出しがパーサ内で多用され、境界の判定を
     *  呼び出し側に散らしたくないためである。
     *  (operator[] とは方針が異なる。あちらは 1 バイトの読み出しであり、
     *   安全化するとホットパスに分岐が入るため未定義のままにしてある)
     */
    [[nodiscard]] constexpr StringView SubStr(size_t offset, size_t count = npos) const noexcept {
      const size_t begin     = (offset < m_size) ? offset : m_size;
      const size_t remaining = m_size - begin;
      const size_t length    = (count < remaining) ? count : remaining;
      return StringView(m_data + begin, length);
    }

    /// s で始まるか。s が空なら常に true
    [[nodiscard]] constexpr bool StartsWith(StringView s) const noexcept {
      return (s.m_size <= m_size) && Equals(m_data, s.m_data, s.m_size);
    }

    /// s で終わるか。s が空なら常に true
    [[nodiscard]] constexpr bool EndsWith(StringView s) const noexcept {
      return (s.m_size <= m_size) && Equals(m_data + (m_size - s.m_size), s.m_data, s.m_size);
    }

    /**
     * @brief 文字 c を前方から探す
     * @param c    探す文字
     * @param from 探索開始位置。Size() 以上なら npos を返す
     * @return 見つかった位置。見つからなければ npos
     */
    [[nodiscard]] constexpr size_t Find(char c, size_t from = 0) const noexcept {
      for (size_t i = from; i < m_size; ++i) {
        if (m_data[i] == c) {
          return i;
        }
      }
      return npos;
    }

  private:
    /// ヌル終端文字列の長さ。nullptr は 0
    [[nodiscard]] static constexpr size_t Length(const char* s) noexcept {
      if (s == nullptr) {
        return 0;
      }
      size_t n = 0;
      while (s[n] != '\0') {
        ++n;
      }
      return n;
    }

    /**
     * @brief n バイトの内容比較
     * @note  定数評価中は素朴なループ、実行時は memcmp を使う。
     *        JSON のキー比較は線形走査 (R2-2) のホットパスになるため
     */
    [[nodiscard]] static constexpr bool Equals(const char* a, const char* b, size_t n) noexcept {
      if (n == 0) {
        return true;
      }
      if (a == b) {
        return true;
      }
      if (std::is_constant_evaluated()) {
        for (size_t i = 0; i < n; ++i) {
          if (a[i] != b[i]) {
            return false;
          }
        }
        return true;
      }
      return std::memcmp(a, b, n) == 0;
    }

    const char* m_data = nullptr;
    size_t      m_size = 0;

    friend constexpr bool operator==(StringView a, StringView b) noexcept;
  };

  /**
   * @brief 内容の一致を判定する
   * @note  長さが等しく、Size() バイトすべてが一致する場合のみ true。
   *        埋め込みの '\\0' も内容として比較される。
   *        ポインタの一致ではなく内容の一致であり、
   *        別々のバッファを指す等価なビュー同士は true になる
   */
  [[nodiscard]] constexpr bool operator==(StringView a, StringView b) noexcept {
    return (a.m_size == b.m_size) && StringView::Equals(a.m_data, b.m_data, a.m_size);
  }

  [[nodiscard]] constexpr bool operator!=(StringView a, StringView b) noexcept {
    return !(a == b);
  }

}
