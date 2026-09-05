/**
 * @file  StringViewTests.cpp
 * @brief StringView の単体テスト(GLFD JSON サブシステム フェーズ 1-2a / 要件 R0-6 〜 R0-10)
 */

#include <cstddef>
#include <cstring>

#include "TestHarness.h"
#include "Core/StringView.h"

using GLFD::StringView;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::Summarize;

// ---------------------------------------------------------------------------
// 1. constexpr 文脈での評価 (R0-10)
//    -- static_assert が通ること自体がテストなので、実行時の検証は不要
// ---------------------------------------------------------------------------

namespace {

  // 既定構築
  static_assert(StringView{}.Size() == 0);
  static_assert(StringView{}.Empty());
  static_assert(StringView{}.Data() == nullptr);

  // ヌル終端文字列からの構築(非 explicit)
  static_assert(StringView("hello").Size() == 5);
  static_assert(StringView("").Size() == 0);
  static_assert(StringView("").Empty());
  static_assert(StringView(static_cast<const char*>(nullptr)).Size() == 0);

  // ポインタ + 長さからの構築
  static_assert(StringView("hello", 3).Size() == 3);

  // operator[]
  static_assert(StringView("hello")[0] == 'h');
  static_assert(StringView("hello")[4] == 'o');

  // ==  /  !=
  static_assert(StringView("hello") == StringView("hello"));
  static_assert(StringView("hello") != StringView("hell"));
  static_assert(StringView("hello") != StringView("hellO"));
  static_assert(StringView("hello", 4) == StringView("hell"));
  static_assert(StringView{} == StringView(""));
  static_assert(StringView{} == StringView(nullptr, 0));

  // SubStr
  static_assert(StringView("hello").SubStr(1, 3) == StringView("ell"));
  static_assert(StringView("hello").SubStr(2) == StringView("llo"));       // count 既定 = 残り全部
  static_assert(StringView("hello").SubStr(5).Empty());                    // offset == Size()
  static_assert(StringView("hello").SubStr(99).Empty());                   // offset > Size() はクランプ
  static_assert(StringView("hello").SubStr(1, 99) == StringView("ello"));  // count は残りへクランプ
  static_assert(StringView("hello").SubStr(0, 0).Empty());

  // StartsWith / EndsWith
  static_assert(StringView("hello").StartsWith(StringView("hel")));
  static_assert(StringView("hello").StartsWith(StringView("hello")));
  static_assert(StringView("hello").StartsWith(StringView("")));          // 空は常に true
  static_assert(!StringView("hello").StartsWith(StringView("hello!")));   // より長い
  static_assert(!StringView("hello").StartsWith(StringView("ello")));
  static_assert(StringView("hello").EndsWith(StringView("llo")));
  static_assert(StringView("hello").EndsWith(StringView("hello")));
  static_assert(StringView("hello").EndsWith(StringView("")));
  static_assert(!StringView("hello").EndsWith(StringView("hell")));
  static_assert(!StringView("hello").EndsWith(StringView("xhello")));
  static_assert(StringView{}.StartsWith(StringView("")));
  static_assert(StringView{}.EndsWith(StringView("")));

  // Find
  static_assert(StringView("hello").Find('l') == 2);
  static_assert(StringView("hello").Find('l', 3) == 3);
  static_assert(StringView("hello").Find('l', 4) == StringView::npos);
  static_assert(StringView("hello").Find('z') == StringView::npos);
  static_assert(StringView("hello").Find('h', 99) == StringView::npos);   // from が範囲外
  static_assert(StringView{}.Find('a') == StringView::npos);

}

// ---------------------------------------------------------------------------
// 2. 実行時の基本動作
// ---------------------------------------------------------------------------

