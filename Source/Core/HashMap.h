#pragma once
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>
#include <utility>
#include <stdexcept>
#include <optional>
#include <array>
#include <algorithm>
#include <iterator>
#include <cassert>

namespace GLFD {
  // --- ヘルパー構造体 ---
  // 透過的キー検索用ハッシャー
  struct StringViewHasher {
    using is_transparent = void;
    std::size_t operator()(const char* str) const { return std::hash<std::string_view>{}(str); }
    std::size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
    std::size_t operator()(const std::string& s) const { return std::hash<std::string>{}(s); }
  };

  template <
    typename Key,
    typename Value,
    size_t SmallSize = 8,
    typename Hasher = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>,
    typename Allocator = std::allocator<std::pair<const Key, Value>>
  >
  class HashMap {
  private:
    struct Node;

  public:
    // --- 型定義 ---
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<const Key, Value>;
    using size_type = size_t;
    using hasher = Hasher;
    using key_equal = KeyEqual;
    using allocator_type = Allocator;

  private:
    struct Node {
      value_type m_pair;
      Node* m_next;
      bool m_isSmall = false;  // Bug #1 fix: デフォルト初期化

      template<typename... Args>
      Node(Args&&... args) : m_pair(std::forward<Args>(args)...), m_next(nullptr) {}
    };

    using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    using AllocatorTraits = std::allocator_traits<NodeAllocator>;

    // SBO用ストレージ
    struct SmallStorage {
      std::array<Node*, SmallSize> m_buckets;
      std::array<std::aligned_storage_t<sizeof(Node), alignof(Node)>, SmallSize> m_nodes;
      Node* m_sboFreeList = nullptr; // SBO内部の空きノードリスト
    };

    // --- メンバ変数 ---
    Node** m_buckets;
    size_type m_bucketCount;
    size_type m_size;
    float m_maxLoadFactor;

    // 削除されたヒープノードを再利用するためのリスト（新規割り当てコスト削減）
    Node* m_heapRecycledList = nullptr;

    [[no_unique_address]] std::optional<SmallStorage> m_storage;
    [[no_unique_address]] Hasher m_hasher;
    [[no_unique_address]] KeyEqual m_keyEqual;
    [[no_unique_address]] NodeAllocator m_allocator;

  public:
    // --- イテレータ (前方イテレータ) ---
    template<bool IsConst>
    class ForwardIterator {
    public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = HashMap::value_type;
      using difference_type = std::ptrdiff_t;
      using pointer = std::conditional_t<IsConst, const value_type*, value_type*>;
      using reference = std::conditional_t<IsConst, const value_type&, value_type&>;

      ForwardIterator() : m_map(nullptr), m_node(nullptr), m_bucketIndex(0) {}

      reference operator*() const { return m_node->m_pair; }
      pointer operator->() const { return &m_node->m_pair; }

      ForwardIterator& operator++() {
        m_node = m_node->m_next;
        while (!m_node && ++m_bucketIndex < m_map->m_bucketCount) {
          m_node = m_map->m_buckets[m_bucketIndex];
        }
        return *this;
      }

      ForwardIterator operator++(int) { ForwardIterator temp = *this; ++(*this); return temp; }
      friend bool operator==(const ForwardIterator& a, const ForwardIterator& b) { return a.m_node == b.m_node; }
      friend bool operator!=(const ForwardIterator& a, const ForwardIterator& b) { return a.m_node != b.m_node; }

    private:
      friend class HashMap;
      using MapPtr = std::conditional_t<IsConst, const HashMap*, HashMap*>;
      MapPtr m_map;
      Node*  m_node;
      size_t m_bucketIndex;
      ForwardIterator(MapPtr map, Node* node, size_t bucketIndex) : m_map(map), m_node(node), m_bucketIndex(bucketIndex) {}
    };

    using iterator = ForwardIterator<false>;
    using const_iterator = ForwardIterator<true>;

