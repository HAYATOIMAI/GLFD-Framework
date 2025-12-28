#pragma once
#include <cstddef>
#include <memory>
#include <utility>
#include <stdexcept>
#include <type_traits>
#include <algorithm> 
#include <cassert>
#include "MemoryResource.h"

namespace GLFD {
  /**
   * @brief    可変長配列クラス
   * @tparam T 格納する要素の型
   */
  template <typename T>
  class DynamicArray {
  public:
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;

    /**
     * @brief コンストラクタ
     * @param resource 使用するメモリリソース (nullptrの場合はデフォルト/ヒープ)
     */
    explicit DynamicArray(Memory::IMemoryResource* resource = nullptr) noexcept
      : m_resource(resource ? resource : Memory::GetDefaultResource()) {
    }
    /**
     * @brief コピーコンストラクタ
     */
    DynamicArray(const DynamicArray& other)
      : m_resource(other.m_resource) {

      if (other.IsEmpty()) return;

      AllocateMemory(other.m_capacity);

      try {
        std::uninitialized_copy_n(other.m_data, other.m_size, m_data);
        m_size = other.m_size;
      }
      catch (...) {
        DeallocateMemory();
        throw;
      }
    }
    /**
     * @brief ムーブコンストラクタ
     */
    DynamicArray(DynamicArray&& other) noexcept :
      m_data(other.m_data),
      m_size(other.m_size),
      m_capacity(other.m_capacity),
      m_resource(other.m_resource) {

      other.m_data = nullptr;
      other.m_size = 0;
      other.m_capacity = 0;
    }
    // 特定のリソースを指定してコピーを作成したい場合用
    DynamicArray(const DynamicArray& other, Memory::IMemoryResource* resource)
      : m_resource(resource ? resource : Memory::GetDefaultResource()) {
      if (other.IsEmpty()) return;

      AllocateMemory(other.m_capacity);

      try {
        std::uninitialized_copy_n(other.m_data, other.m_size, m_data);
        m_size = other.m_size;
      }
      catch (...) {
        DeallocateMemory();
        throw;
      }
    }
    /**
     * @brief デストラクタ
     */
    ~DynamicArray() {
      Clear();
      DeallocateMemory();
    }
    /**
     * @brief コピー代入演算子
     */
    DynamicArray& operator=(const DynamicArray& other) {
      if (this != &other) {
        DynamicArray temp(other);
        Swap(temp);
      }
      return *this;
    }
    /**
     * @brief ムーブ代入演算子
     */
    DynamicArray& operator=(DynamicArray&& other) noexcept {
      if (this != &other) {
        // 既存のメモリを解放する際も、必ず関数を経由させる
        Clear();
        DeallocateMemory();

        // ... (ポインタの付け替え) ...
        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        m_resource = other.m_resource;

        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
      }
      return *this;
    }

    // 要素アクセス
    reference operator[](size_type index) {
      assert(index < m_size && "DynamicArray index out of range");
      return m_data[index];
    }
    const_reference operator[](size_type index) const {
      assert(index < m_size && "DynamicArray index out of range");
      return m_data[index];
    }

    reference At(size_type index) {
      if (index >= m_size) throw std::out_of_range("Index out of range");
      return m_data[index];
    }
    const_reference At(size_type index) const {
      if (index >= m_size) throw std::out_of_range("Index out of range");
      return m_data[index];
    }

    /** @brief 先頭要素への参照を返す。 */
    reference Front() { assert(m_size > 0); return m_data[0]; }
    /** @brief 先頭要素へのconst参照を返す。 */
    const_reference Front() const { assert(m_size > 0); return m_data[0]; }

    /** @brief 末尾要素への参照を返す */
    reference Back() { assert(m_size > 0); return m_data[m_size - 1]; }
    /** @brief 末尾要素へのconst参照を返す */
    const_reference Back() const { assert(m_size > 0); return m_data[m_size - 1]; }

    // 容量管理
    template<typename... Args>
    reference EmplaceBack(Args&&... args) {
      if (m_size >= m_capacity) {
        Grow();
      }
      std::construct_at(&m_data[m_size], std::forward<Args>(args)...);
      return m_data[m_size++];
    }

    void PushBack(const T& value) { EmplaceBack(value); }
    void PushBack(T&& value) { EmplaceBack(std::move(value)); }

    void PopBack() noexcept {
      assert(m_size > 0 && "PopBack called on empty array");
      m_size--;
      if constexpr (!std::is_trivially_destructible_v<T>) {
        std::destroy_at(&m_data[m_size]);
      }
    }

    /**
     * @brief 要素数を変更
     * @param newSize 新しい要素数
     */
    void Resize(size_type newSize) {
      if (newSize < m_size) {
        // 縮小: 不要な分を破棄
        if constexpr (!std::is_trivially_destructible_v<T>) {
          std::destroy(m_data + newSize, m_data + m_size);
        }
      }
      else if (newSize > m_size) {
        // 拡大: 新規分を構築
        Reserve(newSize);
        std::uninitialized_default_construct(m_data + m_size, m_data + newSize);
      }
      m_size = newSize;
    }

    /**
     * @brief 指定インデックスの要素を削除
     * @details 順序を維持したい場合に使用。
     */
    void Erase(size_type index) {
      assert(index < m_size);
      // 削除位置より後ろの要素を1つ前に移動
      std::move(m_data + index + 1, m_data + m_size, m_data + index);
      PopBack(); // 末尾を破棄
    }

