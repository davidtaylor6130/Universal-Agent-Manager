#include "settings_store.h"

#include "app_paths.h"

#include <algorithm>
#include <cctype>
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

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool ParseBool(const std::string& value, const bool fallback) {
  const std::string lowered = ToLower(value);
  if (lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes") {
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no") {
    return false;
  }
  return fallback;
}

int ParseInt(const std::string& value, const int fallback) {
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

float ParseFloat(const std::string& value, const float fallback) {
  try {
    return std::stof(value);
  } catch (...) {
    return fallback;
  }
}

std::string NormalizeThemeId(std::string value) {
  value = ToLower(value);
  if (value == "light") {
    return "light";
  }
  if (value == "system") {
    return "system";
  }
  return "dark";
}

}  // namespace

bool SettingsStore::Save(const std::filesystem::path& settings_file,
                         const AppSettings& settings,
                         const CenterViewMode center_view_mode) {
  std::error_code ec;
  std::filesystem::create_directories(settings_file.parent_path(), ec);
  std::ostringstream lines;
  lines << "active_provider_id=" << settings.active_provider_id << '\n';
  lines << "gemini_command_template=" << settings.gemini_command_template << '\n';
  lines << "gemini_yolo_mode=" << (settings.gemini_yolo_mode ? "1" : "0") << '\n';
  lines << "gemini_extra_flags=" << settings.gemini_extra_flags << '\n';
  lines << "gemini_global_root_path=" << settings.gemini_global_root_path << '\n';
  lines << "default_gemini_template_id=" << settings.default_gemini_template_id << '\n';
  lines << "center_view_mode=" << ViewModeToString(center_view_mode) << '\n';
  lines << "ui_theme=" << NormalizeThemeId(settings.ui_theme) << '\n';
  lines << "confirm_delete_chat=" << (settings.confirm_delete_chat ? "1" : "0") << '\n';
  lines << "confirm_delete_folder=" << (settings.confirm_delete_folder ? "1" : "0") << '\n';
  lines << "remember_last_chat=" << (settings.remember_last_chat ? "1" : "0") << '\n';
  lines << "mirror_native_gemini_history_to_local=" << (settings.mirror_native_gemini_history_to_local ? "1" : "0") << '\n';
  lines << "delete_empty_native_gemini_chats_on_import=" << (settings.delete_empty_native_gemini_chats_on_import ? "1" : "0") << '\n';
  lines << "native_history_mirror_idle_seconds=" << settings.native_history_mirror_idle_seconds << '\n';
  lines << "last_selected_chat_id=" << settings.last_selected_chat_id << '\n';
  lines << "ui_scale_multiplier=" << settings.ui_scale_multiplier << '\n';
  lines << "window_width=" << settings.window_width << '\n';
  lines << "window_height=" << settings.window_height << '\n';
  lines << "window_maximized=" << (settings.window_maximized ? "1" : "0") << '\n';
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
    if (key == "active_provider_id") {
      settings.active_provider_id = value;
    } else if (key == "gemini_command_template") {
      settings.gemini_command_template = value;
    } else if (key == "gemini_yolo_mode") {
      settings.gemini_yolo_mode = ParseBool(value, settings.gemini_yolo_mode);
    } else if (key == "gemini_extra_flags") {
      settings.gemini_extra_flags = value;
    } else if (key == "gemini_global_root_path") {
      settings.gemini_global_root_path = value;
    } else if (key == "default_gemini_template_id") {
      settings.default_gemini_template_id = value;
    } else if (key == "center_view_mode") {
      center_view_mode = ViewModeFromString(value);
    } else if (key == "ui_theme") {
      settings.ui_theme = NormalizeThemeId(value);
    } else if (key == "confirm_delete_chat") {
      settings.confirm_delete_chat = ParseBool(value, settings.confirm_delete_chat);
    } else if (key == "confirm_delete_folder") {
      settings.confirm_delete_folder = ParseBool(value, settings.confirm_delete_folder);
    } else if (key == "remember_last_chat") {
      settings.remember_last_chat = ParseBool(value, settings.remember_last_chat);
    } else if (key == "mirror_native_gemini_history_to_local") {
      settings.mirror_native_gemini_history_to_local = ParseBool(value, settings.mirror_native_gemini_history_to_local);
    } else if (key == "delete_empty_native_gemini_chats_on_import") {
      settings.delete_empty_native_gemini_chats_on_import =
          ParseBool(value, settings.delete_empty_native_gemini_chats_on_import);
    } else if (key == "native_history_mirror_idle_seconds") {
      settings.native_history_mirror_idle_seconds = ParseInt(value, settings.native_history_mirror_idle_seconds);
    } else if (key == "last_selected_chat_id") {
      settings.last_selected_chat_id = value;
    } else if (key == "ui_scale_multiplier") {
      settings.ui_scale_multiplier = ParseFloat(value, settings.ui_scale_multiplier);
    } else if (key == "window_width") {
      settings.window_width = ParseInt(value, settings.window_width);
    } else if (key == "window_height") {
      settings.window_height = ParseInt(value, settings.window_height);
    } else if (key == "window_maximized") {
      settings.window_maximized = ParseBool(value, settings.window_maximized);
    }
  }

  if (settings.gemini_command_template.empty() || settings.gemini_command_template == "gemini -p {prompt}") {
    settings.gemini_command_template = "gemini {resume} {flags} {prompt}";
  }
  if (settings.active_provider_id.empty()) {
    settings.active_provider_id = "gemini";
  }
  if (settings.gemini_global_root_path.empty()) {
    settings.gemini_global_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
  }
  settings.ui_theme = NormalizeThemeId(settings.ui_theme);
  settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
  settings.native_history_mirror_idle_seconds = std::clamp(settings.native_history_mirror_idle_seconds, 5, 3600);
  settings.window_width = std::clamp(settings.window_width, 960, 8192);
  settings.window_height = std::clamp(settings.window_height, 620, 8192);
  if (!settings.remember_last_chat) {
    settings.last_selected_chat_id.clear();
  }
}
