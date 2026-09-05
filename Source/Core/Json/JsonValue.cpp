/**
 * @file  JsonValue.cpp
 * @brief Value の変換規則と、確保を伴う構築 API
 */

#include "Core/Json/JsonValue.h"

#include <cstring>

namespace GLFD::Json {

  namespace {

    /// double が int64_t の範囲に収まるか(境界値も含めて正しく判定する)
    constexpr double kInt64LowerBound  = -9223372036854775808.0;   // -2^63 (厳密)
    constexpr double kInt64UpperBound  =  9223372036854775808.0;   //  2^63 (厳密。未満であること)
    constexpr double kUInt64UpperBound = 18446744073709551616.0;   //  2^64 (厳密。未満であること)

    /// double で厳密に表せる整数の上限。これを超える整数は Double へ渡さない
    constexpr std::int64_t  kExactIntLimit  = 1LL << 53;
    constexpr std::uint64_t kExactUIntLimit = 1ULL << 53;

    /// 値が数値なら double へ丸めて返す(精度欠落を許す)
    [[nodiscard]] bool IsNaN(double v) noexcept { return v != v; }

  }

  // ---------------------------------------------------------------------------
  // 安全系アクセサ
  // ---------------------------------------------------------------------------

  bool Value::TryGetInt64(std::int64_t& out) const noexcept {
    switch (m_type) {
    case ValueType::Int64:
      out = m_payload.i;
      return true;

    case ValueType::UInt64:
      if (m_payload.u > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
      }
      out = static_cast<std::int64_t>(m_payload.u);
      return true;

    case ValueType::Double: {
      const double v = m_payload.d;
      // NaN は比較がすべて false になるのでここで弾かれる
      if (!(v >= kInt64LowerBound && v < kInt64UpperBound)) {
        return false;
      }
      const auto truncated = static_cast<std::int64_t>(v);
      if (static_cast<double>(truncated) != v) {
        return false;   // 小数部があるのでサイレントに切り詰めない
      }
      out = truncated;
      return true;
    }

    default:
      return false;
    }
  }

  bool Value::TryGetUInt64(std::uint64_t& out) const noexcept {
    switch (m_type) {
    case ValueType::Int64:
      if (m_payload.i < 0) {
        return false;
      }
      out = static_cast<std::uint64_t>(m_payload.i);
      return true;

    case ValueType::UInt64:
      out = m_payload.u;
      return true;

    case ValueType::Double: {
      const double v = m_payload.d;
      if (!(v >= 0.0 && v < kUInt64UpperBound)) {
        return false;
      }
      const auto truncated = static_cast<std::uint64_t>(v);
      if (static_cast<double>(truncated) != v) {
        return false;
      }
      out = truncated;
      return true;
    }

    default:
      return false;
    }
  }

  bool Value::TryGetInt(std::int32_t& out) const noexcept {
    std::int64_t wide = 0;
    if (!TryGetInt64(wide)) {
      return false;
    }
    if (wide < (std::numeric_limits<std::int32_t>::min)()
        || wide > (std::numeric_limits<std::int32_t>::max)()) {
      return false;
    }
    out = static_cast<std::int32_t>(wide);
    return true;
  }

  bool Value::TryGetUInt(std::uint32_t& out) const noexcept {
    std::uint64_t wide = 0;
    if (!TryGetUInt64(wide)) {
      return false;
    }
    if (wide > (std::numeric_limits<std::uint32_t>::max)()) {
      return false;
    }
    out = static_cast<std::uint32_t>(wide);
    return true;
  }

  bool Value::TryGetDouble(double& out) const noexcept {
    switch (m_type) {
    case ValueType::Double:
      out = m_payload.d;
      return true;

    case ValueType::Int64: {
      // 往復一致する範囲だけを通す。64bit のエンティティ ID が
      // 黙って桁落ちするのを防ぐのが目的
      const std::int64_t v = m_payload.i;
      if (v > kExactIntLimit || v < -kExactIntLimit) {
        return false;
      }
      out = static_cast<double>(v);
      return true;
    }

    case ValueType::UInt64: {
      const std::uint64_t v = m_payload.u;
      if (v > kExactUIntLimit) {
        return false;
      }
      out = static_cast<double>(v);
      return true;
    }

    default:
      return false;
    }
  }

