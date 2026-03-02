#include "settings_store.h"

#include <fstream>
#include <sstream>

namespace {

bool WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    return false;
  }
  out << content;
  return out.good();
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

bool SettingsStore::Save(const std::filesystem::path& settings_file,
                         const AppSettings& settings,
                         const CenterViewMode center_view_mode) {
  std::error_code ec;
  std::filesystem::create_directories(settings_file.parent_path(), ec);
  std::ostringstream lines;
  lines << "gemini_command_template=" << settings.gemini_command_template << '\n';
  lines << "gemini_yolo_mode=" << (settings.gemini_yolo_mode ? "1" : "0") << '\n';
  lines << "gemini_extra_flags=" << settings.gemini_extra_flags << '\n';
  lines << "center_view_mode=" << ViewModeToString(center_view_mode) << '\n';
  return WriteTextFile(settings_file, lines.str());
}

void SettingsStore::Load(const std::filesystem::path& settings_file,
                         AppSettings& settings,
                         CenterViewMode& center_view_mode) {
  if (!std::filesystem::exists(settings_file)) {
    return;
  }

  std::istringstream lines(ReadTextFile(settings_file));
  std::string line;
  while (std::getline(lines, line)) {
    const auto equals_at = line.find('=');
    if (equals_at == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, equals_at);
    const std::string value = line.substr(equals_at + 1);
    if (key == "gemini_command_template") {
      settings.gemini_command_template = value;
    } else if (key == "gemini_yolo_mode") {
      settings.gemini_yolo_mode = (value == "1" || value == "true" || value == "on");
    } else if (key == "gemini_extra_flags") {
      settings.gemini_extra_flags = value;
    } else if (key == "center_view_mode") {
      center_view_mode = ViewModeFromString(value);
    }
  }
}
