#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

enum class MessageRole {
  User,
  Assistant,
  System
};

struct Message {
  MessageRole role = MessageRole::User;
  std::string content;
  std::string created_at;
};

struct ChatSession {
  std::string id;
  std::string native_session_id;
  bool uses_native_session = false;
  std::string folder_id;
  std::string template_override_id;
  bool gemini_md_bootstrapped = false;
  std::string title;
  std::string created_at;
  std::string updated_at;
  std::vector<std::string> linked_files;
  std::vector<Message> messages;
};

struct ChatFolder {
  std::string id;
  std::string title;
  std::string directory;
  bool collapsed = false;
};

struct AppSettings {
  std::string active_provider_id = "gemini";
  std::string gemini_command_template = "gemini {resume} {flags} {prompt}";
  bool gemini_yolo_mode = false;
  std::string gemini_extra_flags;
  std::string gemini_global_root_path;
  std::string default_gemini_template_id;
  std::string ui_theme = "dark";
  bool confirm_delete_chat = true;
  bool confirm_delete_folder = true;
  bool remember_last_chat = true;
  bool mirror_native_gemini_history_to_local = true;
  int native_history_mirror_idle_seconds = 30;
  std::string last_selected_chat_id;
  float ui_scale_multiplier = 1.0f;
  int window_width = 1440;
  int window_height = 860;
  bool window_maximized = false;
};

enum class CenterViewMode {
  Structured,
  CliConsole
};

struct PendingGeminiCall {
  std::string chat_id;
  std::string resume_session_id;
  std::vector<std::string> session_ids_before;
  std::string command_preview;
  std::shared_ptr<std::atomic<bool>> completed;
  std::shared_ptr<std::string> output;
};

struct TemplateCatalogEntry {
  std::string id;
  std::string display_name;
  std::string absolute_path;
  std::string updated_at;
};

std::string RoleToString(MessageRole role);
MessageRole RoleFromString(const std::string& value);
std::string ViewModeToString(CenterViewMode mode);
CenterViewMode ViewModeFromString(const std::string& value);