static void Test_Basics() {
  BeginCase("basics: construction, size, indexing, iteration");

  const StringView empty;
  CHECK(empty.Empty());
  CHECK(empty.Size() == 0);
  CHECK(empty.Data() == nullptr);
  CHECK(empty.begin() == empty.end());

  const char* literal = "GLFD";
  const StringView view(literal);
  CHECK(view.Size() == 4);
  CHECK(!view.Empty());
  CHECK(view.Data() == literal);
  CHECK(view[0] == 'G');
  CHECK(view[3] == 'D');
  CHECK(static_cast<size_t>(view.end() - view.begin()) == view.Size());

  // range-for が使えること
  size_t counted = 0;
  for (const char c : view) {
    CHECK(c == literal[counted]);
    ++counted;
  }
  CHECK(counted == 4);

  // nullptr からの構築は空になる(クラッシュしない)
  const StringView fromNull(static_cast<const char*>(nullptr));
  CHECK(fromNull.Empty());
  CHECK(fromNull.Size() == 0);
}

// ---------------------------------------------------------------------------
// 3. ヌル終端されていないバッファ (R0-7 の核心)
// ---------------------------------------------------------------------------

static void Test_NonNullTerminatedBuffer() {
  BeginCase("non-terminated: view respects its bounds, ignoring surrounding bytes");

  // 前後をゴミで挟んだ、ヌル終端されていない領域を用意する。
  // strlen 相当の処理が混入していれば、ここで境界を踏み越えて失敗する
  char buffer[16];
  std::memset(buffer, 'X', sizeof(buffer));
  buffer[4] = 'a';
  buffer[5] = 'b';
  buffer[6] = 'c';
  // buffer[7] 以降も 'X' のまま。終端バイトはどこにも無い

  const StringView view(buffer + 4, 3);
  CHECK(view.Size() == 3);
  CHECK(view == StringView("abc"));
  CHECK(view != StringView("abcX"));
  CHECK(view != StringView("abc", 2));
  CHECK(view.StartsWith(StringView("ab")));
  CHECK(view.EndsWith(StringView("bc")));
  CHECK(!view.EndsWith(StringView("cX")));
  CHECK(view.Find('X') == StringView::npos);   // 後続のゴミを拾わないこと
  CHECK(view.SubStr(1) == StringView("bc"));
  CHECK(view.SubStr(1).Size() == 2);

  // 同じ内容を別のバッファに置いても内容比較で一致すること
  char other[3] = { 'a', 'b', 'c' };
  const StringView otherView(other, 3);
  CHECK(view == otherView);
  CHECK(view.Data() != otherView.Data());
}

// ---------------------------------------------------------------------------
// 4. 埋め込みヌルを含むビュー
// ---------------------------------------------------------------------------

static void Test_EmbeddedNul() {
  BeginCase("embedded nul: size and comparison cover the whole range");

  const char rawA[6] = { 'a', '\0', 'b', '\0', 'c', '\0' };
  const char rawB[6] = { 'a', '\0', 'b', '\0', 'c', '\0' };
  const char rawC[6] = { 'a', '\0', 'b', '\0', 'd', '\0' };

  const StringView a(rawA, 5);   // "a\0b\0c" ('\0' を 2 つ内包する)
  const StringView b(rawB, 5);
  const StringView c(rawC, 5);

  CHECK(a.Size() == 5);                      // 最初の '\0' で切れないこと
  CHECK(!a.Empty());
  CHECK(a[1] == '\0');
  CHECK(a == b);                             // '\0' の先まで比較されること
  CHECK(a != c);                             // 差異は 5 バイト目にしかない
  CHECK(a != StringView(rawA, 1));           // "a" とは長さが違う
  CHECK(a.Find('\0') == 1);
  CHECK(a.Find('\0', 2) == 3);
  CHECK(a.Find('c') == 4);
  CHECK(a.SubStr(2, 2) == StringView(rawB + 2, 2));
  CHECK(a.StartsWith(StringView(rawB, 2)));  // "a\0"
  CHECK(a.EndsWith(StringView(rawB + 3, 2)));

  // ヌル終端コンストラクタは最初の '\0' で止まる(こちらは仕様どおり)
  const StringView terminated(rawA);
  CHECK(terminated.Size() == 1);
  CHECK(terminated == StringView("a"));
}

