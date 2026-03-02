#include "app_paths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
namespace fs = std::filesystem;

std::string Trim(std::string value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}
}  // namespace

std::filesystem::path AppPaths::SettingsFilePath(const std::filesystem::path& data_root) {
  return data_root / "settings.txt";
}

std::filesystem::path AppPaths::ChatsRootPath(const std::filesystem::path& data_root) {
  return data_root / "chats";
}

std::filesystem::path AppPaths::ChatPath(const std::filesystem::path& data_root, const std::string& chat_id) {
  return ChatsRootPath(data_root) / chat_id;
}

std::filesystem::path AppPaths::GeminiHomePath() {
  if (const char* gemini_home = std::getenv("GEMINI_HOME")) {
    return std::filesystem::path(gemini_home);
  }
  if (const char* home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".gemini";
  }
  return std::filesystem::current_path() / ".gemini";
}

std::optional<std::filesystem::path> AppPaths::ResolveGeminiProjectTmpDir(const std::filesystem::path& project_root) {
  namespace fs = std::filesystem;
  const fs::path tmp_root = GeminiHomePath() / "tmp";
  if (!fs::exists(tmp_root) || !fs::is_directory(tmp_root)) {
    return std::nullopt;
  }

  std::error_code ec;
  const fs::path canonical_project = fs::weakly_canonical(project_root, ec);
  const fs::path normalized_project = ec ? project_root : canonical_project;

  for (const auto& item : fs::directory_iterator(tmp_root, ec)) {
    if (ec || !item.is_directory()) {
      continue;
    }
    const fs::path project_root_file = item.path() / ".project_root";
    if (!fs::exists(project_root_file)) {
      continue;
    }
    const std::string recorded_path_raw = Trim(ReadTextFile(project_root_file));
    if (recorded_path_raw.empty()) {
      continue;
    }
    const fs::path recorded_path = fs::path(recorded_path_raw);
    const fs::path canonical_recorded = fs::weakly_canonical(recorded_path, ec);
    const fs::path normalized_recorded = ec ? recorded_path : canonical_recorded;
    if (normalized_recorded == normalized_project) {
      return item.path();
    }
  }

  return std::nullopt;
}
