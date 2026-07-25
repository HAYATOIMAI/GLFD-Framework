#include "ResourceManager.h"

namespace GLFD::Resource {

  ResourceManager::ResourceManager()
    : m_typeMap(Memory::GetDefaultResource()) {
  }

  ResourceManager::~ResourceManager() {
    ReleaseAll();
  }

  void ResourceManager::ReleaseAll() {
    for (size_t i = 0; i < m_typeMap.GetSize(); ++i) {
      if (m_typeMap[i].storage) {
        m_typeMap[i].storage->Clear();
      }
    }
    m_typeMap.Clear();
  }

} // namespace GLFD::Resource