// ---------------------------------------------------------------------------
// 5. 境界条件
// ---------------------------------------------------------------------------

static void Test_Boundaries() {
  BeginCase("boundaries: npos, offset == Size(), oversized count");

  const StringView view("abcdef");

  // SubStr のクランプ
  CHECK(view.SubStr(0) == view);
  CHECK(view.SubStr(0, StringView::npos) == view);
  CHECK(view.SubStr(6).Empty());                       // offset == Size()
  CHECK(view.SubStr(6).Size() == 0);
  CHECK(view.SubStr(7).Empty());                       // offset > Size()
  CHECK(view.SubStr(StringView::npos).Empty());        // 極端な offset
  CHECK(view.SubStr(3, 100) == StringView("def"));     // count > 残り
  CHECK(view.SubStr(3, StringView::npos) == StringView("def"));
  CHECK(view.SubStr(2, 0).Empty());

  // 二重の SubStr でも境界を保つこと
  CHECK(view.SubStr(1, 4).SubStr(1, 2) == StringView("cd"));
  CHECK(view.SubStr(1, 4).SubStr(10).Empty());

  // 空ビューに対する操作
  const StringView empty;
  CHECK(empty.SubStr(0).Empty());
  CHECK(empty.SubStr(5).Empty());
  CHECK(empty.Find('a') == StringView::npos);
  CHECK(empty.StartsWith(StringView("")));
  CHECK(!empty.StartsWith(StringView("a")));
  CHECK(empty == StringView(""));
  CHECK(empty != StringView("a"));

  // Find の from 境界
  CHECK(view.Find('a', 0) == 0);
  CHECK(view.Find('a', 1) == StringView::npos);
  CHECK(view.Find('f', 5) == 5);
  CHECK(view.Find('f', 6) == StringView::npos);
  CHECK(view.Find('f', StringView::npos) == StringView::npos);
}

// ---------------------------------------------------------------------------
// 6. 比較の意味論
// ---------------------------------------------------------------------------

static void Test_Comparison() {
  BeginCase("comparison: content equality, not pointer equality");

  const char* text = "compare";
  const StringView a(text);
  const StringView b(text);            // 同じポインタ
  char copy[8];
  std::memcpy(copy, text, 8);
  const StringView c(copy, 7);         // 別バッファの同内容

  CHECK(a == b);
  CHECK(a == c);
  CHECK(a.Data() != c.Data());

  // 長さが違えば内容の接頭辞が一致しても不一致
  CHECK(a != StringView(text, 6));
  CHECK(StringView(text, 6) != a);

  // 1 バイトだけ異なるケース(先頭 / 中間 / 末尾)
  CHECK(StringView("abc") != StringView("xbc"));
  CHECK(StringView("abc") != StringView("axc"));
  CHECK(StringView("abc") != StringView("abx"));

  // 空同士はすべて等しい
  CHECK(StringView() == StringView(""));
  CHECK(StringView("abc", 0) == StringView());
  CHECK(StringView(nullptr, 0) == StringView(""));

  // 長い文字列(実行時は memcmp 経路になる)でも一致すること
  char longA[512];
  char longB[512];
  for (size_t i = 0; i < sizeof(longA); ++i) {
    longA[i] = static_cast<char>('a' + (i % 26));
    longB[i] = longA[i];
  }
  CHECK(StringView(longA, sizeof(longA)) == StringView(longB, sizeof(longB)));
  longB[511] = '!';
  CHECK(StringView(longA, sizeof(longA)) != StringView(longB, sizeof(longB)));
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("StringView");

  Test_Basics();
  Test_NonNullTerminatedBuffer();
  Test_EmbeddedNul();
  Test_Boundaries();
  Test_Comparison();

  return Summarize();
}