  bool Value::ToDoubleLossy(double& out) const noexcept {
    switch (m_type) {
    case ValueType::Double: out = m_payload.d;                        return true;
    case ValueType::Int64:  out = static_cast<double>(m_payload.i);   return true;
    case ValueType::UInt64: out = static_cast<double>(m_payload.u);   return true;
    default:                                                          return false;
    }
  }

  bool Value::TryGetFloat(float& out) const noexcept {
    double wide = 0.0;
    if (!ToDoubleLossy(wide)) {
      return false;
    }

    // NaN / Inf はそのまま通す(値としての意味を保つ)
    if (IsNaN(wide)) {
      out = std::numeric_limits<float>::quiet_NaN();
      return true;
    }
    const double infinity = std::numeric_limits<double>::infinity();
    if (wide == infinity)  { out =  std::numeric_limits<float>::infinity(); return true; }
    if (wide == -infinity) { out = -std::numeric_limits<float>::infinity(); return true; }

    // float の表現域を超える有限値は、inf へ化けてしまうので失敗させる。
    // 一方で**丸めは許容する** — float は本質的に近似表現であり、
    // 手書き config の 0.1 を拒否すると float パラメータが読めなくなる
    const double floatMax = static_cast<double>((std::numeric_limits<float>::max)());
    if (wide > floatMax || wide < -floatMax) {
      return false;
    }

    out = static_cast<float>(wide);
    return true;
  }

  // ---------------------------------------------------------------------------
  // 高速系アクセサ
  // ---------------------------------------------------------------------------

  std::int32_t Value::GetInt() const noexcept {
    std::int32_t value = 0;
    const bool   ok    = TryGetInt(value);
    assert(ok && "Value::GetInt: type mismatch or out of range");
    (void)ok;
    return value;
  }

  std::uint32_t Value::GetUInt() const noexcept {
    std::uint32_t value = 0;
    const bool    ok    = TryGetUInt(value);
    assert(ok && "Value::GetUInt: type mismatch or out of range");
    (void)ok;
    return value;
  }

  float Value::GetFloat() const noexcept {
    float      value = 0.0f;
    const bool ok    = TryGetFloat(value);
    assert(ok && "Value::GetFloat: type mismatch or out of range");
    (void)ok;
    return value;
  }

  // ---------------------------------------------------------------------------
  // 構築
  // ---------------------------------------------------------------------------

  bool Value::SetString(StringView v, JsonArena& arena) noexcept {
    // 長さは uint32_t で保持するため、4 GiB 以上の文字列は扱えない
    if (v.Size() > (std::numeric_limits<std::uint32_t>::max)()) {
      return false;
    }

    if (v.Empty()) {
      Assign(ValueType::String);
      m_payload.str = nullptr;
      return true;
    }

    char* const buffer = arena.AllocateArray<char>(v.Size());
    if (buffer == nullptr) {
      return false;
    }
    std::memcpy(buffer, v.Data(), v.Size());

    Assign(ValueType::String);
    m_payload.str = buffer;
    m_size        = static_cast<std::uint32_t>(v.Size());
    return true;
  }

  bool Value::SetStringRef(StringView v) noexcept {
    if (v.Size() > (std::numeric_limits<std::uint32_t>::max)()) {
      return false;
    }
    Assign(ValueType::String);
    m_payload.str = v.Data();
    m_size        = static_cast<std::uint32_t>(v.Size());
    return true;
  }

  bool Value::ReserveArray(std::uint32_t capacity, JsonArena& arena) noexcept {
    if (capacity <= m_capacity) {
      return true;
    }
    Value* const buffer = arena.AllocateArray<Value>(capacity);
    if (buffer == nullptr) {
      return false;
    }
    if (m_size != 0) {
      // R2-5: Value は trivially copyable なので memcpy で移送してよい。
      // 旧領域はアリーナ上に放棄される(個別解放できないため)
      std::memcpy(buffer, m_payload.items, static_cast<size_t>(m_size) * sizeof(Value));
    }
    m_payload.items = buffer;
    m_capacity      = capacity;
    return true;
  }

  bool Value::ReserveObject(std::uint32_t capacity, JsonArena& arena) noexcept {
    if (capacity <= m_capacity) {
      return true;
    }
    Member* const buffer = arena.AllocateArray<Member>(capacity);
    if (buffer == nullptr) {
      return false;
    }
    if (m_size != 0) {
      std::memcpy(buffer, m_payload.members, static_cast<size_t>(m_size) * sizeof(Member));
    }
    m_payload.members = buffer;
    m_capacity        = capacity;
    return true;
  }