    // --- コンストラクタ / デストラクタ ---
    explicit HashMap(const Allocator& alloc = Allocator())
      : m_buckets(nullptr), m_bucketCount(0), m_size(0), m_maxLoadFactor(0.75f), m_heapRecycledList(nullptr), m_allocator(alloc) {
      if constexpr (SmallSize > 0) {
        m_storage.emplace();
        ResetToSBO();
      }
      else {
        m_bucketCount = 16;
        m_buckets = new Node * [m_bucketCount]();
      }
    }

    ~HashMap() {
      Clear();
      if (!IsUsingSmallStorage()) {
        delete[] m_buckets;
      }
    }
    // コピーコンストラクタ
    HashMap(const HashMap& other)
      : m_bucketCount(0), m_size(0), m_maxLoadFactor(other.m_maxLoadFactor),
      m_allocator(AllocatorTraits::select_on_container_copy_construction(other.m_allocator)),
      m_hasher(other.m_hasher), m_keyEqual(other.m_keyEqual)
    {
      // 相手がSBOを使っているか、空であればSBOで開始
      if (SmallSize > 0 && (other.IsUsingSmallStorage() || other.Empty())) {
        m_storage.emplace();
        ResetToSBO();
      }
      else {
        m_bucketCount = other.m_bucketCount;
        m_buckets = new Node * [m_bucketCount]();
      }

      try {
        for (const auto& pair : other) {
          try_emplace(pair.first, pair.second);
        }
      }
      catch (...) {
        Clear();
        if (!IsUsingSmallStorage()) delete[] m_buckets;
        throw;
      }
    }

    // ムーブコンストラクタ
    HashMap(HashMap&& other) noexcept
      : m_buckets(nullptr), m_bucketCount(0), m_size(0), m_allocator(std::move(other.m_allocator))
    {
      MoveFrom(std::move(other));
    }

    // ムーブ代入
    HashMap& operator=(HashMap&& other) noexcept {
      if (this != &other) {
        Clear();
        // ヒープ配列の解放が必要なら行う
        if (!IsUsingSmallStorage()) { delete[] m_buckets; m_buckets = nullptr; }

        MoveFrom(std::move(other));
      }
      return *this;
    }

    HashMap& operator=(const HashMap& other)
    {
      if (this != &other) {
        HashMap temp(other);
        Swap(temp);
      }
      return *this;
    }

    // --- 主要API ---
    // 挿入 (Try Emplace)
    template<typename K, typename... Args>
    std::pair<iterator, bool> try_emplace(const K& key, Args&&... args)
    {
      // 容量チェック & リハッシュ
      if (NeedsRehash())
      {
        Rehash(std::max<size_t>(16, m_bucketCount * 2));
      }

      size_t index = GetIndex(key);
      if (Node* found = FindNode(key, index))
      {
        return { iterator(this, found, index), false };
      }

      // ノードの確保（再利用リスト優先）
      Node* newNode = AllocateNode(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple(std::forward<Args>(args)...));

      newNode->m_next = m_buckets[index];
      m_buckets[index] = newNode;
      m_size++;

      return { iterator(this, newNode, index), true };
    }

    // 削除 (Erase)
    // ノードを破棄せず、リサイクルリストに回す
    size_type erase(const key_type& key)
    {
      size_t index = GetIndex(key);
      Node* current = m_buckets[index];
      Node* prev = nullptr;

      while (current)
      {
        if (m_keyEqual(current->m_pair.first, key))
        {
          if (prev) prev->m_next = current->m_next;
          else m_buckets[index] = current->m_next;

          RecycleNode(current); // ここで再利用リストへ
          m_size--;
          return 1;
        }
        prev = current;
        current = current->m_next;
      }
      return 0;
    }

    // 検索 (透過的)
    template<typename K>
    iterator Find(const K& key)
    {
      size_t index = GetIndex(key);
      if (Node* node = FindNode(key, index)) return iterator(this, node, index);
      return end();
    }

