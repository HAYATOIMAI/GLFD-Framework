#pragma once
#include <cstddef>
#include <memory>
#include <utility>
#include <stdexcept>
#include <type_traits>
#include <algorithm> 
#include <cassert>
#include "MemoryResource.h"
#include <iterator>

namespace GLFD {

  template <typename T>
  concept StorableElement =
    (std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>) &&
    std::is_destructible_v<T>;

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
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    /**
     * @brief 既定コンストラクタ。リソースは後から `SetResource` で設定する
     *
     * @details
     *  入れ子のコンテナ (`DynamicArray<DynamicArray<T>>`) は、外側の
     *  `TryResize` が内側を**既定構築**するため、この状態を必ず通る。
     *  したがってここで assert すると入れ子が成立しない。
     *
     *  リソース未設定のまま確保を要求しても黙って進むことはない。
     *  非投擲版 (`Try*`) は `false` を返し、投擲版は
     *  `runtime_error("No Memory Resource Set")` を投げる。
     */
    DynamicArray() noexcept : m_resource(nullptr) {}

    /**
     * @brief リソースを指定するコンストラクタ
     * @param resource 使用するメモリリソース
     * @note  **明示的に nullptr を渡すのは誤り**なので assert で捕まえる。
     *        リソースを後から決めたい場合は、引数無しで構築して
     *        `SetResource` を呼ぶこと
     */
    explicit DynamicArray(Memory::IMemoryResource* resource) noexcept
      : m_resource(resource) {
      assert(m_resource && "IMemoryResource must not be nullptr");
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

    // カスタムアロケータを指定するコピーコンストラクタ（代入演算子での一時オブジェクト用）
    DynamicArray(const DynamicArray& other, Memory::IMemoryResource* resource)
      : m_resource(resource) {
      assert(m_resource && "IMemoryResource must not be nullptr");
      if (other.m_size > 0) {
        AllocateMemory(other.m_size);
        std::uninitialized_copy_n(other.m_data, other.m_size, m_data);
        m_size = other.m_size;
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
        DynamicArray temp(other, this->m_resource);
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

    void PushBack(const T& value) { 
      if (m_size == m_capacity) Grow();
      std::construct_at(&m_data[m_size], value);
      m_size++;
    }

    void PushBack(T&& value) { 
      if (m_size == m_capacity) Grow();
      std::construct_at(&m_data[m_size], std::move(value));
      m_size++;
    }

    void PopBack() noexcept {
      assert(m_size > 0 && "PopBack called on empty array");
      m_size--;
      if constexpr (!std::is_trivially_destructible_v<T>) {
        std::destroy_at(&m_data[m_size]);
      }
    }

    /**
     * @brief 指定インデックスの要素を削除
     * @details 順序を維持したい場合に使用
     */
    void Erase(size_type index) {
      assert(index < m_size);
      // 削除位置より後ろの要素を1つ前に移動
      std::move(m_data + index + 1, m_data + m_size, m_data + index);
      PopBack(); // 末尾を破棄
    }

    /**
     * @brief 指定インデックスの要素を削除します（順序維持なし）
     * @details 削除対象と末尾要素を入れ替えてからPopBack
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
        if constexpr (!std::is_trivially_destructible_v<T>) {
          std::destroy(m_data + newSize, m_data + m_size);
        }
      }
      else if (newSize > m_size) {
        Reserve(newSize);
        size_type constructed = m_size;
        try {
          for (; constructed < newSize; ++constructed) {
            std::construct_at(&m_data[constructed], value);
          }
        }
        catch (...) {
          std::destroy(m_data + m_size, m_data + constructed);
          throw;
        }
      }
      m_size = newSize;
    }

    /**
     * @brief 要素数を変更
     * @param newSize 新しい要素数
     */
    void Resize(size_type newSize) {
      if (newSize < m_size) {
        // 縮小 不要な分を破棄
        if constexpr (!std::is_trivially_destructible_v<T>) {
          std::destroy(m_data + newSize, m_data + m_size);
        }
      }
      else if (newSize > m_size) {
        // 拡大 新規分を構築
        Reserve(newSize);
        size_type constructed = m_size;
        try {
          for (; constructed < newSize; ++constructed) {
            std::construct_at(&m_data[constructed]);
          }
        }
        catch (...) {
          std::destroy(m_data + m_size, m_data + constructed);
          throw;
        }
      }
      m_size = newSize;
    }

    template <typename... Args>
      requires std::constructible_from<T, Args...>
    reference EmplaceBack(Args&&... args) {
      if (m_size == m_capacity) Grow();
      std::construct_at(&m_data[m_size], std::forward<Args>(args)...);
      return m_data[m_size++];
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

    // -------------------------------------------------------------------------
    //  非投擲 API (R3-13 / フェーズ 1-5a で追加)
    //
    //  アーカイブ層は例外を使えないため、確保失敗を戻り値で受け取る経路が要る。
    //  投擲版はこれらを呼んで失敗時に throw する形にしてあり、実装は重複しない。
    //
    //  【保証の内容】
    //
    //  (1) 確保失敗 (戻り値 false) のとき: **状態は完全に不変**。
    //      要素数・容量・GetData() のポインタ値まで確保前と一致する。
    //      確保に失敗した時点で戻るため、要素の移動も構築も始まっていない。
    //
    //  (2) 要素型由来の例外が伝播したとき: **強い保証**。
    //      これは kNothrowRelocate == false の T でのみ起こり得る。
    //      要素数と各要素の値は保たれ、コンテナは有効なまま使い続けられる。
    //      **ただし容量と GetData() のポインタは変わり得る**。
    //      再確保に成功した後で新要素の構築が失敗した場合、再確保そのものは
    //      巻き戻さないためである (std::vector::push_back と同じ強さ)。
    //
    //  R3-13 の「失敗時 false、状態は不変」は (1) を指す。全ての型・全ての失敗で
    //  ポインタ値まで不変になるわけではない点に注意すること。
    //  アーカイブ層が扱う型は全て kNothrowRelocate == true であり (2) は起こらない。
    //
    //  noexcept は T に依存する。要素の再配置は T が nothrow ムーブ構築可能なら
    //  決して throw しないが、コピー構築へ落ちる T では要素側の例外が伝播する。
    //  無条件 noexcept にすると、その伝播が terminate になり
    //  「失敗を戻り値で扱う」という Try* の目的が壊れるため条件付きにしている。
    //  その catch は if constexpr の破棄される分岐に置いてあるため、
    //  nothrow ムーブ構築可能な T では /EH 無しでも C4530 が出ない。
    // -------------------------------------------------------------------------

    /// 要素の再配置が throw し得ないか。Try* 群の noexcept 指定に使う
    static constexpr bool kNothrowRelocate = std::is_nothrow_move_constructible_v<T>;

    /**
     * @brief  確保失敗を戻り値で返す Reserve
     * @return 失敗時 false。コンテナの状態は不変
     */
    [[nodiscard]] bool TryReserve(size_type newCapacity) noexcept(kNothrowRelocate) {
      if (newCapacity <= m_capacity) {
        return true;
      }
      return TryReallocate(newCapacity);
    }

    /**
     * @brief  確保失敗を戻り値で返す PushBack
     * @return 失敗時 false。コンテナの状態は不変
     */
    [[nodiscard]] bool TryPushBack(const T& value)
      noexcept(kNothrowRelocate && std::is_nothrow_copy_constructible_v<T>) {
      if (m_size == m_capacity && !TryGrow()) {
        return false;
      }
      std::construct_at(&m_data[m_size], value);
      ++m_size;
      return true;
    }

    /// @copydoc TryPushBack(const T&)
    [[nodiscard]] bool TryPushBack(T&& value) noexcept(kNothrowRelocate) {
      if (m_size == m_capacity && !TryGrow()) {
        return false;
      }
      std::construct_at(&m_data[m_size], std::move(value));
      ++m_size;
      return true;
    }

    /**
     * @brief  確保失敗を戻り値で返す Resize
     * @return 失敗時 false。コンテナの状態は不変
     * @note   縮小は確保を伴わないため必ず成功する
     */
    [[nodiscard]] bool TryResize(size_type newSize)
      noexcept(kNothrowRelocate && std::is_nothrow_default_constructible_v<T>) {
      if (newSize < m_size) {
        if constexpr (!std::is_trivially_destructible_v<T>) {
          std::destroy(m_data + newSize, m_data + m_size);
        }
        m_size = newSize;
        return true;
      }
      if (newSize > m_size) {
        if (!TryReserve(newSize)) {
          return false;
        }
        if constexpr (std::is_nothrow_default_constructible_v<T>) {
          for (size_type i = m_size; i < newSize; ++i) {
            std::construct_at(&m_data[i]);
          }
        }
        else {
          size_type constructed = m_size;
          try {
            for (; constructed < newSize; ++constructed) {
              std::construct_at(&m_data[constructed]);
            }
          }
          catch (...) {
            std::destroy(m_data + m_size, m_data + constructed);
            throw;   // 要素型由来の失敗。確保失敗ではないので false にはしない
          }
        }
        m_size = newSize;
      }
      return true;
    }

    /// @copydoc TryResize(size_type)
    [[nodiscard]] bool TryResize(size_type newSize, const_reference value)
      noexcept(kNothrowRelocate && std::is_nothrow_copy_constructible_v<T>) {
      if (newSize < m_size) {
        if constexpr (!std::is_trivially_destructible_v<T>) {
          std::destroy(m_data + newSize, m_data + m_size);
        }
        m_size = newSize;
        return true;
      }
      if (newSize > m_size) {
        if (!TryReserve(newSize)) {
          return false;
        }
        if constexpr (std::is_nothrow_copy_constructible_v<T>) {
          for (size_type i = m_size; i < newSize; ++i) {
            std::construct_at(&m_data[i], value);
          }
        }
        else {
          size_type constructed = m_size;
          try {
            for (; constructed < newSize; ++constructed) {
              std::construct_at(&m_data[constructed], value);
            }
          }
          catch (...) {
            std::destroy(m_data + m_size, m_data + constructed);
            throw;
          }
        }
        m_size = newSize;
      }
      return true;
    }

    /**
     * @brief 余分なメモリを解放し、容量を要素数に一致させる
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
     * @brief 2つのDynamicArrayの内容を交換
     */
    void Swap(DynamicArray& other) noexcept {
      // メンバをまとめて交換
      std::swap(m_data, other.m_data);
      std::swap(m_size, other.m_size);
      std::swap(m_capacity, other.m_capacity);
    }

    // -------------------------------------------------------------------------
    //  メモリリソースの取得 / 差し替え (フェーズ 1-6 で追加)
    //
    //  入れ子のコンテナ (DynamicArray<DynamicArray<T>>) を成立させるために要る。
    //  TryResize / Resize は新しい要素を **既定構築** するため、内側の配列は
    //  コンストラクタの既定引数 nullptr を掴んでリソース未設定になる。
    //  外側のリソースを配る手段が無いと、内側は一度も確保できない。
    //
    //  IMemoryResource を軸に据えたエンジンで、コンテナが自分のリソースを
    //  外から知れないこと自体が欠落であり、JSON とは独立に価値がある。
    // -------------------------------------------------------------------------

    /// 使用中のメモリリソース(未設定なら nullptr)
    [[nodiscard]] Memory::IMemoryResource* GetResource() const noexcept { return m_resource; }

    /**
     * @brief メモリリソースを差し替える
     *
     * @details
     *  **要素を保持していないときだけ呼べる。** 構築済みの要素は現在のリソースから
     *  確保した領域の上にあるため、差し替えると解放先が食い違う。
     *
     *  条件を `m_size == 0` にしたのは、`m_capacity == 0` だと `Reserve` 済みで
     *  空の配列に呼べず、汎用 API として不自然になるため。**確保済みの容量が
     *  残っている場合は、現在のリソースで解放してから差し替える。**
     *  したがって予約済みの容量は失われる。
     *
     *  Release では assert が消えるので、要素がある場合は**何もせずに返る**。
     *  黙って差し替えて別のリソースへ解放を投げるより安全なため。
     */
    void SetResource(Memory::IMemoryResource* resource) noexcept {
      assert(m_size == 0 && "SetResource: the array must not hold any elements");
      if (m_size != 0) {
        return;
      }
      DeallocateMemory();   // 旧リソースで解放してから差し替える
      m_resource = resource;
    }

    [[nodiscard]] size_type GetSize() const noexcept { return m_size; }
    [[nodiscard]] size_type GetCapacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }
    [[nodiscard]] T* GetData() noexcept { return m_data; }
    [[nodiscard]] const T* GetData() const noexcept { return m_data; }

    /** イテレータ */
    iterator begin() noexcept { return m_data; }
    iterator end() noexcept { return m_data + m_size; }
    const_iterator begin() const noexcept { return m_data; }
    const_iterator end() const noexcept { return m_data + m_size; }
    const_iterator cbegin() const noexcept { return m_data; }
    const_iterator cend() const noexcept { return m_data + m_size; }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(begin()); }

  private:
    T* m_data = nullptr;
    size_type m_size = 0;
    size_type m_capacity = 0;

    Memory::IMemoryResource* m_resource;

    // 容量拡張
    void Grow() {
      // 容量上限と確保失敗の両方が TryGrow() の false に集約される。
      // どちらも「これ以上要素を追加できない」ことを意味するため bad_alloc とする
      if (!TryGrow()) {
        throw std::bad_alloc();
      }
    }

    /**
     * @brief  確保失敗を戻り値で返す Grow
     * @return 失敗時 false。コンテナの状態は不変
     */
    [[nodiscard]] bool TryGrow() noexcept(kNothrowRelocate) {
      constexpr size_type max_size = static_cast<size_type>(-1) / sizeof(T);
      if (m_capacity == max_size) {
        return false;
      }

      size_type newCap = (m_capacity > 0) ? (m_capacity + m_capacity / 2) : 8;

      // オーバーフローしたら最大値に丸める
      if (newCap < m_capacity || newCap > max_size) {
        newCap = max_size;
      }

      return TryReallocate(newCap);
    }

    /**
     * @brief 指定された容量でメモリを再確保し、要素を移動
     * @param newCapacity 新しい容量。0の場合はメモリを解放します。
     */
    void Reallocate(size_type newCapacity) {
      if (!m_resource) {
        throw std::runtime_error("No Memory Resource Set");
      }
      // 従来はここで m_resource->Allocate の戻り値を検査しておらず、
      // 確保失敗時は nullptr のまま未初期化領域へ書き込んでヌル参照で落ちていた。
      // TryReallocate へ委譲することで、未定義動作を定義された停止に置き換える
      if (!TryReallocate(newCapacity)) {
        throw std::bad_alloc();
      }
    }

    /**
     * @brief  確保失敗を戻り値で返す Reallocate
     * @param  newCapacity 新しい容量
     * @return 失敗時 false。コンテナの状態は不変
     */
    [[nodiscard]] bool TryReallocate(size_type newCapacity) noexcept(kNothrowRelocate) {
      constexpr size_type max_size = static_cast<size_type>(-1) / sizeof(T);

      if (!m_resource) {
        return false;
      }
      // newCapacity * sizeof(T) の桁溢れを防ぐ
      if (newCapacity > max_size) {
        return false;
      }

      // IMemoryResource は確保失敗時に nullptr を返す (throw しない)
      T* newData = static_cast<T*>(m_resource->Allocate(newCapacity * sizeof(T), alignof(T)));
      if (!newData) {
        return false;
      }

      // 移動またはコピー
      if constexpr (std::is_nothrow_move_constructible_v<T>) {
        std::uninitialized_move_n(m_data, m_size, newData);
      }
      else if constexpr (std::is_copy_constructible_v<T>) {
        try {
          std::uninitialized_copy_n(m_data, m_size, newData);
        }
        catch (...) {
          m_resource->Deallocate(newData, newCapacity * sizeof(T), alignof(T));
          throw;   // 要素型由来の失敗。確保失敗ではないので false にはしない
        }
      }
      else {
        // ムーブで例外を投げる可能性があり、かつコピーもできない型に対する厳格な拒否
        static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>,
          "T must be nothrow-move-constructible or copy-constructible. Please add 'noexcept' to T's move constructor.");
      }

      // 古い要素の破棄
      std::destroy_n(m_data, m_size);

      // 古いメモリの解放 (リソース経由)
      if (m_data) {
        m_resource->Deallocate(m_data, m_capacity * sizeof(T), alignof(T));
      }

      m_data = newData;
      m_capacity = newCapacity;
      return true;
    }

    // 内部ヘルパー: メモリ確保
    void AllocateMemory(size_type cap) {
      assert(m_resource && "IMemoryResource is nullptr");
      void* raw = m_resource->Allocate(cap * sizeof(T), alignof(T));
      if (!raw) throw std::bad_alloc();

      m_data = static_cast<T*>(raw);
      m_capacity = cap;

      assert(reinterpret_cast<std::uintptr_t>(m_data) % alignof(T) == 0 &&
        "IMemoryResource returned misaligned pointer");
    }

    // 内部ヘルパー: メモリ解放
    void DeallocateMemory() {
      if (m_data) {
        assert(m_resource && "IMemoryResource is nullptr");
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
} // namespace GLFD