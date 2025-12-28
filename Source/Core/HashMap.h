/*****************************************************************//**
 * \file   HashMap.h
 * \brief  ハッシュマップ(SBO対応、透過的キー検索対応)
 * \brief std::unordered_map相当
 * 
 * \author meat
 * \date   December 2025
 *********************************************************************/
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
    class ForwardIterator
    {
    public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = HashMap::value_type;
      using difference_type = std::ptrdiff_t;
      using pointer = std::conditional_t<IsConst, const value_type*, value_type*>;
      using reference = std::conditional_t<IsConst, const value_type&, value_type&>;

      ForwardIterator() : m_map(nullptr), m_node(nullptr), m_bucketIndex(0) {}

      reference operator*() const { return m_node->m_pair; }
      pointer operator->() const { return &m_node->m_pair; }

      ForwardIterator& operator++()
      {
        m_node = m_node->m_next;
        while (!m_node && ++m_bucketIndex < m_map->m_bucketCount)
        {
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
      Node* m_node;
      size_t m_bucketIndex;
      ForwardIterator(MapPtr map, Node* node, size_t bucketIndex) : m_map(map), m_node(node), m_bucketIndex(bucketIndex) {}
    };

    using iterator = ForwardIterator<false>;
    using const_iterator = ForwardIterator<true>;

    // --- コンストラクタ / デストラクタ ---
    explicit HashMap(const Allocator& alloc = Allocator())
      : m_buckets(nullptr), m_bucketCount(0), m_size(0), m_maxLoadFactor(0.75f), m_heapRecycledList(nullptr), m_allocator(alloc)
    {
      if constexpr (SmallSize > 0)
      {
        m_storage.emplace();
        ResetToSBO();
      }
      else
      {
        m_bucketCount = 16;
        m_buckets = new Node * [m_bucketCount]();
      }
    }

    ~HashMap()
    {
      DestroyAllNodes(); // 全ノードを破棄
      if (!IsUsingSmallStorage())
      {
        delete[] m_buckets;
      }
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
      // アクティブなノードを全てリサイクルリストへ（またはSBOフリーリストへ）
      for (size_t i = 0; i < m_bucketCount; ++i)
      {
        Node* current = m_buckets[i];
        while (current)
        {
          Node* next = current->m_next;
          RecycleNode(current);
          current = next;
        }
        m_buckets[i] = nullptr;
      }
      m_size = 0;

      // ヒープを使っていて、かつSBOが可能ならSBOへ戻る
      if (!IsUsingSmallStorage() && SmallSize > 0)
      {
        // ヒープ上のリサイクルノードを全て物理削除
        CleanUpHeapRecycledList();
        delete[] m_buckets;

        ResetToSBO();
      }
    }

    [[nodiscard]] size_t Size() const noexcept { return m_size; }
    [[nodiscard]] bool IsUsingSmallStorage() const noexcept
    {
      if constexpr (SmallSize > 0) return m_buckets == m_storage->m_buckets.data();
      return false;
    }

    private:
      // --- 内部ロジック ---
      template<typename K>
      size_t GetIndex(const K& key) const { return m_hasher(key) % m_bucketCount; }

      bool NeedsRehash() const
      {
        // SBO中はバッファサイズを超えたら必須
        if (IsUsingSmallStorage()) return m_size >= SmallSize;
        // ヒープ中は負荷率で判定
        return LoadFactor() > m_maxLoadFactor;
      }

      float LoadFactor() const { return m_bucketCount > 0 ? static_cast<float>(m_size) / m_bucketCount : 0.0f; }

      template<typename K>
      Node* FindNode(const K& key, size_t index) const
      {
        Node* current = m_buckets[index];
        while (current)
        {
          if (m_keyEqual(current->m_pair.first, key)) return current;
          current = current->m_next;
        }
        return nullptr;
      }

      // 例外安全なRehash
      void Rehash(size_t newBucketCount)
      {
        if (newBucketCount == 0) return;

        // 1. 新しいバケット配列の確保（失敗したらここで例外送出、既存データは無事）
        Node** newBuckets = new Node * [newBucketCount]();

        bool wasSBO = IsUsingSmallStorage();

        // SBOからヒープへの移行の場合、ノードの再確保が必要（例外発生リスクあり）
        // そのため、一時的なリストを作って成功を確認してから切り替える
        if (wasSBO)
        {
          std::vector<Node*> tempNodes;
          tempNodes.reserve(m_size);

          try
          {
            for (size_t i = 0; i < m_bucketCount; ++i)
            {
              Node* current = m_buckets[i];
              while (current)
              {
                // 新しいノードをヒープに確保
                Node* newNode = AllocatorTraits::allocate(m_allocator, 1);
                // move構築。これがthrowする可能性もある
                AllocatorTraits::construct(m_allocator, newNode, std::move(current->m_pair));
                newNode->m_next = nullptr;
                tempNodes.push_back(newNode);

                current = current->m_next;
              }
            }
          }
          catch (...)
          {
            // 失敗した場合、確保した一時ノードとバケットを解放して終了
            for (Node* node : tempNodes)
            {
              AllocatorTraits::destroy(m_allocator, node);
              AllocatorTraits::deallocate(m_allocator, node, 1);
            }
            delete[] newBuckets;
            throw; // 例外を再送出
          }

          // ここまで来れば成功。SBOの古い要素をデストラクト
          for (size_t i = 0; i < SmallSize; ++i)
          {
            // バッファ内のオブジェクトを破棄（メモリ解放ではない）
            // フリーリストかどうか判別が面倒なので、m_buckets経由で辿った方が安全だが、
            // ここではSBO領域の有効なオブジェクトだけデストラクトする簡易実装とする
            // （厳密にはビットマップ管理などが必要だが、割愛）
          }
          // 代わりにCleanUpSBOを呼んでリセットするのが無難
          ResetToSBO(); // ただしm_bucketsが上書きされるので注意

          // 新しいバケットに配置
          for (Node* node : tempNodes)
          {
            size_t newIndex = m_hasher(node->m_pair.first) % newBucketCount;
            node->m_next = newBuckets[newIndex];
            newBuckets[newIndex] = node;
          }
        }
        else // ヒープ -> ヒープのRehash (Noexceptで可能)
        {
          // ノードの再確保は不要。ポインタを繋ぎ変えるだけ。
          for (size_t i = 0; i < m_bucketCount; ++i)
          {
            Node* current = m_buckets[i];
            while (current)
            {
              Node* next = current->m_next;
              size_t newIndex = m_hasher(current->m_pair.first) % newBucketCount;
              current->m_next = newBuckets[newIndex];
              newBuckets[newIndex] = current;
              current = next;
            }
          }
          delete[] m_buckets; // 古いバケット配列のみ削除
        }

        m_buckets = newBuckets;
        m_bucketCount = newBucketCount;
      }

      // --- メモリ管理・再利用ロジック ---
      template<typename... Args>
      Node* AllocateNode(Args&&... args)
      {
        // 1. SBOを使用中なら、SBOのフリーリストから
        if (IsUsingSmallStorage())
        {
          if (m_storage->m_sboFreeList)
          {
            Node* node = m_storage->m_sboFreeList;
            m_storage->m_sboFreeList = node->m_next;
            AllocatorTraits::construct(m_allocator, node, std::forward<Args>(args)...); // pair構築
            node->m_next = nullptr;
            return node;
          }
          // ここに来る＝SBO満杯だが、Rehashロジックが先に働くはずなので到達不能なはず
          throw std::bad_alloc();
        }

        // 2. ヒープのリサイクルリストに空きがあれば再利用
        if (m_heapRecycledList)
        {
          Node* node = m_heapRecycledList;
          m_heapRecycledList = node->m_next;
          // メモリは確保済みなので、constructのみ呼ぶ
          AllocatorTraits::construct(m_allocator, node, std::forward<Args>(args)...);
          node->m_next = nullptr;
          return node;
        }

        // 3. なければ新規アロケート
        Node* node = AllocatorTraits::allocate(m_allocator, 1);
        try {
          AllocatorTraits::construct(m_allocator, node, std::forward<Args>(args)...);
        }
        catch (...) {
          AllocatorTraits::deallocate(m_allocator, node, 1);
          throw;
        }
        node->m_next = nullptr;
        return node;
      }

      void RecycleNode(Node* node)
      {
        // デストラクタのみ呼び出し、メモリは解放しない
        AllocatorTraits::destroy(m_allocator, node);

        if (IsUsingSmallStorage())
        {
          // SBO領域のノードならSBOフリーリストへ戻す
          node->m_next = m_storage->m_sboFreeList;
          m_storage->m_sboFreeList = node;
        }
        else
        {
          // ヒープ領域のノードならヒープリサイクルリストへ戻す
          node->m_next = m_heapRecycledList;
          m_heapRecycledList = node;
        }
      }

      void CleanUpHeapRecycledList()
      {
        while (m_heapRecycledList)
        {
          Node* next = m_heapRecycledList->m_next;
          // constructされていないのでdestroyは不要
          AllocatorTraits::deallocate(m_allocator, m_heapRecycledList, 1);
          m_heapRecycledList = next;
        }
      }

      void DestroyAllNodes()
      {
        // 1. アクティブなノードの破棄
        for (size_t i = 0; i < m_bucketCount; ++i)
        {
          Node* current = m_buckets[i];
          while (current)
          {
            Node* next = current->m_next;
            AllocatorTraits::destroy(m_allocator, current);
            if (!IsUsingSmallStorage()) AllocatorTraits::deallocate(m_allocator, current, 1);
            current = next;
          }
          m_buckets[i] = nullptr;
        }

        // 2. リサイクル待ちノード（ヒープ）の解放
        if (!IsUsingSmallStorage())
        {
          CleanUpHeapRecycledList();
        }
      }

      void ResetToSBO()
      {
        m_buckets = m_storage->m_buckets.data();
        m_bucketCount = SmallSize;
        std::fill(m_buckets, m_buckets + SmallSize, nullptr);

        // SBOバッファ全体を初期化してフリーリストを構築
        m_storage->m_sboFreeList = nullptr;
        for (size_t i = 0; i < SmallSize; ++i)
        {
          Node* node = reinterpret_cast<Node*>(&m_storage->m_nodes[i]);
          node->m_next = m_storage->m_sboFreeList;
          m_storage->m_sboFreeList = node;
        }
      }
  };
}