    // アクセス
    mapped_type& operator[](const key_type& key) { return try_emplace(key).first->second; }

    iterator begin()
    {
      size_t i = 0;
      while (i < m_bucketCount && !m_buckets[i]) ++i;
      return i == m_bucketCount ? end() : iterator(this, m_buckets[i], i);
    }
    iterator end() { return iterator(this, nullptr, m_bucketCount); }

    // リセット
    void Clear()
    {
      for (size_t i = 0; i < m_bucketCount; ++i) {
        Node* curr = m_buckets[i];
        while (curr) {
          Node* next = curr->m_next;
          bool wasSmall = curr->m_isSmall;  // destroy前に退避

          AllocatorTraits::destroy(m_allocator, curr); // デストラクタ呼び出し

          // SBO外（ヒープ）にあるノードならメモリ解放
          if (!wasSmall) {
            AllocatorTraits::deallocate(m_allocator, curr, 1);
          }
          // SBO内のノードは何もしなくて良い（後でまとめてリセットされる）

          curr = next;
        }
        m_buckets[i] = nullptr;
      }

      m_size = 0;

      // リサイクルリスト（ヒープ）の解放
      // ※ RecycleNodeで既にdestroy済みなのでdeallocateのみ
      while (m_heapRecycledList) {
        Node* next = m_heapRecycledList->m_next;
        AllocatorTraits::deallocate(m_allocator, m_heapRecycledList, 1);
        m_heapRecycledList = next;
      }

      // 状態のリセット
      if (!IsUsingSmallStorage() && SmallSize > 0)
      {
        delete[] m_buckets; // ヒープバケット解放
        m_storage.emplace(); // SBOリセット
        ResetToSBO();
      }
      else if (IsUsingSmallStorage())
      {
        ResetToSBO();
      }
    }

    void Swap(HashMap& other) noexcept
    {
      if (this == &other) return;
      HashMap temp(std::move(*this));
      *this = std::move(other);
      other = std::move(temp);
    }

    [[nodiscard]] size_t Size() const noexcept { return m_size; }
    [[nodiscard]] bool Empty() const noexcept { return m_size == 0; }
    [[nodiscard]] bool IsUsingSmallStorage() const noexcept {
      if constexpr (SmallSize > 0) return m_buckets == m_storage->m_buckets.data();
      return false;
    }

  private:
    // --- 内部ロジック ---
    template<typename K>
    size_t GetIndex(const K& key) const { return m_hasher(key) % m_bucketCount; }

    bool NeedsRehash() const {
      // SBO中はバッファサイズを超えたら必須
      if (IsUsingSmallStorage()) return m_size >= SmallSize;
      // ヒープ中は負荷率で判定
      return LoadFactor() > m_maxLoadFactor;
    }

    float LoadFactor() const  { 
      return m_bucketCount > 0 ? static_cast<float>(m_size) / m_bucketCount : 0.0f; 
    }

    template<typename K>
    Node* FindNode(const K& key, size_t index) const {
      Node* current = m_buckets[index];
      while (current)
      {
        if (m_keyEqual(current->m_pair.first, key)) return current;
        current = current->m_next;
      }
      return nullptr;
    }

