#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

namespace GLFD::Core {

  class FileManager {
  public:
    FileManager(const std::string& rootDirectry = ".");

    bool ReadTextFile(const std::string& relativePath, std::string& outContent);
    bool ReadBinaryFile(const std::string& relativePath, std::vector<uint8_t>& outData);

    bool WriteTextFile(const std::string& relativePath, const std::string& content);
    bool WriteBinaryFile(const std::string& relativePath, const void* data, size_t size);

    // --- ユーティリティ ---
    bool Exists(const std::string& relativePath) const;
    std::string GetAbsolutePath(const std::string& relativePath) const;

  private:
    std::filesystem::path m_rootPath;
    std::filesystem::path ResolvePath(const std::string& relativePath) const;
  };
}