    /**
     * @brief 指定インデックスの要素を削除します（順序維持なし）。
     * @details 削除対象と末尾要素を入れ替えてからPopBackします。
     */
    void EraseSwap(size_type index) {
      assert(index < m_size);
      if (index != m_size - 1) {
        // 末尾要素を削除位置へムーブ
        m_data[index] = std::move(m_data[m_size - 1]);
      }
      PopBack();
    }

    /**
     * @brief 要素数を変更し、新しい要素を指定した値で初期化
     * @param newSize 新しい要素数
     * @param value 初期化する値
     */
    void Resize(size_type newSize, const_reference value) {
      if (newSize < m_size) {
        std::destroy(m_data + newSize, m_data + m_size);
      }
      else if (newSize > m_size) {
        Reserve(newSize);
        std::uninitialized_default_construct(m_data + m_size, m_data + newSize);
      }
      m_size = newSize;
    }

    void Clear() noexcept {
      if constexpr (!std::is_trivially_destructible_v<T>) {
        std::destroy_n(m_data, m_size);
      }
      m_size = 0;
    }

    void Reserve(size_type newCapacity) {
      if (newCapacity > m_capacity) {
        Reallocate(newCapacity);
      }
    }

    /**
     * @brief 余分なメモリを解放し、容量を要素数に一致させます。
     */
    void ShrinkToFit() {
      if (m_size < m_capacity) {
        if (m_size == 0) {
          DeallocateMemory(); // 容量0なら完全解放
          m_capacity = 0;
          m_data = nullptr;
        }
        else {
          Reallocate(m_size);
        }
      }
    }

    /**
     * @brief 2つの DynamicArray の内容を交換します。
     */
    void Swap(DynamicArray& other) noexcept {
      // メンバをまとめて交換
      std::swap(m_data, other.m_data);
      std::swap(m_size, other.m_size);
      std::swap(m_capacity, other.m_capacity);
    }

    //================================================================================
    // 情報取得
    //================================================================================

    [[nodiscard]] size_type GetSize() const noexcept { return m_size; }
    [[nodiscard]] size_type GetCapacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }
    [[nodiscard]] T* GetData() noexcept { return m_data; }
    [[nodiscard]] const T* GetData() const noexcept { return m_data; }

    //================================================================================
    // イテレータ
    //================================================================================
    iterator begin() noexcept { return m_data; }
    iterator end() noexcept { return m_data + m_size; }
    const_iterator begin() const noexcept { return m_data; }
    const_iterator end() const noexcept { return m_data + m_size; }
    const_iterator cbegin() const noexcept { return m_data; }
    const_iterator cend() const noexcept { return m_data + m_size; }

  private:
    T* m_data = nullptr;
    size_type m_size = 0;
    size_type m_capacity = 0;

    Memory::IMemoryResource* m_resource;

    // 容量拡張
    void Grow() {
      size_type newCap = (m_capacity > 0) ? (m_capacity + m_capacity / 2) : 8;
      Reallocate(newCap);
    }

    /**
     * @brief 指定された容量でメモリを再確保し、要素を移動
     * @param newCapacity 新しい容量。0の場合はメモリを解放します。
     */
    void Reallocate(size_type newCapacity) {
      // 新しい領域をリソースから確保
      T* newData = static_cast<T*>(m_resource->Allocate(newCapacity * sizeof(T), alignof(T)));

      // 移動またはコピー
      if constexpr (std::is_nothrow_move_constructible_v<T>) {
        std::uninitialized_move_n(m_data, m_size, newData);
      }
      else {
        try {
          std::uninitialized_copy_n(m_data, m_size, newData);
        }
        catch (...) {
          m_resource->Deallocate(newData, newCapacity * sizeof(T), alignof(T));
          throw;
        }
      }

      // 古い要素の破棄
      std::destroy_n(m_data, m_size);

      // 古いメモリの解放 (リソース経由)
      if (m_data) {
        m_resource->Deallocate(m_data, m_capacity * sizeof(T), alignof(T));
      }

      // 新しいメモリと容量でメンバ変数を更新
      m_data = newData;
      m_capacity = newCapacity;
    }

    // 内部ヘルパー: メモリ確保
    void AllocateMemory(size_type cap) {
      // m_resource 経由で確保
      m_data = static_cast<T*>(m_resource->Allocate(cap * sizeof(T), alignof(T)));
      m_capacity = cap;
    }

    // 内部ヘルパー: メモリ解放
    void DeallocateMemory() {
      if (m_data) {
        m_resource->Deallocate(m_data, m_capacity * sizeof(T), alignof(T));

        m_data = nullptr;
        m_capacity = 0;
      }
    }
  };

  /**
   * @brief DynamicArray のための非メンバ swap
   */
  template <typename T>
  void swap(DynamicArray<T>& lhs, DynamicArray<T>& rhs) noexcept { lhs.Swap(rhs); }

  /**
   * @brief 2つの DynamicArray が等しいか比較
   */
  template <typename T>
  bool operator==(const DynamicArray<T>& lhs, const DynamicArray<T>& rhs) {
    if (lhs.GetSize() != rhs.GetSize()) {
      return false;
    }
    // std::equal を使って全要素を効率的に比較
    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }

  /**
   * @brief 2つの DynamicArray が等しくないか比較
   */
  template <typename T>
  bool operator!=(const DynamicArray<T>& lhs, const DynamicArray<T>& rhs) {
    return !(lhs == rhs);
  }
} // namespace GLFD