#pragma once
#include "ResourceHandle.h"
#include "IResourceLoader.h"
#include "../Core/DynamicArray.h"
#include "../Core/TypeInfo.h"
#include "../Core/Logger.h"

#include <memory>
#include <string>
#include <vector>

namespace GLFD::Resource {

  // Type-erased base for per-type storage
  class IResourceStorage {
  public:
    virtual ~IResourceStorage() = default;
    virtual void Clear() = 0;
  };

  // Typed resource storage
  template <typename T>
  class ResourceStorage : public IResourceStorage {
  public:
    struct Entry {
      size_t id;
      std::string key;
      std::unique_ptr<T> resource;
    };

    void SetLoader(std::unique_ptr<IResourceLoader<T>> loader) {
      m_loader = std::move(loader);
    }

    ResourceHandle<T> Load(const std::string& key, GameContext& ctx) {
      size_t id = HashString(key);

      // Cache check (compare both hash and key to handle hash collisions)
      for (auto& entry : m_entries) {
        if (entry.id == id && entry.key == key) {
          return ResourceHandle<T>{ id };
        }
      }

      if (!m_loader) {
        LOG_ERROR("ResourceManager: No loader registered for this resource type");
        return ResourceHandle<T>{};
      }

      auto resource = m_loader->Load(key, ctx);
      if (!resource) {
        LOG_ERROR("ResourceManager: Failed to load '%s'", key.c_str());
        return ResourceHandle<T>{};
      }

      LOG_INFO("ResourceManager: Loaded '%s'", key.c_str());
      m_entries.push_back(Entry{ id, key, std::move(resource) });
      return ResourceHandle<T>{ id };
    }

    T* Get(size_t id) {
      for (auto& entry : m_entries) {
        if (entry.id == id) {
          return entry.resource.get();
        }
      }
      return nullptr;
    }

    T* GetByKey(const std::string& key) {
      return Get(HashString(key));
    }

    void Release(size_t id) {
      for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].id == id) {
          // swap-and-pop for O(1) removal (order does not matter)
          if (i != m_entries.size() - 1) {
            m_entries[i] = std::move(m_entries.back());
          }
          m_entries.pop_back();
          return;
        }
      }
    }

    void Clear() override {
      m_entries.clear();
      m_entries.shrink_to_fit();
    }

  private:
    // std::vector used here: Entry contains unique_ptr/string (see coding guidelines exception)
    std::vector<Entry> m_entries;
    std::unique_ptr<IResourceLoader<T>> m_loader;
  };

  // Type map entry for ResourceManager
  struct TypeMapEntry {
    size_t typeHash = 0;
    std::unique_ptr<IResourceStorage> storage;

    TypeMapEntry() = default;
    TypeMapEntry(size_t hash, std::unique_ptr<IResourceStorage> s)
      : typeHash(hash), storage(std::move(s)) {}

    TypeMapEntry(TypeMapEntry&&) noexcept = default;
    TypeMapEntry& operator=(TypeMapEntry&&) noexcept = default;

    TypeMapEntry(const TypeMapEntry&) = delete;
    TypeMapEntry& operator=(const TypeMapEntry&) = delete;
  };

  class ResourceManager {
  public:
    ResourceManager();
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    template <typename T>
    void RegisterLoader(std::unique_ptr<IResourceLoader<T>> loader) {
      constexpr size_t typeHash = TypeInfo::GetID<T>();

      for (size_t i = 0; i < m_typeMap.GetSize(); ++i) {
        if (m_typeMap[i].typeHash == typeHash) {
          auto* storage = static_cast<ResourceStorage<T>*>(m_typeMap[i].storage.get());
          storage->SetLoader(std::move(loader));
          return;
        }
      }

      auto storage = std::make_unique<ResourceStorage<T>>();
      storage->SetLoader(std::move(loader));
      m_typeMap.EmplaceBack(typeHash, std::move(storage));
    }

    template <typename T>
    ResourceHandle<T> Load(const std::string& key, GameContext& ctx) {
      auto* storage = GetStorage<T>();
      if (!storage) {
        LOG_ERROR("ResourceManager: No storage registered for type");
        return ResourceHandle<T>{};
      }
      return storage->Load(key, ctx);
    }

    template <typename T>
    T* Get(ResourceHandle<T> handle) {
      if (!handle.IsValid()) return nullptr;
      auto* storage = GetStorage<T>();
      if (!storage) return nullptr;
      return storage->Get(handle.id);
    }

    template <typename T>
    T* GetByKey(const std::string& key) {
      auto* storage = GetStorage<T>();
      if (!storage) return nullptr;
      return storage->GetByKey(key);
    }

    template <typename T>
    void Release(ResourceHandle<T> handle) {
      if (!handle.IsValid()) return;
      auto* storage = GetStorage<T>();
      if (!storage) return;
      storage->Release(handle.id);
    }

    void ReleaseAll();

  private:
    DynamicArray<TypeMapEntry> m_typeMap;

    template <typename T>
    ResourceStorage<T>* GetStorage() {
      constexpr size_t typeHash = TypeInfo::GetID<T>();
      for (size_t i = 0; i < m_typeMap.GetSize(); ++i) {
        if (m_typeMap[i].typeHash == typeHash) {
          return static_cast<ResourceStorage<T>*>(m_typeMap[i].storage.get());
        }
      }
      return nullptr;
    }
  };

} // namespace GLFD::Resource
