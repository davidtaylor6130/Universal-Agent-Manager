#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

/// <summary>
/// Message role marker persisted with each chat message entry.
/// </summary>
enum class MessageRole {
  User,
  Assistant,
  System
};

/// <summary>
/// One persisted chat message payload.
/// </summary>
struct Message {
  MessageRole role = MessageRole::User;
  std::string content;
  std::string created_at;
};

/// <summary>
/// Chat session metadata and message history.
/// </summary>
struct ChatSession {
  std::string id;
  std::string provider_id;
  std::string native_session_id;
  bool uses_native_session = false;
  std::string parent_chat_id;
  std::string branch_root_chat_id;
  int branch_from_message_index = -1;
  std::string folder_id;
  std::string template_override_id;
  bool gemini_md_bootstrapped = false;
  bool rag_enabled = true;
  std::vector<std::string> rag_source_directories;
  std::string import_source_kind;
  std::string import_source_ref;
  std::string title;
  std::string created_at;
  std::string updated_at;
  std::vector<std::string> linked_files;
  std::vector<Message> messages;
};

/// <summary>
/// User-defined chat folder metadata.
/// </summary>
struct ChatFolder {
  std::string id;
  std::string title;
  std::string directory;
  bool collapsed = false;
  std::string import_source_kind;
  std::string import_source_ref;
};

/// <summary>
/// Persisted application settings.
/// </summary>
struct AppSettings {
  std::string active_provider_id = "gemini-structured";
  std::string provider_command_template = "gemini {resume} {flags} -p {prompt}";
  bool provider_yolo_mode = false;
  std::string provider_extra_flags;
  std::string runtime_backend = "provider-cli";
  std::string selected_model_id;
  std::string models_folder_directory;
  std::string vector_db_backend = "ollama-engine";
  std::string selected_vector_model_id;
  std::string vector_database_name_override;
  int cli_idle_timeout_seconds = 300;
  int cli_max_columns = 160;
  std::string prompt_profile_root_path;
  std::string default_prompt_profile_id;
  // Legacy keys retained for backward-compatible load paths.
  std::string gemini_command_template = "gemini {resume} {flags} -p {prompt}";
  bool gemini_yolo_mode = false;
  std::string gemini_extra_flags;
  std::string gemini_global_root_path;
  std::string default_gemini_template_id;
  bool rag_enabled = true;
  int rag_top_k = 6;
  int rag_max_snippet_chars = 600;
  int rag_max_file_bytes = 1024 * 1024;
  int rag_scan_max_tokens = 0;
  std::string rag_project_source_directory;
  std::string ui_theme = "dark";
  bool confirm_delete_chat = true;
  bool confirm_delete_folder = true;
  bool remember_last_chat = true;
  std::string last_selected_chat_id;
  float ui_scale_multiplier = 1.0f;
  int window_width = 1440;
  int window_height = 860;
  bool window_maximized = false;
};

/// <summary>
/// Main center-pane rendering mode.
/// </summary>
enum class CenterViewMode {
  Structured,
  CliConsole
};

/// <summary>
/// In-flight provider command state used by async polling.
/// </summary>
struct PendingGeminiCall {
  std::string chat_id;
  std::string resume_session_id;
  std::vector<std::string> session_ids_before;
  std::string command_preview;
  std::shared_ptr<std::atomic<bool>> completed;
  std::shared_ptr<std::string> output;
};

/// <summary>
/// One template entry discovered in the Gemini Markdown catalog.
/// </summary>
struct TemplateCatalogEntry {
  std::string id;
  std::string display_name;
  std::string absolute_path;
  std::string updated_at;
};

/// <summary>
/// Converts a message role enum into persisted text.
/// </summary>
std::string RoleToString(MessageRole role);
/// <summary>
/// Parses persisted message role text into an enum value.
/// </summary>
MessageRole RoleFromString(const std::string& value);
/// <summary>
/// Converts center view mode enum into persisted text.
/// </summary>
std::string ViewModeToString(CenterViewMode mode);
/// <summary>
/// Parses persisted center view mode text into an enum value.
/// </summary>
CenterViewMode ViewModeFromString(const std::string& value);
