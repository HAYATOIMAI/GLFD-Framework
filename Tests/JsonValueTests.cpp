/**
 * @file  JsonValueTests.cpp
 * @brief Value / Member の単体テスト(フェーズ 1-3 / 要件 T-11, T-12)
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "TestHarness.h"
#include "MockMemoryResource.h"
#include "Core/Json/JsonValue.h"

using GLFD::StringView;
using GLFD::Json::JsonArena;
using GLFD::Json::kNullValue;
using GLFD::Json::Member;
using GLFD::Json::Value;
using GLFD::Json::ValueType;
using GLFD::Test::BeginCase;
using GLFD::Test::BeginSuite;
using GLFD::Test::MockMemoryResource;
using GLFD::Test::Summarize;

// ---------------------------------------------------------------------------
// T-11 静的検証
// ---------------------------------------------------------------------------

namespace {

  // --- レイアウトと trivial 性(ヘッダ内の static_assert と同じものを再掲し、
  //     テスト側からも破れていないことを保証する)---
  static_assert(sizeof(Value) <= 24, "Value must stay compact");
  static_assert(std::is_trivially_copyable_v<Value>);
  static_assert(std::is_trivially_destructible_v<Value>);
  static_assert(std::is_trivially_copyable_v<Member>);
  static_assert(std::is_trivially_destructible_v<Member>);

  // --- 既定構築は Null で、constexpr 文脈で評価できること ---
  static_assert(Value{}.Type() == ValueType::Null);
  static_assert(Value{}.IsNull());
  static_assert(!Value{}.IsNumber());
  static_assert(kNullValue.IsNull());

  // --- const 正当性 (R2-4) ---
  //     const Value& から変更系 API に到達できないことを concept で検出する。
  //     「コンパイルエラーになるコード」をコメントで残すのではなく、
  //     到達不能であること自体を静的に検証する
  template <class T> concept CanSetNull    = requires(T v) { v.SetNull(); };
  template <class T> concept CanSetBool    = requires(T v) { v.SetBool(true); };
  template <class T> concept CanSetInt64   = requires(T v) { v.SetInt64(1); };
  template <class T> concept CanSetDouble  = requires(T v) { v.SetDouble(1.0); };
  template <class T> concept CanSetStrRef  = requires(T v) { v.SetStringRef(StringView("x")); };
  template <class T> concept CanIndexWrite =
    requires(T v) { { v[0u] } -> std::same_as<Value&>; };
  template <class T> concept CanFindWrite =
    requires(T v) { { v.Find(StringView("k")) } -> std::same_as<Value*>; };

  static_assert(CanSetNull<Value&>);
  static_assert(CanSetBool<Value&>);
  static_assert(CanSetInt64<Value&>);
  static_assert(CanSetDouble<Value&>);
  static_assert(CanSetStrRef<Value&>);
  static_assert(CanIndexWrite<Value&>);
  static_assert(CanFindWrite<Value&>);

  static_assert(!CanSetNull<const Value&>,    "const Value must not reach SetNull");
  static_assert(!CanSetBool<const Value&>,    "const Value must not reach SetBool");
  static_assert(!CanSetInt64<const Value&>,   "const Value must not reach SetInt64");
  static_assert(!CanSetDouble<const Value&>,  "const Value must not reach SetDouble");
  static_assert(!CanSetStrRef<const Value&>,  "const Value must not reach SetStringRef");
  static_assert(!CanIndexWrite<const Value&>, "const Value must not yield a writable element");
  static_assert(!CanFindWrite<const Value&>,  "const Value must not yield a writable Find result");

  // --- 読み取り系は const からも到達できること(過剰に締めていないことの確認)---
  template <class T> concept CanRead =
    requires(T v) { v.Type(); v.IsNull(); v.IsNumber(); };
  static_assert(CanRead<const Value&>);

  // --- 確保系 API は戻り値を無視できないこと ([[nodiscard]]) ---
  template <class T>
  concept SetStringReturnsBool =
    requires(T v, JsonArena& a) { { v.SetString(StringView("x"), a) } -> std::same_as<bool>; };
  template <class T>
  concept PushBackReturnsPointer =
    requires(T v, JsonArena& a) { { v.PushBack(a) } -> std::same_as<Value*>; };
  static_assert(SetStringReturnsBool<Value&>);
  static_assert(PushBackReturnsPointer<Value&>);

  /// テストごとに使い捨てるアリーナ一式
  struct Fixture {
    MockMemoryResource mock;
    JsonArena          arena{ &mock };
  };

}

static void Test_StaticContracts() {
  BeginCase("static: layout, trivial-ness, and const correctness are enforced at compile time");

  // 上の static_assert 群が通っている時点で検証済み。
  // 実行時にもレイアウトを記録し、変化したら気づけるようにしておく
  CHECK(sizeof(Value) == 24);
  CHECK(alignof(Value) == 8);
  CHECK(sizeof(Member) == sizeof(StringView) + sizeof(Value));

  std::printf("      sizeof(Value)=%zu align=%zu  sizeof(Member)=%zu\n",
              sizeof(Value), alignof(Value), sizeof(Member));

  // 既定構築が Null であること
  const Value defaulted;
  CHECK(defaulted.IsNull());
  CHECK(defaulted.Type() == ValueType::Null);
  CHECK(!defaulted.IsNumber());

  // 共有の Null インスタンス
  CHECK(kNullValue.IsNull());
}

// ---------------------------------------------------------------------------
// スカラの設定と型判定
// ---------------------------------------------------------------------------

static void Test_ScalarTypes() {
  BeginCase("scalars: setters establish exactly one type");

  Value v;
  CHECK(v.IsNull());

  v.SetBool(true);
  CHECK(v.Type() == ValueType::Bool);
  CHECK(v.IsBool() && !v.IsNull() && !v.IsNumber());
  CHECK(v.GetBool());

  v.SetBool(false);
  CHECK(!v.GetBool());

  v.SetInt64(-42);
  CHECK(v.Type() == ValueType::Int64);
  CHECK(v.IsInt64() && v.IsNumber() && !v.IsBool());
  CHECK(v.GetInt64() == -42);

  v.SetUInt64(18446744073709551615ull);
  CHECK(v.Type() == ValueType::UInt64);
  CHECK(v.IsUInt64() && v.IsNumber());
  CHECK(v.GetUInt64() == 18446744073709551615ull);

  v.SetDouble(1.5);
  CHECK(v.Type() == ValueType::Double);
  CHECK(v.IsDouble() && v.IsNumber());
  CHECK(v.GetDouble() == 1.5);

  // 型を切り替えても古いペイロードが残らないこと
  v.SetNull();
  CHECK(v.IsNull());
  CHECK(!v.IsNumber());

  // 排他性: どの時点でも成立する型はひとつだけ
  const ValueType kinds[] = { ValueType::Null, ValueType::Bool, ValueType::Int64,
                              ValueType::UInt64, ValueType::Double };
  Value probe;
  for (const ValueType kind : kinds) {
    switch (kind) {
    case ValueType::Null:   probe.SetNull();      break;
    case ValueType::Bool:   probe.SetBool(true);  break;
    case ValueType::Int64:  probe.SetInt64(1);    break;
    case ValueType::UInt64: probe.SetUInt64(1);   break;
    case ValueType::Double: probe.SetDouble(1.0); break;
    default: break;
    }
    int matched = 0;
    matched += probe.IsNull()   ? 1 : 0;
    matched += probe.IsBool()   ? 1 : 0;
    matched += probe.IsInt64()  ? 1 : 0;
    matched += probe.IsUInt64() ? 1 : 0;
    matched += probe.IsDouble() ? 1 : 0;
    matched += probe.IsString() ? 1 : 0;
    matched += probe.IsArray()  ? 1 : 0;
    matched += probe.IsObject() ? 1 : 0;
    CHECK(matched == 1);
  }
}

// ---------------------------------------------------------------------------
// T-12 アクセサ: 変換規則と out 不変性
// ---------------------------------------------------------------------------

static void Test_IntegerConversions() {
  BeginCase("accessors: integer conversions succeed only when exactly representable");

  constexpr std::int64_t  kI64Max = (std::numeric_limits<std::int64_t>::max)();
  constexpr std::int64_t  kI64Min = (std::numeric_limits<std::int64_t>::min)();
  constexpr std::uint64_t kU64Max = (std::numeric_limits<std::uint64_t>::max)();
  constexpr std::int32_t  kI32Max = (std::numeric_limits<std::int32_t>::max)();
  constexpr std::int32_t  kI32Min = (std::numeric_limits<std::int32_t>::min)();
  constexpr std::uint32_t kU32Max = (std::numeric_limits<std::uint32_t>::max)();

  Value v;

  // --- Int64 → 各種 ---
  v.SetInt64(kI64Max);
  {
    std::int64_t  i64 = 0;
    std::uint64_t u64 = 0;
    std::int32_t  i32 = 0;
    CHECK(v.TryGetInt64(i64) && i64 == kI64Max);
    CHECK(v.TryGetUInt64(u64) && u64 == static_cast<std::uint64_t>(kI64Max));
    CHECK(!v.TryGetInt(i32));                      // int32 に収まらない
  }
  v.SetInt64(kI64Min);
  {
    std::uint64_t u64 = 12345;
    std::int32_t  i32 = 999;
    CHECK(!v.TryGetUInt64(u64));                   // 負値は符号なしへ変換できない
    CHECK(u64 == 12345);                           // 失敗時 out は不変
    CHECK(!v.TryGetInt(i32));
    CHECK(i32 == 999);
  }
  v.SetInt64(kI32Max);
  {
    std::int32_t i32 = 0;
    CHECK(v.TryGetInt(i32) && i32 == kI32Max);
  }
  v.SetInt64(static_cast<std::int64_t>(kI32Max) + 1);
  {
    std::int32_t i32 = 7;
    CHECK(!v.TryGetInt(i32));
    CHECK(i32 == 7);
  }
  v.SetInt64(kI32Min);
  {
    std::int32_t i32 = 0;
    CHECK(v.TryGetInt(i32) && i32 == kI32Min);
  }
  v.SetInt64(static_cast<std::int64_t>(kI32Min) - 1);
  {
    std::int32_t i32 = 7;
    CHECK(!v.TryGetInt(i32));
  }
  v.SetInt64(-1);
  {
    std::uint32_t u32 = 5;
    CHECK(!v.TryGetUInt(u32));                     // 負値
    CHECK(u32 == 5);
  }

  // --- UInt64 → 各種 ---
  v.SetUInt64(kU64Max);
  {
    std::int64_t  i64 = 3;
    std::uint64_t u64 = 0;
    std::uint32_t u32 = 3;
    CHECK(!v.TryGetInt64(i64));                    // int64 を超える
    CHECK(i64 == 3);
    CHECK(v.TryGetUInt64(u64) && u64 == kU64Max);
    CHECK(!v.TryGetUInt(u32));
    CHECK(u32 == 3);
  }
  v.SetUInt64(static_cast<std::uint64_t>(kI64Max));
  {
    std::int64_t i64 = 0;
    CHECK(v.TryGetInt64(i64) && i64 == kI64Max);   // 境界ぴったりは通る
  }
  v.SetUInt64(static_cast<std::uint64_t>(kI64Max) + 1);
  {
    std::int64_t i64 = 3;
    CHECK(!v.TryGetInt64(i64));                    // 境界 +1 は通らない
  }
  v.SetUInt64(kU32Max);
  {
    std::uint32_t u32 = 0;
    CHECK(v.TryGetUInt(u32) && u32 == kU32Max);
  }
  v.SetUInt64(static_cast<std::uint64_t>(kU32Max) + 1);
  {
    std::uint32_t u32 = 3;
    CHECK(!v.TryGetUInt(u32));
  }
}

static void Test_DoubleConversions() {
  BeginCase("accessors: Double <-> integer rejects silent precision loss");

  Value v;

  // --- Double → 整数: 小数部が無ければ成功 ---
  v.SetDouble(42.0);
  {
    std::int64_t  i64 = 0;
    std::int32_t  i32 = 0;
    std::uint64_t u64 = 0;
    std::uint32_t u32 = 0;
    CHECK(v.TryGetInt64(i64) && i64 == 42);
    CHECK(v.TryGetInt(i32) && i32 == 42);
    CHECK(v.TryGetUInt64(u64) && u64 == 42u);
    CHECK(v.TryGetUInt(u32) && u32 == 42u);
  }
  v.SetDouble(-42.0);
  {
    std::int64_t  i64 = 0;
    std::uint64_t u64 = 9;
    CHECK(v.TryGetInt64(i64) && i64 == -42);
    CHECK(!v.TryGetUInt64(u64));                   // 負値
    CHECK(u64 == 9);
  }

  // --- Double → 整数: 小数部があれば失敗(サイレント切り詰めをしない)---
  v.SetDouble(42.5);
  {
    std::int64_t  i64 = 9;
    std::int32_t  i32 = 9;
    std::uint64_t u64 = 9;
    CHECK(!v.TryGetInt64(i64));  CHECK(i64 == 9);
    CHECK(!v.TryGetInt(i32));    CHECK(i32 == 9);
    CHECK(!v.TryGetUInt64(u64)); CHECK(u64 == 9);
  }
  v.SetDouble(-0.5);
  {
    std::int64_t i64 = 9;
    CHECK(!v.TryGetInt64(i64));
  }

  // --- Double → 整数: 範囲外 / NaN / Inf は失敗 ---
  v.SetDouble(1e300);
  {
    std::int64_t i64 = 9;
    CHECK(!v.TryGetInt64(i64));  CHECK(i64 == 9);
  }
  v.SetDouble(std::numeric_limits<double>::quiet_NaN());
  {
    std::int64_t  i64 = 9;
    std::uint64_t u64 = 9;
    CHECK(!v.TryGetInt64(i64));                    // NaN は比較がすべて false
    CHECK(!v.TryGetUInt64(u64));
  }
  v.SetDouble(std::numeric_limits<double>::infinity());
  {
    std::int64_t i64 = 9;
    CHECK(!v.TryGetInt64(i64));
  }

  // --- 整数 → Double: 往復一致する範囲だけ通す ---
  v.SetInt64(1LL << 53);
  {
    double d = 0.0;
    CHECK(v.TryGetDouble(d) && d == 9007199254740992.0);   // 2^53 ぴったりは通る
  }
  v.SetInt64((1LL << 53) + 1);
  {
    double d = -1.0;
    CHECK(!v.TryGetDouble(d));                     // 2^53 超は桁落ちするので拒否
    CHECK(d == -1.0);
  }
  v.SetInt64((std::numeric_limits<std::int64_t>::max)());
  {
    double d = -1.0;
    CHECK(!v.TryGetDouble(d));
  }
  v.SetInt64(-(1LL << 53));
  {
    double d = 0.0;
    CHECK(v.TryGetDouble(d) && d == -9007199254740992.0);
  }
  v.SetUInt64((1ULL << 53) + 1);
  {
    double d = -1.0;
    CHECK(!v.TryGetDouble(d));
  }
  v.SetDouble(0.1);
  {
    double d = 0.0;
    CHECK(v.TryGetDouble(d) && d == 0.1);
  }

  // --- 符号付きゼロが保たれること(R1-19 と対になる)---
  v.SetDouble(-0.0);
  {
    double d = 1.0;
    CHECK(v.TryGetDouble(d));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    CHECK((bits >> 63) != 0);                      // 符号ビットが立っている
  }
}

static void Test_FloatConversions() {
  BeginCase("accessors: float allows rounding but rejects overflow");

  Value v;

  // 丸めは許容する(手書き config の 0.1 が読めなくなるため)
  v.SetDouble(0.1);
  {
    float f = 0.0f;
    CHECK(v.TryGetFloat(f));
    CHECK(f == 0.1f);
    CHECK(static_cast<double>(f) != 0.1);          // 実際に丸められている
  }
  v.SetDouble(1.0 / 3.0);
  {
    float f = 0.0f;
    CHECK(v.TryGetFloat(f));
    CHECK(f == static_cast<float>(1.0 / 3.0));
  }

  // 整数からの丸めも許容
  v.SetInt64((1LL << 53) + 1);
  {
    float f = 0.0f;
    CHECK(v.TryGetFloat(f));                       // Double は拒否するが float は通す
  }

  // float の表現域を超える有限値は失敗(inf へ化けさせない)
  v.SetDouble(1e300);
  {
    float f = 7.0f;
    CHECK(!v.TryGetFloat(f));
    CHECK(f == 7.0f);
  }
  v.SetDouble(-1e300);
  {
    float f = 7.0f;
    CHECK(!v.TryGetFloat(f));
  }

  // NaN / Inf はそのまま通す
  v.SetDouble(std::numeric_limits<double>::quiet_NaN());
  {
    float f = 0.0f;
    CHECK(v.TryGetFloat(f));
    CHECK(f != f);
  }
  v.SetDouble(std::numeric_limits<double>::infinity());
  {
    float f = 0.0f;
    CHECK(v.TryGetFloat(f));
    CHECK(f == std::numeric_limits<float>::infinity());
  }
  v.SetDouble(-std::numeric_limits<double>::infinity());
  {
    float f = 0.0f;
    CHECK(v.TryGetFloat(f));
    CHECK(f == -std::numeric_limits<float>::infinity());
  }

  // アンダーフローは 0 へ丸める(失敗にしない)
  v.SetDouble(1e-300);
  {
    float f = 7.0f;
    CHECK(v.TryGetFloat(f));
    CHECK(f == 0.0f);
  }
}

static void Test_TypeMismatch() {
  BeginCase("accessors: type mismatch fails and leaves out untouched");

  Fixture fx;

  Value str;
  CHECK(str.SetString(StringView("hello"), fx.arena));

  {
    bool          b   = true;
    std::int64_t  i64 = 11;
    std::uint64_t u64 = 22;
    std::int32_t  i32 = 33;
    std::uint32_t u32 = 44;
    double        d   = 55.0;
    float         f   = 66.0f;

    CHECK(!str.TryGetBool(b));    CHECK(b == true);
    CHECK(!str.TryGetInt64(i64)); CHECK(i64 == 11);
    CHECK(!str.TryGetUInt64(u64));CHECK(u64 == 22);
    CHECK(!str.TryGetInt(i32));   CHECK(i32 == 33);
    CHECK(!str.TryGetUInt(u32));  CHECK(u32 == 44);
    CHECK(!str.TryGetDouble(d));  CHECK(d == 55.0);
    CHECK(!str.TryGetFloat(f));   CHECK(f == 66.0f);
  }

  // 数値から文字列は取れない
  {
    Value number;
    number.SetInt64(1);
    StringView sv("untouched");
    CHECK(!number.TryGetString(sv));
    CHECK(sv == StringView("untouched"));
  }

  // Null からは何も取れない
  {
    const Value nul;
    bool         b   = true;
    std::int64_t i64 = 11;
    StringView   sv("untouched");
    CHECK(!nul.TryGetBool(b));   CHECK(b == true);
    CHECK(!nul.TryGetInt64(i64));CHECK(i64 == 11);
    CHECK(!nul.TryGetString(sv));CHECK(sv == StringView("untouched"));
  }

  // Bool は数値ではない(JSON の型として区別する)
  {
    Value flag;
    flag.SetBool(true);
    std::int64_t i64 = 11;
    double       d   = 55.0;
    CHECK(!flag.IsNumber());
    CHECK(!flag.TryGetInt64(i64)); CHECK(i64 == 11);
    CHECK(!flag.TryGetDouble(d));  CHECK(d == 55.0);
  }

#ifdef NDEBUG
  // Release では高速系アクセサが assert せず型ごとの既定値を返す。
  // Debug では assert が発火して停止するため、この検証は Release 限定
  {
    const Value nul;
    CHECK(nul.GetBool() == false);
    CHECK(nul.GetInt64() == 0);
    CHECK(nul.GetUInt64() == 0u);
    CHECK(nul.GetDouble() == 0.0);
    CHECK(nul.GetInt() == 0);
    CHECK(nul.GetUInt() == 0u);
    CHECK(nul.GetFloat() == 0.0f);
    CHECK(nul.GetString().Empty());
    CHECK(nul.Size() == 0u);
    CHECK(nul.MemberCount() == 0u);
  }
#endif
}

// ---------------------------------------------------------------------------
// 文字列
// ---------------------------------------------------------------------------

static void Test_Strings() {
  BeginCase("strings: SetString copies into the arena, SetStringRef does not");

  Fixture fx;

  // SetString はアリーナへコピーする → 元バッファを壊しても内容が保たれる
  {
    char source[] = "copied";
    Value v;
    CHECK(v.SetString(StringView(source, 6), fx.arena));
    CHECK(v.IsString());
    CHECK(v.GetString() == StringView("copied"));
    CHECK(v.GetString().Data() != source);

    std::memset(source, 0xCC, sizeof(source));
    CHECK(v.GetString() == StringView("copied"));   // コピー済みなので無傷
  }

  // SetStringRef は参照のみ → 元バッファを指したまま
  {
    const char* literal = "referenced";
    Value v;
    CHECK(v.SetStringRef(StringView(literal)));
    CHECK(v.GetString() == StringView("referenced"));
    CHECK(v.GetString().Data() == literal);
  }

  // 空文字列は確保しない
  {
    const int before = fx.mock.AllocateCalls();
    Value v;
    CHECK(v.SetString(StringView(""), fx.arena));
    CHECK(v.IsString());
    CHECK(v.GetString().Empty());
    CHECK(fx.mock.AllocateCalls() == before);
  }

  // 埋め込みヌルを含む文字列も長さで扱えること
  {
    const char raw[5] = { 'a', '\0', 'b', '\0', 'c' };
    Value v;
    CHECK(v.SetString(StringView(raw, 5), fx.arena));
    CHECK(v.GetString().Size() == 5);
    CHECK(v.GetString() == StringView(raw, 5));
  }

  // TryGetString と GetString が一致すること
  {
    Value v;
    CHECK(v.SetString(StringView("abc"), fx.arena));
    StringView out;
    CHECK(v.TryGetString(out));
    CHECK(out == v.GetString());
  }
}

// ---------------------------------------------------------------------------
// 配列
// ---------------------------------------------------------------------------

static void Test_Arrays() {
  BeginCase("arrays: SetArray reserves, PushBack grows, elements stay addressable");

  Fixture fx;

  // 事前確保して埋める → 伸長は一度も起きない
  {
    Value v;
    CHECK(v.SetArray(3, fx.arena));
    CHECK(v.IsArray());
    CHECK(v.Size() == 0u);
    CHECK(v.Empty());

    const int allocsAfterReserve = fx.mock.AllocateCalls();

    for (std::int64_t i = 0; i < 3; ++i) {
      Value* const slot = v.PushBack(fx.arena);
      CHECK(slot != nullptr);
      if (slot != nullptr) {
        slot->SetInt64(i * 10);
      }
    }
    CHECK(v.Size() == 3u);
    CHECK(!v.Empty());
    CHECK(v[0u].GetInt64() == 0);
    CHECK(v[1u].GetInt64() == 10);
    CHECK(v[2u].GetInt64() == 20);
    // 事前確保ぶんで足りているので、追加のブロック要求は発生しない
    CHECK(fx.mock.AllocateCalls() == allocsAfterReserve);
  }

  // 容量ゼロから伸長する
  {
    Value v;
    CHECK(v.SetArray(0, fx.arena));
    CHECK(v.IsArray() && v.Size() == 0u);

    for (std::int64_t i = 0; i < 50; ++i) {
      Value* const slot = v.PushBack(fx.arena);
      CHECK(slot != nullptr);
      if (slot != nullptr) {
        slot->SetInt64(i);
      }
    }
    CHECK(v.Size() == 50u);

    // 伸長で移送されても全要素が保たれていること
    bool allMatch = true;
    for (std::uint32_t i = 0; i < v.Size(); ++i) {
      if (v[i].GetInt64() != static_cast<std::int64_t>(i)) { allMatch = false; }
    }
    CHECK(allMatch);

    // Begin / End での走査
    std::int64_t expected = 0;
    bool         iterated = true;
    for (const Value* it = v.Begin(); it != v.End(); ++it, ++expected) {
      if (it->GetInt64() != expected) { iterated = false; }
    }
    CHECK(iterated);
    CHECK(expected == 50);
  }

  // 入れ子の配列
  {
    Value outer;
    CHECK(outer.SetArray(2, fx.arena));
    Value* const first = outer.PushBack(fx.arena);
    CHECK(first != nullptr);
    if (first != nullptr) {
      CHECK(first->SetArray(2, fx.arena));
      Value* const inner = first->PushBack(fx.arena);
      CHECK(inner != nullptr);
      if (inner != nullptr) { inner->SetBool(true); }
    }
    CHECK(outer.Size() == 1u);
    CHECK(outer[0u].IsArray());
    CHECK(outer[0u].Size() == 1u);
    CHECK(outer[0u][0u].GetBool());
  }

  // 非配列に対する Begin / End は nullptr
  {
    const Value scalar;
    CHECK(scalar.Begin() == nullptr);
    CHECK(scalar.End() == nullptr);
  }
}

// ---------------------------------------------------------------------------
// オブジェクト
// ---------------------------------------------------------------------------

static void Test_Objects() {
  BeginCase("objects: linear Find, duplicate keys, and key copy semantics");

  Fixture fx;

  {
    Value v;
    CHECK(v.SetObject(4, fx.arena));
    CHECK(v.IsObject());
    CHECK(v.MemberCount() == 0u);

    Value* const a = v.AddMember(StringView("alpha"), fx.arena);
    Value* const b = v.AddMember(StringView("beta"), fx.arena);
    Value* const c = v.AddMember(StringView("gamma"), fx.arena);
    CHECK(a != nullptr && b != nullptr && c != nullptr);
    if (a != nullptr) { a->SetInt64(1); }
    if (b != nullptr) { b->SetInt64(2); }
    if (c != nullptr) { c->SetInt64(3); }

    CHECK(v.MemberCount() == 3u);

    // 先頭 / 中央 / 末尾 / 不在(線形走査の正しさ)
    CHECK(v.Has(StringView("alpha")));
    CHECK(v.Has(StringView("beta")));
    CHECK(v.Has(StringView("gamma")));
    CHECK(!v.Has(StringView("delta")));
    CHECK(v.Find(StringView("alpha")) != nullptr);
    CHECK(v.Find(StringView("delta")) == nullptr);
    CHECK(v[StringView("alpha")].GetInt64() == 1);
    CHECK(v[StringView("beta")].GetInt64() == 2);
    CHECK(v[StringView("gamma")].GetInt64() == 3);

    // 部分一致で誤ヒットしないこと
    CHECK(!v.Has(StringView("alph")));
    CHECK(!v.Has(StringView("alphaa")));
    CHECK(!v.Has(StringView("")));

    // メンバ走査
    std::uint32_t counted = 0;
    for (const Member* it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
      ++counted;
    }
    CHECK(counted == 3u);
    CHECK(v.MemberBegin()->key == StringView("alpha"));
  }

  // R2-3: キー重複は全件保持し、Find は最初の一致を返す
  {
    Value v;
    CHECK(v.SetObject(0, fx.arena));
    Value* const first  = v.AddMember(StringView("dup"), fx.arena);
    Value* const second = v.AddMember(StringView("dup"), fx.arena);
    Value* const third  = v.AddMember(StringView("dup"), fx.arena);
    CHECK(first != nullptr && second != nullptr && third != nullptr);
    if (first != nullptr)  { first->SetInt64(1); }
    if (second != nullptr) { second->SetInt64(2); }
    if (third != nullptr)  { third->SetInt64(3); }

    CHECK(v.MemberCount() == 3u);                       // 重複も数える
    CHECK(v[StringView("dup")].GetInt64() == 1);        // 最初の一致
    CHECK(v.Find(StringView("dup")) == &v.MemberBegin()->value);
  }

  // AddMember はキーをアリーナへコピーする
  {
    char key[] = "volatile";
    Value v;
    CHECK(v.SetObject(1, fx.arena));
    Value* const slot = v.AddMember(StringView(key, 8), fx.arena);
    CHECK(slot != nullptr);
    if (slot != nullptr) { slot->SetInt64(7); }

    std::memset(key, 0xCC, sizeof(key));
    CHECK(v.Has(StringView("volatile")));               // コピー済みなので無傷
    CHECK(v[StringView("volatile")].GetInt64() == 7);
  }

  // AddMemberRef はコピーしない
  {
    const char* literal = "static-key";
    Value v;
    CHECK(v.SetObject(1, fx.arena));
    Value* const slot = v.AddMemberRef(StringView(literal), fx.arena);
    CHECK(slot != nullptr);
    if (slot != nullptr) { slot->SetInt64(8); }
    CHECK(v.MemberBegin()->key.Data() == literal);
    CHECK(v[StringView("static-key")].GetInt64() == 8);
  }

  // メンバ数の多いオブジェクトで線形走査が正しいこと
  {
    Value v;
    CHECK(v.SetObject(0, fx.arena));

    char keys[64][8];
    for (int i = 0; i < 64; ++i) {
      std::snprintf(keys[i], sizeof(keys[i]), "k%03d", i);
      Value* const slot = v.AddMember(StringView(keys[i]), fx.arena);
      CHECK(slot != nullptr);
      if (slot != nullptr) { slot->SetInt64(i); }
    }
    CHECK(v.MemberCount() == 64u);

    bool allFound = true;
    for (int i = 0; i < 64; ++i) {
      const Value* const found = v.Find(StringView(keys[i]));
      if (found == nullptr || found->GetInt64() != i) { allFound = false; }
    }
    CHECK(allFound);
    CHECK(v.Find(StringView("k999")) == nullptr);
  }

  // 非オブジェクトに対する走査は nullptr
  {
    const Value scalar;
    CHECK(scalar.MemberBegin() == nullptr);
    CHECK(scalar.MemberEnd() == nullptr);
    CHECK(scalar.Find(StringView("x")) == nullptr);
    CHECK(!scalar.Has(StringView("x")));
  }
}

// ---------------------------------------------------------------------------
// 確保失敗
// ---------------------------------------------------------------------------

static void Test_AllocationFailure() {
  BeginCase("allocation failure: construction APIs report failure instead of aborting");

  // 文字列
  {
    MockMemoryResource mock;
    mock.SetFailAfter(0);
    JsonArena arena(&mock);

    Value v;
    CHECK(!v.SetString(StringView("abc"), arena));
    CHECK(v.IsNull() || v.IsString());              // 状態は壊れていない
    CHECK(!v.SetArray(4, arena));
    CHECK(!v.SetObject(4, arena));
  }

  // 配列の伸長
  {
    MockMemoryResource mock;
    JsonArena arena(&mock);

    Value v;
    CHECK(v.SetArray(0, arena));
    Value* const first = v.PushBack(arena);         // ここで最初のブロックを取る
    CHECK(first != nullptr);

    mock.SetFailAfter(mock.AllocateCalls());        // 以降の確保をすべて失敗させる
    // 容量 4 まではブロック内に収まるので成功し、伸長時に失敗する
    Value* slot = nullptr;
    for (int i = 0; i < 10000; ++i) {
      slot = v.PushBack(arena);
      if (slot == nullptr) { break; }
      slot->SetInt64(i);
    }
    CHECK(slot == nullptr);                         // どこかで必ず失敗する
    CHECK(mock.InjectedFailures() >= 1);
  }

  // オブジェクトのキー複製失敗
  {
    MockMemoryResource mock;
    JsonArena arena(&mock);

    Value v;
    CHECK(v.SetObject(4, arena));
    mock.SetFailAfter(mock.AllocateCalls());

    Value* slot = nullptr;
    for (int i = 0; i < 10000; ++i) {
      char key[16];
      std::snprintf(key, sizeof(key), "key%d", i);
      slot = v.AddMember(StringView(key), arena);
      if (slot == nullptr) { break; }
    }
    CHECK(slot == nullptr);

    // 失敗した AddMember がメンバ数を進めていないこと
    const std::uint32_t count = v.MemberCount();
    bool allValid = true;
    for (std::uint32_t i = 0; i < count; ++i) {
      if ((v.MemberBegin() + i)->key.Empty()) { allValid = false; }
    }
    CHECK(allValid);
  }

  // 失敗後もリークしないこと
  {
    MockMemoryResource mock;
    {
      JsonArena arena(&mock);
      Value v;
      CHECK(v.SetArray(0, arena));
      for (int i = 0; i < 100; ++i) {
        Value* const slot = v.PushBack(arena);
        if (slot != nullptr) { slot->SetInt64(i); }
      }
      mock.SetFailAfter(mock.AllocateCalls());
      for (int i = 0; i < 100000; ++i) {
        if (v.PushBack(arena) == nullptr) { break; }
      }
    }
    CHECK(mock.AllocateCalls() - mock.InjectedFailures() == mock.DeallocateCalls());
    CHECK(mock.LiveBlockCount() == 0);
    CHECK(!mock.DoubleFree());
    CHECK(!mock.SizeMismatch());
  }
}

// ---------------------------------------------------------------------------
// memcpy 移送 (R2-5)
// ---------------------------------------------------------------------------

static void Test_MemcpyMovable() {
  BeginCase("R2-5: Value trees survive a raw memcpy relocation");

  Fixture fx;

  Value original;
  CHECK(original.SetObject(2, fx.arena));
  Value* const name = original.AddMember(StringView("name"), fx.arena);
  Value* const nums = original.AddMember(StringView("nums"), fx.arena);
  CHECK(name != nullptr && nums != nullptr);
  if (name != nullptr) { CHECK(name->SetString(StringView("glfd"), fx.arena)); }
  if (nums != nullptr) {
    CHECK(nums->SetArray(2, fx.arena));
    Value* const a = nums->PushBack(fx.arena);
    Value* const b = nums->PushBack(fx.arena);
    if (a != nullptr) { a->SetInt64(1); }
    if (b != nullptr) { b->SetDouble(2.5); }
  }

  // 生の memcpy で別の記憶域へ移す
  alignas(Value) unsigned char storage[sizeof(Value)];
  std::memcpy(storage, &original, sizeof(Value));
  const Value* const moved = reinterpret_cast<const Value*>(storage);

  CHECK(moved->IsObject());
  CHECK(moved->MemberCount() == 2u);
  CHECK((*moved)[StringView("name")].GetString() == StringView("glfd"));
  CHECK((*moved)[StringView("nums")].Size() == 2u);
  CHECK((*moved)[StringView("nums")][0u].GetInt64() == 1);
  CHECK((*moved)[StringView("nums")][1u].GetDouble() == 2.5);
}

// ---------------------------------------------------------------------------

int main() {
  BeginSuite("JsonValue");

  Test_StaticContracts();
  Test_ScalarTypes();
  Test_IntegerConversions();
  Test_DoubleConversions();
  Test_FloatConversions();
  Test_TypeMismatch();
  Test_Strings();
  Test_Arrays();
  Test_Objects();
  Test_AllocationFailure();
  Test_MemcpyMovable();

  return Summarize();
}
