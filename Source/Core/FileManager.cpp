#include "FileManager.h"
#include "Logger.h"

namespace GLFD::Core {
  FileManager::FileManager(const std::string& rootDirectry)
    : m_rootPath(rootDirectry) {

    // ルートパスが存在するかチェック
    if (!std::filesystem::exists(m_rootPath)) {
      // カレントディレクトリをデフォルトにする
      m_rootPath = std::filesystem::current_path();
      LOG_WARN("Root directory not found. Using current path: %s", m_rootPath.string().c_str());
    }
  }

  bool FileManager::ReadTextFile(const std::string& relativePath, std::string& outContent) {
    auto path = ResolvePath(relativePath);
    std::ifstream file(path, std::ios::in | std::ios::binary); // binaryモードで開いてサイズを取得

    if (!file.is_open()) {
      LOG_ERROR("Failed to open text file: %s", path.string().c_str());
      return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    if (size == -1) return false;

    outContent.resize(size);
    file.seekg(0, std::ios::beg);
    file.read(&outContent[0], size);

    return true;
  }

  bool FileManager::ReadBinaryFile(const std::string& relativePath, std::vector<uint8_t>& outData) {
    auto path = ResolvePath(relativePath);
    std::ifstream file(path, std::ios::in | std::ios::binary);

    if (!file.is_open()) {
      LOG_ERROR("Failed to open binary file: %s", path.string().c_str());
      return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    outData.resize(size);

    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(outData.data()), size);

    return true;
  }

  bool FileManager::WriteTextFile(const std::string& relativePath, const std::string& content)
  {
    auto path = ResolvePath(relativePath);
    std::ofstream file(path);

    if (!file.is_open()) {
      LOG_ERROR("Failed to write text file: %s", path.string().c_str());
      return false;
    }

    file << content;
    return true;
  }

  bool FileManager::WriteBinaryFile(const std::string& relativePath, const void* data, size_t size) {
    auto path = ResolvePath(relativePath);

    std::ofstream file;
    file.open(path, std::ios::out | std::ios::binary);

    if (!file.is_open()) {
      LOG_ERROR("Failed to write binary file: %s", path.string().c_str());
      return false;
    }

    file.write(reinterpret_cast<const char*>(data), size);

    return false;
  }

  bool FileManager::ReadJsonFile(const std::string& relativePath, Json& outJson) {
    std::string content;

    if (!ReadTextFile(relativePath, content)) return false;

    try {
      outJson = Json::parse(content);
    }
    catch (Json::parse_error& e) {
      LOG_ERROR("JSON Parse Error in %s: %s", relativePath.c_str(), e.what());
      return false;
    }
    return true;
  }

  bool FileManager::WriteJsonFile(const std::string& relativePath, const Json& json, bool prettyPrint) {
    std::string content = prettyPrint ? json.dump(4) : json.dump(); // dump(4)でインデント4
    return WriteTextFile(relativePath, content);
  }

  bool FileManager::Exists(const std::string& relativePath) const {
    return std::filesystem::exists(ResolvePath(relativePath));
  }

  std::string FileManager::GetAbsolutePath(const std::string& relativePath) const {
    return std::filesystem::absolute(ResolvePath(relativePath)).string();
  }

  std::filesystem::path FileManager::ResolvePath(const std::string& relativePath) const {
    return m_rootPath / relativePath;
  }
}