    // 例外安全なRehash
    void Rehash(size_t newBucketCount) {
      if (newBucketCount == 0) return;

      // 新しいバケット配列の確保
      Node** newBuckets = nullptr;
      try {
        newBuckets = new Node * [newBucketCount]();
      }
      catch (...) {
        throw;
      }

      // ガード: 関数終了時にcommitフラグがfalseならnewBucketsを削除
      bool commit = false;
      auto bucketGuard = std::unique_ptr<Node * []>(newBuckets);

      bool wasUsingSBO = IsUsingSmallStorage();
      std::vector<Node*> nodesToMove;
      nodesToMove.reserve(m_size);

      // ノードの準備
      if (wasUsingSBO) {
        try {
          for (auto it = begin(); it != end(); ++it) {
            // 新しいヒープノードを確保してムーブ構築
            Node* newNode = AllocatorTraits::allocate(m_allocator, 1);
            try {
              AllocatorTraits::construct(m_allocator, newNode, std::move(*it));
            }
            catch (...) {
              AllocatorTraits::deallocate(m_allocator, newNode, 1);
              throw;
            }
            newNode->m_isSmall = false;  // ヒープノードとして明示的にマーク
            newNode->m_next = nullptr;
            nodesToMove.push_back(newNode);
          }
        }
        catch (...) {
          // 作成途中の一時ノードを破棄
          for (Node* n : nodesToMove) {
            AllocatorTraits::destroy(m_allocator, n);
            AllocatorTraits::deallocate(m_allocator, n, 1);
          }
          throw; // Rehash中止、元の状態は維持される
        }
      }

      // コミットフェーズ（ここからは例外を出さない）
      bucketGuard.release(); // 所有権を放棄（m_bucketsに渡すため）

      if (wasUsingSBO) {
        // 古いSBOノードの破棄
        DestroyAllNodesInSBO();
        m_storage.reset(); // SBO領域を無効化（必要なら）

        // 新しいノードを配置
        for (Node* node : nodesToMove) {
          size_t idx = m_hasher(node->m_pair.first) % newBucketCount;
          node->m_next = newBuckets[idx];
          newBuckets[idx] = node;
        }
      }
      else {
        for (size_t i = 0; i < m_bucketCount; ++i) {
          Node* curr = m_buckets[i];
          while (curr) {
            Node* next = curr->m_next;
            size_t idx = m_hasher(curr->m_pair.first) % newBucketCount;
            curr->m_next = newBuckets[idx];
            newBuckets[idx] = curr;
            curr = next;
          }
        }
        delete[] m_buckets; // 古いバケット配列を削除
      }

      m_buckets = newBuckets;
      m_bucketCount = newBucketCount;
    }

    bool IsSmallNode(Node* node) const noexcept {
      return node && node->m_isSmall;
    }

    void DestroyAllNodesInSBO() {
      // バケット経由でアクティブなノードを探してdestroy
      for (size_t i = 0; i < m_bucketCount; ++i) {
        Node* current = m_buckets[i];
        while (current) {
          Node* next = current->m_next;
          if (current->m_isSmall) {
            AllocatorTraits::destroy(m_allocator, current);
          }
          current = next;
        }
      }
    }

    // ムーブロジックの共通化
    void MoveFrom(HashMap&& other) {
      m_maxLoadFactor = other.m_maxLoadFactor;
      m_hasher = std::move(other.m_hasher);
      m_keyEqual = std::move(other.m_keyEqual);

      if (other.IsUsingSmallStorage()) {
        // 相手がSBOの場合、ポインタの横取りはできない（バッファが相手の中にあるため）
        // SBO領域を確保し、中身を「移動」させる必要がある
        m_storage.emplace();
        ResetToSBO();

        // ノードを移動
        for (auto it = other.begin(); it != other.end(); ++it) {
          // SBO内での移動なので、try_emplaceで再構築
          // キーはconst Keyのためコピー、値はムーブ
          try_emplace(it->first, std::move(it->second));
        }
        other.Clear(); // 元のオブジェクトは空にする
      }
      else {
        // 相手がヒープの場合、ポインタを奪うだけでOK
        m_buckets = other.m_buckets;
        m_bucketCount = other.m_bucketCount;
        m_size = other.m_size;
        m_heapRecycledList = other.m_heapRecycledList; // リサイクルリストも奪う

        // 相手を無効化
        other.m_buckets = nullptr;
        other.m_size = 0;
        other.m_bucketCount = 0;
        other.m_heapRecycledList = nullptr;
      }
    }

