#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace GLFD::Core {
  using Json = nlohmann::json;

  class FileManager {
  public:
    FileManager(const std::string& rootDirectry = ".");

    bool ReadTextFile(const std::string& relativePath, std::string& outContent);
    bool ReadBinaryFile(const std::string& relativePath, std::vector<uint8_t>& outData);

    bool WriteTextFile(const std::string& relativePath, const std::string& content);
    bool WriteBinaryFile(const std::string& relativePath, const void* data, size_t size);

    // --- JSON操作 ---
    // ファイルからJSONオブジェクトを読み込む
    bool ReadJsonFile(const std::string& relativePath, Json& outJson);

    // JSONオブジェクトをファイルに保存する (prettyPrint=true で整形)
    bool WriteJsonFile(const std::string& relativePath, const Json& json, bool prettyPrint = true);

    // --- ユーティリティ ---
    bool Exists(const std::string& relativePath) const;
    std::string GetAbsolutePath(const std::string& relativePath) const;

  private:
    std::filesystem::path m_rootPath;
    std::filesystem::path ResolvePath(const std::string& relativePath) const;
  };
}