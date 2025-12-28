#pragma once
#include <string_view>
#include <cstdint>

namespace GLFD::TypeInfo {
  using TypeID = size_t;

  // FNV-1a Hash (コンパイル時計算可能)
  consteval size_t Hash(std::string_view str) {
    size_t hash = 14695981039346656037ull;
    for (char c : str) {
      hash ^= static_cast<size_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  // 型名を取得する魔法の関数
  // コンパイラごとにマクロが異なるが、主要な環境をカバー
  template <typename T>
  consteval std::string_view GetTypeName() {
#if defined(_MSC_VER)
    return __FUNCSIG__;
#elif defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#else
#error "Unsupported compiler"
#endif
  }

  // 型 T の一意なIDをコンパイル時に計算して返す
  template <typename T>
  consteval TypeID GetID() { return Hash(GetTypeName<T>()); }
} // namespace GLFD::TypeInfo