    void CleanupPartialConstruction() {
      for (size_t i = 0; i < m_bucketCount; ++i) {
        Node* current = m_buckets[i];
        while (current) {
          Node* next = current->m_next;
          bool wasSmall = current->m_isSmall;  // destroy前に退避
          AllocatorTraits::destroy(m_allocator, current);
          if (!wasSmall) {
            AllocatorTraits::deallocate(m_allocator, current, 1);
          }
          current = next;
        }
      }
      if (!IsUsingSmallStorage()) {
        delete[] m_buckets;
      }
    }

    template<typename... Args>
    Node* AllocateNode(Args&&... args) {
      // SBOを使用中なら、SBOのフリーリストから
      if (IsUsingSmallStorage()) {
        if (m_storage->m_sboFreeList) {
          Node* node = m_storage->m_sboFreeList;
          m_storage->m_sboFreeList = node->m_next;
          AllocatorTraits::construct(m_allocator, node, std::forward<Args>(args)...); // pair構築
          node->m_isSmall = true;  // constructがデフォルト初期化子で上書きするため再設定
          node->m_next = nullptr;
          return node;
        }

        throw std::bad_alloc();
      }

      // ヒープのリサイクルリストに空きがあれば再利用
      if (m_heapRecycledList) {
        Node* node = m_heapRecycledList;
        m_heapRecycledList = node->m_next;
        // メモリは確保済みなので、constructのみ呼ぶ
        AllocatorTraits::construct(m_allocator, node, std::forward<Args>(args)...);
        node->m_isSmall = false;  // ヒープノードにフラグ設定
        node->m_next = nullptr;
        return node;
      }

      // なければ新規確保
      Node* node = AllocatorTraits::allocate(m_allocator, 1);
      try {
        AllocatorTraits::construct(m_allocator, node, std::forward<Args>(args)...);
        node->m_isSmall = false;  // 新規ヒープノードにフラグ設定
      }
      catch (...) {
        AllocatorTraits::deallocate(m_allocator, node, 1);
        throw;
      }
      node->m_next = nullptr;
      return node;
    }

    void RecycleNode(Node* node) {
      // デストラクタのみ呼び出し、メモリは解放しない
      AllocatorTraits::destroy(m_allocator, node);

      if (IsUsingSmallStorage()) {
        // SBO領域のノードならSBOフリーリストへ戻す
        node->m_next = m_storage->m_sboFreeList;
        m_storage->m_sboFreeList = node;
      }
      else {
        // ヒープ領域のノードならヒープリサイクルリストへ戻す
        node->m_next = m_heapRecycledList;
        m_heapRecycledList = node;
      }
    }

    void CleanUpHeapRecycledList() {
      while (m_heapRecycledList) {
        Node* next = m_heapRecycledList->m_next;
        AllocatorTraits::deallocate(m_allocator, m_heapRecycledList, 1);
        m_heapRecycledList = next;
      }
    }

    void DestroyAllNodes() {
      // アクティブなノードの破棄
      for (size_t i = 0; i < m_bucketCount; ++i) {
        Node* current = m_buckets[i];
        while (current) {
          Node* next = current->m_next;
          AllocatorTraits::destroy(m_allocator, current);
          if (!IsUsingSmallStorage()) AllocatorTraits::deallocate(m_allocator, current, 1);
          current = next;
        }
        m_buckets[i] = nullptr;
      }

      // リサイクル待ちノード（ヒープ）の解放
      if (!IsUsingSmallStorage()) {
        CleanUpHeapRecycledList();
      }
    }

    void ResetToSBO() {
      m_buckets = m_storage->m_buckets.data();
      m_bucketCount = SmallSize;
      std::fill(m_buckets, m_buckets + SmallSize, nullptr);

      // SBOバッファ全体を初期化してフリーリストを構築
      m_storage->m_sboFreeList = nullptr;
      for (size_t i = 0; i < SmallSize; ++i) {
        Node* node = reinterpret_cast<Node*>(&m_storage->m_nodes[i]);
        node->m_next = m_storage->m_sboFreeList;
        node->m_isSmall = true;  // Bug #2 fix: SBOノードにフラグ設定
        m_storage->m_sboFreeList = node;
      }
    }
  };
}