  bool Value::SetArray(std::uint32_t capacity, JsonArena& arena) noexcept {
    Assign(ValueType::Array);
    m_payload.items = nullptr;
    return (capacity == 0) ? true : ReserveArray(capacity, arena);
  }

  bool Value::SetObject(std::uint32_t capacity, JsonArena& arena) noexcept {
    Assign(ValueType::Object);
    m_payload.members = nullptr;
    return (capacity == 0) ? true : ReserveObject(capacity, arena);
  }

  namespace {

    /// 容量を 2 倍にする。uint32_t を溢れるなら 0(= 失敗)を返す
    [[nodiscard]] std::uint32_t GrowCapacity(std::uint32_t current) noexcept {
      if (current == 0) {
        return Value::kMinCapacity;
      }
      if (current > ((std::numeric_limits<std::uint32_t>::max)() / 2u)) {
        return 0;
      }
      return current * 2u;
    }

  }

  Value* Value::PushBack(JsonArena& arena) noexcept {
    assert(IsArray() && "Value::PushBack: not an array");
    if (!IsArray()) {
      return nullptr;
    }
    if (m_size == m_capacity) {
      const std::uint32_t grown = GrowCapacity(m_capacity);
      if (grown == 0 || !ReserveArray(grown, arena)) {
        return nullptr;
      }
    }
    Value* const slot = m_payload.items + m_size;
    *slot = Value{};
    ++m_size;
    return slot;
  }

  Value* Value::AddMemberRef(StringView key, JsonArena& arena) noexcept {
    assert(IsObject() && "Value::AddMemberRef: not an object");
    if (!IsObject()) {
      return nullptr;
    }
    if (m_size == m_capacity) {
      const std::uint32_t grown = GrowCapacity(m_capacity);
      if (grown == 0 || !ReserveObject(grown, arena)) {
        return nullptr;
      }
    }
    Member* const slot = m_payload.members + m_size;
    slot->key   = key;
    slot->value = Value{};
    ++m_size;
    return &slot->value;
  }

  bool Value::AssignArray(const Value* items, std::uint32_t count, JsonArena& arena) noexcept {
    assert((items != nullptr || count == 0) && "Value::AssignArray: null items with count > 0");

    if (count == 0) {
      Assign(ValueType::Array);
      m_payload.items = nullptr;
      return true;
    }
    if (items == nullptr) {
      return false;
    }

    Value* const buffer = arena.AllocateArray<Value>(count);
    if (buffer == nullptr) {
      return false;   // 失敗時は *this を変更しない
    }
    // R2-5: Value は trivially copyable なので memcpy でよい
    std::memcpy(buffer, items, static_cast<size_t>(count) * sizeof(Value));

    Assign(ValueType::Array);
    m_payload.items = buffer;
    m_size          = count;
    m_capacity      = count;
    return true;
  }

  bool Value::AssignObject(const Member* members, std::uint32_t count, JsonArena& arena) noexcept {
    assert((members != nullptr || count == 0) && "Value::AssignObject: null members with count > 0");

    if (count == 0) {
      Assign(ValueType::Object);
      m_payload.members = nullptr;
      return true;
    }
    if (members == nullptr) {
      return false;
    }

    Member* const buffer = arena.AllocateArray<Member>(count);
    if (buffer == nullptr) {
      return false;
    }
    std::memcpy(buffer, members, static_cast<size_t>(count) * sizeof(Member));

    Assign(ValueType::Object);
    m_payload.members = buffer;
    m_size            = count;
    m_capacity        = count;
    return true;
  }

  Value* Value::AddMember(StringView key, JsonArena& arena) noexcept {
    assert(IsObject() && "Value::AddMember: not an object");
    if (!IsObject()) {
      return nullptr;
    }

    // キーを先にアリーナへ複製する。伸長より前に行うことで、
    // 複製に失敗したときにメンバ数を進めてしまう事故を避ける
    StringView owned;
    if (!key.Empty()) {
      if (key.Size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return nullptr;
      }
      char* const buffer = arena.AllocateArray<char>(key.Size());
      if (buffer == nullptr) {
        return nullptr;
      }
      std::memcpy(buffer, key.Data(), key.Size());
      owned = StringView(buffer, key.Size());
    }

    return AddMemberRef(owned, arena);
  }

}
