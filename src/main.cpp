#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <vterm.h>
#include "common/constants/app_constants.h"
#include "common/state/app_state.h"
#include "common/app_models.h"
#include "common/app_paths.h"
#include "common/chat_branching.h"
#include "common/chat_folder_store.h"
#include "common/chat_repository.h"
#include "common/frontend_actions.h"
#include "common/gemini_command_builder.h"
#include "common/gemini_template_catalog.h"
#include "common/provider_profile.h"
#include "common/provider_runtime.h"
#include "common/rag_index_service.h"
#include "common/settings_store.h"
#include "common/vcs_workspace_service.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincontypes.h>
#include <shellapi.h>
#endif

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace fs = std::filesystem;

#if defined(_WIN32)
static void ClosePseudoConsoleSafe(HPCON handle);
#endif

using uam::AppState;
using uam::AsyncCommandTask;
#if defined(_WIN32)
using uam::AsyncNativeChatLoadTask;
#endif
using uam::CliTerminalState;
using uam::TerminalScrollbackLine;
using uam::kTerminalScrollbackMaxLines;

#if defined(_WIN32)
static bool StartCliTerminalWindows(AppState& app, CliTerminalState& terminal, const ChatSession& chat);
#else
static bool StartCliTerminalUnix(AppState& app, CliTerminalState& terminal, const ChatSession& chat);
#endif

static ImFont* g_font_ui = nullptr;
static ImFont* g_font_title = nullptr;
static ImFont* g_font_mono = nullptr;
static ImGuiStyle g_user_scale_base_style{};
static bool g_user_scale_base_style_ready = false;
static float g_last_applied_user_scale = -1.0f;
static float g_ui_layout_scale = 1.0f;
static float g_platform_layout_scale = 1.0f;

using uam::constants::kAppCopyright;
using uam::constants::kAppDisplayName;
using uam::constants::kAppVersion;
using uam::constants::kDefaultFolderId;
using uam::constants::kDefaultFolderTitle;
using uam::constants::kSupportedGeminiVersion;

static void SortChatsByRecent(std::vector<ChatSession>& chats);
static std::vector<ChatSession> DeduplicateChatsById(std::vector<ChatSession> chats);
static void ClampWindowSettings(AppSettings& settings);
static std::string NormalizeThemeChoice(std::string value);
static void DrawSessionSidePane(AppState& app, ChatSession& chat);
static void SaveAndUpdateStatus(AppState& app, const ChatSession& chat, const std::string& success, const std::string& failure);
static void RefreshGeminiChatsDir(AppState& app);
static const ChatFolder* FindFolderById(const AppState& app, const std::string& folder_id);
static const ProviderProfile& ActiveProviderOrDefault(const AppState& app);
static bool ActiveProviderUsesGeminiHistory(const AppState& app);
static int FindChatIndexById(const AppState& app, const std::string& chat_id);
static ChatSession CreateNewChat(const std::string& folder_id);
static std::string CompactPreview(const std::string& text, std::size_t max_len);
static void MarkSelectedCliTerminalForLaunch(AppState& app);
static void SelectChatById(AppState& app, const std::string& chat_id);
static void SaveSettings(AppState& app);
static bool SaveChat(const AppState& app, const ChatSession& chat);
static void StartAsyncCommandTask(AsyncCommandTask& task, const std::string& command);
static bool TryConsumeAsyncCommandTaskOutput(AsyncCommandTask& task, std::string& output_out);
static std::optional<std::string> ExtractSemverVersion(const std::string& text);
static std::string BuildGeminiVersionCheckCommand();
static std::string BuildGeminiDowngradeCommand();
static void StartGeminiVersionCheck(AppState& app, const bool force);
static void StartGeminiDowngradeToSupported(AppState& app);
static void PollGeminiCompatibilityTasks(AppState& app);
static bool IsLocalDraftChatId(const std::string& chat_id);
static std::string ResolveResumeSessionIdForChat(const AppState& app, const ChatSession& chat);
static bool HasPendingCallForChat(const AppState& app, const std::string& chat_id);
static bool HasAnyPendingCall(const AppState& app);
static const PendingGeminiCall* FirstPendingCallForChat(const AppState& app, const std::string& chat_id);
static void NormalizeChatBranchMetadata(AppState& app);
static bool CreateBranchFromMessage(AppState& app, const std::string& source_chat_id, int message_index);
static void ConsumePendingBranchRequest(AppState& app);
static std::string BuildRagContextBlock(const std::vector<RagSnippet>& snippets);
static std::string BuildRagEnhancedPrompt(AppState& app, const ChatSession& chat, const std::string& prompt_text);
static RagIndexService::Config RagConfigFromSettings(const AppSettings& settings);
static void SyncRagServiceConfig(AppState& app);
static bool RefreshWorkspaceVcsSnapshot(AppState& app, const std::filesystem::path& workspace_root, bool force);
static void ShowVcsCommandOutput(AppState& app, const std::string& title, const VcsCommandResult& result);
static float PlatformUiSpacingScale();
static float ScaleUiLength(float value);
static ImVec2 ScaleUiSize(const ImVec2& value);
static void CaptureUiScaleBaseStyle();
static void ApplyUserUiScale(ImGuiIO& io, float user_scale_multiplier);

static std::string Trim(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

static std::string TimestampNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &tt);
#else
  localtime_r(&tt, &tm_snapshot);
#endif
  std::ostringstream out;
  out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

static std::string NewSessionId() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> hex_digit(0, 15);
  std::ostringstream id;
  id << "chat-" << epoch_ms << "-";
  for (int i = 0; i < 6; ++i) {
    id << std::hex << hex_digit(rng);
  }
  return id.str();
}


static std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

static bool WriteTextFile(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    return false;
  }
  out << content;
  return out.good();
}

#if defined(_WIN32)
static std::optional<fs::path> ResolveWindowsHomePath() {
  if (const char* user_profile = std::getenv("USERPROFILE")) {
    const std::string value = Trim(user_profile);
    if (!value.empty()) {
      return fs::path(value);
    }
  }
  if (const char* home_drive = std::getenv("HOMEDRIVE")) {
    if (const char* home_path = std::getenv("HOMEPATH")) {
      const std::string drive = Trim(home_drive);
      const std::string path = Trim(home_path);
      if (!drive.empty() && !path.empty()) {
        return fs::path(drive + path);
      }
    }
  }
  if (const char* home = std::getenv("HOME")) {
    const std::string value = Trim(home);
    if (!value.empty()) {
      return fs::path(value);
    }
  }
  return std::nullopt;
}
#endif

static fs::path ExpandLeadingTildePath(const std::string& raw_path) {
  const std::string trimmed = Trim(raw_path);
  if (trimmed.empty()) {
    return {};
  }
  if (trimmed[0] != '~') {
    return fs::path(trimmed);
  }
#if defined(_WIN32)
  if (const std::optional<fs::path> home = ResolveWindowsHomePath(); home.has_value()) {
    if (trimmed.size() == 1) {
      return home.value();
    }
    if (trimmed[1] == '\\' || trimmed[1] == '/') {
      return home.value() / trimmed.substr(2);
    }
  }
#else
  if (const char* home = std::getenv("HOME")) {
    if (trimmed.size() == 1) {
      return fs::path(home);
    }
    if (trimmed[1] == '/') {
      return fs::path(home) / trimmed.substr(2);
    }
  }
#endif
  return fs::path(trimmed);
}

static fs::path ResolveGeminiGlobalRootPath(const AppSettings& settings) {
  const fs::path candidate = ExpandLeadingTildePath(settings.gemini_global_root_path);
  if (!candidate.empty()) {
    return candidate;
  }
  return AppPaths::DefaultGeminiUniversalRootPath();
}

static fs::path ResolveWorkspaceRootPath(const AppState& app, const ChatSession& chat) {
  fs::path workspace_root;
  if (const ChatFolder* folder = FindFolderById(app, chat.folder_id); folder != nullptr) {
    workspace_root = ExpandLeadingTildePath(folder->directory);
  }
  if (workspace_root.empty()) {
    workspace_root = fs::current_path();
  }
  std::error_code ec;
  const fs::path absolute_root = fs::absolute(workspace_root, ec);
  return ec ? workspace_root : absolute_root;
}

static fs::path WorkspaceGeminiRootPath(const AppState& app, const ChatSession& chat) {
  return ResolveWorkspaceRootPath(app, chat) / ".gemini";
}

static fs::path WorkspaceGeminiTemplatePath(const AppState& app, const ChatSession& chat) {
  return WorkspaceGeminiRootPath(app, chat) / "gemini.md";
}

static RagIndexService::Config RagConfigFromSettings(const AppSettings& settings) {
  RagIndexService::Config config;
  config.enabled = settings.rag_enabled;
  config.top_k = std::clamp(settings.rag_top_k, 1, 20);
  config.max_snippet_chars = static_cast<std::size_t>(std::clamp(settings.rag_max_snippet_chars, 120, 4000));
  config.max_file_bytes = static_cast<std::size_t>(std::clamp(settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024));
  return config;
}

static void SyncRagServiceConfig(AppState& app) {
  app.rag_index_service.SetConfig(RagConfigFromSettings(app.settings));
  const fs::path model_folder = app.data_root / "models";
  std::error_code ec;
  fs::create_directories(model_folder, ec);
  app.rag_index_service.SetModelFolder(model_folder);
}

static void NormalizeChatBranchMetadata(AppState& app) {
  ChatBranching::Normalize(app.chats);
}

static std::string BuildRagContextBlock(const std::vector<RagSnippet>& snippets) {
  if (snippets.empty()) {
    return "";
  }
  std::ostringstream out;
  out << "Retrieved context:\n";
  for (std::size_t i = 0; i < snippets.size(); ++i) {
    const RagSnippet& snippet = snippets[i];
    out << (i + 1) << ". " << snippet.relative_path << ":" << snippet.start_line << "-" << snippet.end_line << "\n";
    out << snippet.text << "\n\n";
  }
  return out.str();
}

static std::string BuildRagEnhancedPrompt(AppState& app, const ChatSession& chat, const std::string& prompt_text) {
  if (!app.settings.rag_enabled || app.center_view_mode != CenterViewMode::Structured) {
    return prompt_text;
  }

  const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
  const std::string workspace_key = workspace_root.lexically_normal().generic_string();
  const RagRefreshResult refresh = app.rag_index_service.RefreshIndexIncremental(workspace_root);
  if (!refresh.ok) {
    app.rag_last_refresh_by_workspace[workspace_key] = refresh.error;
    return prompt_text;
  }
  app.rag_last_refresh_by_workspace[workspace_key] =
      "Indexed files: " + std::to_string(refresh.indexed_files) +
      ", updated: " + std::to_string(refresh.updated_files) +
      ", removed: " + std::to_string(refresh.removed_files);

  const std::vector<RagSnippet> snippets = app.rag_index_service.RetrieveTopK(workspace_root, prompt_text);
  if (snippets.empty()) {
    return prompt_text;
  }

  return BuildRagContextBlock(snippets) + "User prompt:\n" + prompt_text;
}

static bool RefreshWorkspaceVcsSnapshot(AppState& app, const std::filesystem::path& workspace_root, const bool force) {
  if (workspace_root.empty()) {
    return false;
  }
  const std::string workspace_key = workspace_root.lexically_normal().generic_string();
  if (!force && app.vcs_snapshot_loaded_workspaces.find(workspace_key) != app.vcs_snapshot_loaded_workspaces.end()) {
    return true;
  }

  VcsSnapshot snapshot;
  snapshot.working_copy_root = workspace_key;
  const VcsRepoType repo_type = VcsWorkspaceService::DetectRepo(workspace_root);
  if (repo_type == VcsRepoType::None) {
    snapshot.repo_type = VcsRepoType::None;
    app.vcs_snapshot_by_workspace[workspace_key] = std::move(snapshot);
    app.vcs_snapshot_loaded_workspaces.insert(workspace_key);
    return true;
  }

  VcsCommandResult command = VcsWorkspaceService::ReadSnapshot(workspace_root, snapshot);
  if (!command.ok) {
    snapshot.repo_type = VcsRepoType::Svn;
    app.vcs_snapshot_by_workspace[workspace_key] = std::move(snapshot);
    app.vcs_snapshot_loaded_workspaces.insert(workspace_key);
    if (force) {
      app.status_line = command.error.empty() ? "SVN snapshot refresh failed." : command.error;
    }
    return false;
  }

  app.vcs_snapshot_by_workspace[workspace_key] = std::move(snapshot);
  app.vcs_snapshot_loaded_workspaces.insert(workspace_key);
  return true;
}

static void ShowVcsCommandOutput(AppState& app, const std::string& title, const VcsCommandResult& result) {
  std::ostringstream out;
  if (!result.ok) {
    out << "[Command failed";
    if (result.timed_out) {
      out << " (timed out)";
    }
    if (result.exit_code >= 0) {
      out << ", exit code " << result.exit_code;
    }
    out << "]\n";
    if (!result.error.empty()) {
      out << result.error << "\n\n";
    }
  }
  out << result.output;
  app.vcs_output_popup_title = title;
  app.vcs_output_popup_content = out.str();
  app.open_vcs_output_popup = true;
}

static bool CreateBranchFromMessage(AppState& app, const std::string& source_chat_id, const int message_index) {
  const int source_index = FindChatIndexById(app, source_chat_id);
  if (source_index < 0) {
    app.status_line = "Branch source chat no longer exists.";
    return false;
  }
  const ChatSession source = app.chats[source_index];
  if (message_index < 0 || message_index >= static_cast<int>(source.messages.size())) {
    app.status_line = "Branch source message is no longer valid.";
    return false;
  }
  if (source.messages[message_index].role != MessageRole::User) {
    app.status_line = "Branching is currently supported for user messages only.";
    return false;
  }

  ChatSession branch = CreateNewChat(source.folder_id);
  branch.uses_native_session = false;
  branch.native_session_id.clear();
  branch.parent_chat_id = source.id;
  branch.branch_root_chat_id = source.branch_root_chat_id.empty() ? source.id : source.branch_root_chat_id;
  branch.branch_from_message_index = message_index;
  branch.template_override_id = source.template_override_id;
  branch.gemini_md_bootstrapped = source.gemini_md_bootstrapped;
  branch.linked_files = source.linked_files;
  branch.messages.assign(source.messages.begin(), source.messages.begin() + message_index + 1);
  branch.updated_at = TimestampNow();
  branch.title = "Branch: " + CompactPreview(source.messages[message_index].content, 40);
  if (Trim(branch.title).empty()) {
    branch.title = "Branch Chat";
  }

  app.chats.push_back(branch);
  NormalizeChatBranchMetadata(app);
  SortChatsByRecent(app.chats);
  SelectChatById(app, branch.id);
  SaveSettings(app);
  if (app.center_view_mode == CenterViewMode::CliConsole) {
    MarkSelectedCliTerminalForLaunch(app);
  }

  if (!SaveChat(app, branch)) {
    app.status_line = "Branch created in memory, but failed to save.";
    return false;
  }
  app.status_line = "Branch chat created.";
  return true;
}

static void ConsumePendingBranchRequest(AppState& app) {
  if (app.pending_branch_chat_id.empty()) {
    return;
  }
  const std::string chat_id = app.pending_branch_chat_id;
  const int message_index = app.pending_branch_message_index;
  app.pending_branch_chat_id.clear();
  app.pending_branch_message_index = -1;
  CreateBranchFromMessage(app, chat_id, message_index);
}

static std::string BuildShellCommandWithWorkingDirectory(const fs::path& working_directory, const std::string& command) {
#if defined(_WIN32)
  std::string escaped = "\"";
  for (const char ch : working_directory.string()) {
    if (ch == '"') {
      escaped += "\"\"";
    } else if (ch == '%') {
      escaped += "%%";
    } else if (ch == '\r' || ch == '\n') {
      escaped.push_back(' ');
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return "cd /d " + escaped + " && " + command;
#else
  std::string escaped = "'";
  for (const char ch : working_directory.string()) {
    if (ch == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('\'');
  return "cd " + escaped + " && " + command;
#endif
}

static bool EnsureWorkspaceGeminiLayout(const AppState& app, const ChatSession& chat, std::string* error_out = nullptr) {
  std::error_code ec;
  const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
  fs::create_directories(workspace_root, ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create workspace root '" + workspace_root.string() + "': " + ec.message();
    }
    return false;
  }

  const fs::path workspace_gemini = WorkspaceGeminiRootPath(app, chat);
  fs::create_directories(workspace_gemini, ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create local .gemini directory: " + ec.message();
    }
    return false;
  }
  fs::create_directories(workspace_gemini / "Lessons", ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create .gemini/Lessons: " + ec.message();
    }
    return false;
  }
  fs::create_directories(workspace_gemini / "Failures", ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create .gemini/Failures: " + ec.message();
    }
    return false;
  }
  fs::create_directories(workspace_gemini / "auto-test", ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create .gemini/auto-test: " + ec.message();
    }
    return false;
  }
  return true;
}

static void MarkTemplateCatalogDirty(AppState& app) {
  app.template_catalog_dirty = true;
}

static bool RefreshTemplateCatalog(AppState& app, const bool force = false) {
  if (!force && !app.template_catalog_dirty) {
    return true;
  }
  const fs::path global_root = ResolveGeminiGlobalRootPath(app.settings);
  std::string error;
  if (!GeminiTemplateCatalog::EnsureCatalogPath(global_root, &error)) {
    app.template_catalog.clear();
    app.template_catalog_dirty = false;
    if (!error.empty()) {
      app.status_line = error;
    }
    return false;
  }
  app.template_catalog = GeminiTemplateCatalog::List(global_root);
  app.template_catalog_dirty = false;
  return true;
}

static const TemplateCatalogEntry* FindTemplateEntryById(const AppState& app, const std::string& template_id) {
  if (template_id.empty()) {
    return nullptr;
  }
  for (const TemplateCatalogEntry& entry : app.template_catalog) {
    if (entry.id == template_id) {
      return &entry;
    }
  }
  return nullptr;
}

static std::string TemplateLabelOrFallback(const AppState& app, const std::string& template_id) {
  const TemplateCatalogEntry* entry = FindTemplateEntryById(app, template_id);
  if (entry != nullptr) {
    return entry->display_name;
  }
  return template_id.empty() ? "None" : ("Missing: " + template_id);
}

#include "common/runtime/json_runtime.h"

static std::string ExecuteCommandCaptureOutput(const std::string& command) {
  const std::string full_command = command + " 2>&1";
#if defined(_WIN32)
  FILE* pipe = _popen(full_command.c_str(), "r");
#else
  FILE* pipe = popen(full_command.c_str(), "r");
#endif
  if (pipe == nullptr) {
    std::ostringstream message;
    message << "Failed to launch Gemini CLI command";
    if (errno != 0) {
      message << " (" << std::strerror(errno) << ")";
    }
    message << ".";
    return message.str();
  }

  std::array<char, 4096> buffer{};
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#if defined(_WIN32)
  const int close_code = _pclose(pipe);
#else
  const int close_code = pclose(pipe);
#endif
  int exit_code = close_code;
#if defined(__unix__) || defined(__APPLE__)
  if (WIFEXITED(close_code)) {
    exit_code = WEXITSTATUS(close_code);
  }
#endif

  if (output.empty()) {
    output = "(Gemini CLI returned no output.)";
  }
  if (exit_code != 0) {
    output += "\n\n[Gemini CLI exited with code " + std::to_string(exit_code) + "]";
  }

  return output;
}

static void StartAsyncCommandTask(AsyncCommandTask& task, const std::string& command) {
  task.running = true;
  task.command_preview = command;
  task.completed = std::make_shared<std::atomic<bool>>(false);
  task.output = std::make_shared<std::string>();
  std::shared_ptr<std::atomic<bool>> completed = task.completed;
  std::shared_ptr<std::string> output = task.output;
  std::thread([command, completed, output]() {
    *output = ExecuteCommandCaptureOutput(command);
    completed->store(true, std::memory_order_release);
  }).detach();
}

static bool TryConsumeAsyncCommandTaskOutput(AsyncCommandTask& task, std::string& output_out) {
  if (!task.running) {
    return false;
  }
  if (task.completed == nullptr || task.output == nullptr) {
    task.running = false;
    task.command_preview.clear();
    task.completed.reset();
    task.output.reset();
    output_out.clear();
    return true;
  }
  if (!task.completed->load(std::memory_order_acquire)) {
    return false;
  }
  output_out = *task.output;
  task.running = false;
  task.command_preview.clear();
  task.completed.reset();
  task.output.reset();
  return true;
}

static std::optional<std::string> ExtractSemverVersion(const std::string& text) {
  static const std::regex semver_pattern(R"((\d+)\.(\d+)\.(\d+))");
  std::smatch match;
  if (std::regex_search(text, match, semver_pattern) && !match.str(0).empty()) {
    return match.str(0);
  }
  return std::nullopt;
}

static std::string BuildGeminiVersionCheckCommand() {
  return "gemini --version";
}

static std::string BuildGeminiDowngradeCommand() {
#if defined(__APPLE__)
  return "brew install gemini-cli@0.30.0";
#elif defined(_WIN32)
  return "npm install -g @google/gemini-cli@0.30.0";
#else
  return "npm install -g @google/gemini-cli@0.30.0";
#endif
}

static void StartGeminiVersionCheck(AppState& app, const bool force) {
  if (app.gemini_version_check_task.running) {
    return;
  }
  if (!force && app.gemini_version_checked) {
    return;
  }
  StartAsyncCommandTask(app.gemini_version_check_task, BuildGeminiVersionCheckCommand());
  app.gemini_version_message = "Checking installed Gemini version...";
}

static void StartGeminiDowngradeToSupported(AppState& app) {
  if (app.gemini_downgrade_task.running) {
    return;
  }
  const std::string command = BuildGeminiDowngradeCommand();
  StartAsyncCommandTask(app.gemini_downgrade_task, command);
  app.gemini_downgrade_output.clear();
  app.status_line = "Running Gemini downgrade command...";
}

static bool OutputContainsNonZeroExit(const std::string& output) {
  return output.find("[Gemini CLI exited with code ") != std::string::npos;
}

static void PollGeminiCompatibilityTasks(AppState& app) {
  std::string output;
  if (TryConsumeAsyncCommandTaskOutput(app.gemini_version_check_task, output)) {
    app.gemini_version_checked = true;
    app.gemini_version_raw_output = output;
    app.gemini_installed_version.clear();
    app.gemini_version_supported = false;

    const std::optional<std::string> parsed = ExtractSemverVersion(output);
    if (parsed.has_value()) {
      app.gemini_installed_version = parsed.value();
      app.gemini_version_supported = (app.gemini_installed_version == kSupportedGeminiVersion);
      if (app.gemini_version_supported) {
        app.gemini_version_message = "Gemini version is supported.";
      } else {
        app.gemini_version_message = "Installed Gemini version is unsupported for this app.";
      }
    } else {
      const std::string lowered = Trim(output);
      if (lowered.find("not found") != std::string::npos || lowered.find("not recognized") != std::string::npos) {
        app.gemini_version_message = "Gemini CLI is not installed or not on PATH.";
      } else {
        app.gemini_version_message = "Could not parse Gemini version output.";
      }
    }
  }

  if (TryConsumeAsyncCommandTaskOutput(app.gemini_downgrade_task, output)) {
    app.gemini_downgrade_output = output;
    if (OutputContainsNonZeroExit(output)) {
      app.status_line = "Gemini downgrade command failed. Review output in Settings.";
      app.gemini_version_message = "Downgrade command failed.";
    } else {
      app.status_line = "Gemini downgrade completed. Re-checking installed version.";
      StartGeminiVersionCheck(app, true);
    }
  }
}

static fs::path SettingsFilePath(const AppState& app) {
  return AppPaths::SettingsFilePath(app.data_root);
}

static fs::path ChatsRootPath(const AppState& app) {
  return AppPaths::ChatsRootPath(app.data_root);
}

static fs::path ChatPath(const AppState& app, const ChatSession& chat) {
  return AppPaths::ChatPath(app.data_root, chat.id);
}

static fs::path DefaultDataRootPath() {
  return AppPaths::DefaultDataRootPath();
}

static fs::path TempFallbackDataRootPath() {
  std::error_code ec;
  const fs::path temp = fs::temp_directory_path(ec);
  if (!ec) {
    return temp / "universal_agent_manager_data";
  }
  return fs::path("data");
}

static bool EnsureDataRootLayout(const fs::path& data_root, std::string* error_out) {
  std::error_code ec;
  fs::create_directories(data_root, ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create data root '" + data_root.string() + "': " + ec.message();
    }
    return false;
  }
  fs::create_directories(data_root / "chats", ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create chats dir '" + (data_root / "chats").string() + "': " + ec.message();
    }
    return false;
  }
  return true;
}

static std::optional<fs::path> ResolveGeminiProjectTmpDir(const fs::path& project_root) {
  return AppPaths::ResolveGeminiProjectTmpDir(project_root);
}

#if defined(_WIN32)
static constexpr uintmax_t kWindowsNativeSessionMaxFileBytes = 12ULL * 1024ULL * 1024ULL;
static constexpr std::size_t kWindowsNativeSessionMaxMessages = 12000;
#endif

static std::optional<ChatSession> ParseGeminiSessionFile(const fs::path& file_path, const ProviderProfile& provider) {
  const std::string file_text = ReadTextFile(file_path);
  if (file_text.empty()) {
    return std::nullopt;
  }
  const std::optional<JsonValue> root_opt = ParseJson(file_text);
  if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
    return std::nullopt;
  }
  const JsonValue& root = root_opt.value();
  const std::string session_id = JsonStringOrEmpty(root.Find("sessionId"));
  if (session_id.empty()) {
    return std::nullopt;
  }

  ChatSession chat;
  chat.id = session_id;
  chat.native_session_id = session_id;
  chat.uses_native_session = true;
  chat.parent_chat_id.clear();
  chat.branch_root_chat_id = session_id;
  chat.branch_from_message_index = -1;
  chat.created_at = JsonStringOrEmpty(root.Find("startTime"));
  chat.updated_at = JsonStringOrEmpty(root.Find("lastUpdated"));
  if (chat.created_at.empty()) {
    chat.created_at = TimestampNow();
  }
  if (chat.updated_at.empty()) {
    chat.updated_at = chat.created_at;
  }

  const JsonValue* messages = root.Find("messages");
  if (messages != nullptr && messages->type == JsonValue::Type::Array) {
    for (const JsonValue& raw_message : messages->array_value) {
      if (raw_message.type != JsonValue::Type::Object) {
        continue;
      }
      const std::string type = JsonStringOrEmpty(raw_message.Find("type"));
      const std::string timestamp = JsonStringOrEmpty(raw_message.Find("timestamp"));
      const std::string content = Trim(ExtractGeminiContentText(raw_message.Find("content")));
      if (content.empty()) {
        continue;
      }

      Message message;
      message.role = ProviderRuntime::RoleFromNativeType(provider, type);
      message.content = content;
      message.created_at = timestamp.empty() ? chat.updated_at : timestamp;
      chat.messages.push_back(std::move(message));
#if defined(_WIN32)
      // Windows-only guardrail: if a native session JSON explodes in size, avoid
      // unbounded message expansion in the UI thread path. If macOS starts showing
      // the same symptom, we can make this limit universal.
      if (chat.messages.size() >= kWindowsNativeSessionMaxMessages) {
        break;
      }
#endif
    }
  }

  chat.title = "Session " + chat.created_at;
  for (const Message& message : chat.messages) {
    if (message.role == MessageRole::User) {
      std::string title = Trim(message.content);
      if (title.size() > 48) {
        title = title.substr(0, 45) + "...";
      }
      if (!title.empty()) {
        chat.title = title;
      }
      break;
    }
  }
  return chat;
}

static std::vector<ChatSession> LoadNativeGeminiChats(const fs::path& chats_dir, const ProviderProfile& provider) {
  std::vector<ChatSession> chats;
  if (chats_dir.empty() || !fs::exists(chats_dir) || !fs::is_directory(chats_dir)) {
    return chats;
  }

  std::error_code ec;
  for (const auto& item : fs::directory_iterator(chats_dir, ec)) {
    if (ec || !item.is_regular_file() || item.path().extension() != ".json") {
      continue;
    }
#if defined(_WIN32)
    // Windows-only guardrail to keep a single pathological native session file
    // from stalling this port. If macOS begins exhibiting this issue, promote this
    // check to all platforms.
    std::error_code size_ec;
    const uintmax_t file_size = fs::file_size(item.path(), size_ec);
    if (!size_ec && file_size > kWindowsNativeSessionMaxFileBytes) {
      continue;
    }
#endif
    const auto parsed = ParseGeminiSessionFile(item.path(), provider);
    if (parsed.has_value()) {
      chats.push_back(parsed.value());
    }
  }

  return DeduplicateChatsById(std::move(chats));
}

#if defined(_WIN32)
static bool StartAsyncNativeChatLoad(AppState& app) {
  if (!ActiveProviderUsesGeminiHistory(app) || app.native_chat_load_task.running) {
    return false;
  }
  RefreshGeminiChatsDir(app);
  const fs::path chats_dir = app.gemini_chats_dir;
  const ProviderProfile provider = ActiveProviderOrDefault(app);

  app.native_chat_load_task.running = true;
  app.native_chat_load_task.completed = std::make_shared<std::atomic<bool>>(false);
  app.native_chat_load_task.chats = std::make_shared<std::vector<ChatSession>>();
  app.native_chat_load_task.error = std::make_shared<std::string>();
  std::shared_ptr<std::atomic<bool>> completed = app.native_chat_load_task.completed;
  std::shared_ptr<std::vector<ChatSession>> chats = app.native_chat_load_task.chats;
  std::shared_ptr<std::string> error = app.native_chat_load_task.error;

  std::thread([chats_dir, provider, completed, chats, error]() {
    try {
      *chats = LoadNativeGeminiChats(chats_dir, provider);
    } catch (const std::exception& ex) {
      *error = ex.what();
    } catch (...) {
      *error = "Unknown native chat load failure.";
    }
    completed->store(true, std::memory_order_release);
  }).detach();
  return true;
}

static bool TryConsumeAsyncNativeChatLoad(AppState& app,
                                          std::vector<ChatSession>& chats_out,
                                          std::string& error_out) {
  if (!app.native_chat_load_task.running) {
    return false;
  }
  if (app.native_chat_load_task.completed == nullptr ||
      app.native_chat_load_task.chats == nullptr ||
      app.native_chat_load_task.error == nullptr) {
    app.native_chat_load_task.running = false;
    app.native_chat_load_task.completed.reset();
    app.native_chat_load_task.chats.reset();
    app.native_chat_load_task.error.reset();
    chats_out.clear();
    error_out.clear();
    return true;
  }
  if (!app.native_chat_load_task.completed->load(std::memory_order_acquire)) {
    return false;
  }

  chats_out = *app.native_chat_load_task.chats;
  error_out = *app.native_chat_load_task.error;
  app.native_chat_load_task.running = false;
  app.native_chat_load_task.completed.reset();
  app.native_chat_load_task.chats.reset();
  app.native_chat_load_task.error.reset();
  return true;
}
#endif

static std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& chats) {
  std::vector<std::string> ids;
  ids.reserve(chats.size());
  for (const ChatSession& chat : chats) {
    if (chat.uses_native_session && !chat.native_session_id.empty()) {
      ids.push_back(chat.native_session_id);
    }
  }
  return ids;
}

static std::string NewFolderId() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::ostringstream id;
  id << "folder-" << epoch_ms;
  return id.str();
}

static int FindFolderIndexById(const AppState& app, const std::string& folder_id) {
  for (int i = 0; i < static_cast<int>(app.folders.size()); ++i) {
    if (app.folders[i].id == folder_id) {
      return i;
    }
  }
  return -1;
}

static ChatFolder* FindFolderById(AppState& app, const std::string& folder_id) {
  const int idx = FindFolderIndexById(app, folder_id);
  return (idx >= 0) ? &app.folders[idx] : nullptr;
}

static const ChatFolder* FindFolderById(const AppState& app, const std::string& folder_id) {
  const int idx = FindFolderIndexById(app, folder_id);
  return (idx >= 0) ? &app.folders[idx] : nullptr;
}

static int CountChatsInFolder(const AppState& app, const std::string& folder_id);

static void EnsureDefaultFolder(AppState& app) {
  if (FindFolderIndexById(app, kDefaultFolderId) >= 0) {
    return;
  }
  ChatFolder folder;
  folder.id = kDefaultFolderId;
  folder.title = kDefaultFolderTitle;
  folder.directory = fs::current_path().string();
  folder.collapsed = false;
  app.folders.push_back(std::move(folder));
}

static void EnsureNewChatFolderSelection(AppState& app) {
  EnsureDefaultFolder(app);
  if (app.new_chat_folder_id.empty() || FindFolderById(app, app.new_chat_folder_id) == nullptr) {
    app.new_chat_folder_id = kDefaultFolderId;
  }
}

static void NormalizeChatFolderAssignments(AppState& app) {
  EnsureDefaultFolder(app);
  for (ChatSession& chat : app.chats) {
    if (chat.folder_id.empty() || FindFolderById(app, chat.folder_id) == nullptr) {
      chat.folder_id = kDefaultFolderId;
    }
  }
  bool any_expanded_with_chats = false;
  for (const ChatFolder& folder : app.folders) {
    if (!folder.collapsed && CountChatsInFolder(app, folder.id) > 0) {
      any_expanded_with_chats = true;
      break;
    }
  }
  if (!any_expanded_with_chats) {
    for (ChatFolder& folder : app.folders) {
      if (CountChatsInFolder(app, folder.id) > 0) {
        folder.collapsed = false;
      }
    }
  }
  EnsureNewChatFolderSelection(app);
}

static void SaveFolders(const AppState& app) {
  ChatFolderStore::Save(app.data_root, app.folders);
}

static void SaveProviders(const AppState& app) {
  ProviderProfileStore::Save(app.data_root, app.provider_profiles);
}

static fs::path ProviderProfileFilePath(const AppState& app) {
  return app.data_root / "providers.txt";
}

static fs::path FrontendActionFilePath(const AppState& app) {
  return app.data_root / "frontend_actions.txt";
}

static ProviderProfile* ActiveProvider(AppState& app) {
  ProviderProfile* found = ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
  if (found != nullptr) {
    return found;
  }
  ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
  app.settings.active_provider_id = "gemini";
  return ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
}

static const ProviderProfile* ActiveProvider(const AppState& app) {
  return ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
}

static const ProviderProfile& ActiveProviderOrDefault(const AppState& app) {
  const ProviderProfile* profile = ActiveProvider(app);
  if (profile != nullptr) {
    return *profile;
  }
  static const ProviderProfile fallback = ProviderProfileStore::DefaultGeminiProfile();
  return fallback;
}

static bool ActiveProviderUsesGeminiHistory(const AppState& app) {
  const ProviderProfile* profile = ActiveProvider(app);
  return profile != nullptr && ProviderRuntime::SupportsGeminiJsonHistory(*profile);
}

static const uam::FrontendAction* FindFrontendAction(const AppState& app, const std::string& key) {
  return uam::FindAction(app.frontend_actions, key);
}

static bool FrontendActionVisible(const AppState& app, const std::string& key, const bool fallback_visible = true) {
  const uam::FrontendAction* action = FindFrontendAction(app, key);
  return (action == nullptr) ? fallback_visible : action->visible;
}

static std::string FrontendActionLabel(const AppState& app, const std::string& key, const std::string& fallback_label) {
  const uam::FrontendAction* action = FindFrontendAction(app, key);
  if (action == nullptr || Trim(action->label).empty()) {
    return fallback_label;
  }
  return action->label;
}

static void LoadFrontendActions(AppState& app) {
  std::string error;
  if (!uam::LoadFrontendActionMap(FrontendActionFilePath(app), app.frontend_actions, &error)) {
    app.frontend_actions = uam::DefaultFrontendActionMap();
    if (!uam::SaveFrontendActionMap(FrontendActionFilePath(app), app.frontend_actions, &error) && !error.empty()) {
      app.status_line = "Frontend action map reset, but saving failed: " + error;
    } else if (!error.empty()) {
      app.status_line = "Frontend action map was invalid and has been reset.";
    }
    return;
  }
  uam::NormalizeFrontendActionMap(app.frontend_actions);
}

static std::string FolderForNewChat(const AppState& app) {
  if (!app.new_chat_folder_id.empty()) {
    return app.new_chat_folder_id;
  }
  return kDefaultFolderId;
}

static int CountChatsInFolder(const AppState& app, const std::string& folder_id) {
  int count = 0;
  for (const ChatSession& chat : app.chats) {
    if (chat.folder_id == folder_id) {
      ++count;
    }
  }
  return count;
}

static std::string FolderTitleOrFallback(const ChatFolder& folder) {
  const std::string trimmed = Trim(folder.title);
  return trimmed.empty() ? "Untitled Folder" : trimmed;
}

static int FindChatIndexById(const AppState& app, const std::string& chat_id) {
  for (int i = 0; i < static_cast<int>(app.chats.size()); ++i) {
    if (app.chats[i].id == chat_id) {
      return i;
    }
  }
  return -1;
}

static ChatSession* SelectedChat(AppState& app) {
  if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size())) {
    return nullptr;
  }
  return &app.chats[app.selected_chat_index];
}

static const ChatSession* SelectedChat(const AppState& app) {
  if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size())) {
    return nullptr;
  }
  return &app.chats[app.selected_chat_index];
}

static void SortChatsByRecent(std::vector<ChatSession>& chats) {
  std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) {
    return a.updated_at > b.updated_at;
  });
}

static bool ShouldReplaceChatForDuplicateId(const ChatSession& candidate, const ChatSession& existing) {
  if (candidate.uses_native_session != existing.uses_native_session) {
    return candidate.uses_native_session && !existing.uses_native_session;
  }
  if (candidate.messages.size() != existing.messages.size()) {
    return candidate.messages.size() > existing.messages.size();
  }
  if (candidate.updated_at != existing.updated_at) {
    return candidate.updated_at > existing.updated_at;
  }
  if (candidate.created_at != existing.created_at) {
    return candidate.created_at > existing.created_at;
  }
  if (candidate.linked_files.size() != existing.linked_files.size()) {
    return candidate.linked_files.size() > existing.linked_files.size();
  }
  if (candidate.template_override_id != existing.template_override_id) {
    return !candidate.template_override_id.empty();
  }
  if (candidate.parent_chat_id != existing.parent_chat_id) {
    return !candidate.parent_chat_id.empty();
  }
  if (candidate.branch_root_chat_id != existing.branch_root_chat_id) {
    return !candidate.branch_root_chat_id.empty();
  }
  if (candidate.branch_from_message_index != existing.branch_from_message_index) {
    return candidate.branch_from_message_index > existing.branch_from_message_index;
  }
  return false;
}

static std::vector<ChatSession> DeduplicateChatsById(std::vector<ChatSession> chats) {
  std::vector<ChatSession> deduped;
  deduped.reserve(chats.size());
  std::unordered_map<std::string, std::size_t> index_by_id;

  for (ChatSession& chat : chats) {
    chat.id = Trim(chat.id);
    if (chat.id.empty()) {
      continue;
    }
    const auto it = index_by_id.find(chat.id);
    if (it == index_by_id.end()) {
      index_by_id.emplace(chat.id, deduped.size());
      deduped.push_back(std::move(chat));
      continue;
    }
    ChatSession& existing = deduped[it->second];
    if (ShouldReplaceChatForDuplicateId(chat, existing)) {
      existing = std::move(chat);
    }
  }

  SortChatsByRecent(deduped);
  return deduped;
}

static void RefreshRememberedSelection(AppState& app) {
  if (!app.settings.remember_last_chat) {
    app.settings.last_selected_chat_id.clear();
    return;
  }
  const ChatSession* selected = SelectedChat(app);
  app.settings.last_selected_chat_id = (selected != nullptr) ? selected->id : "";
}

static void SaveSettings(AppState& app) {
  app.settings.ui_theme = NormalizeThemeChoice(app.settings.ui_theme);
  app.settings.rag_top_k = std::clamp(app.settings.rag_top_k, 1, 20);
  app.settings.rag_max_snippet_chars = std::clamp(app.settings.rag_max_snippet_chars, 120, 4000);
  app.settings.rag_max_file_bytes = std::clamp(app.settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024);
  ClampWindowSettings(app.settings);
  SyncRagServiceConfig(app);
  RefreshRememberedSelection(app);
  SettingsStore::Save(SettingsFilePath(app), app.settings, app.center_view_mode);
}

static void LoadSettings(AppState& app) {
  SettingsStore::Load(SettingsFilePath(app), app.settings, app.center_view_mode);
  if (Trim(app.settings.gemini_global_root_path).empty()) {
    app.settings.gemini_global_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
  }
  SyncRagServiceConfig(app);
}

static bool SaveChat(const AppState& app, const ChatSession& chat) {
  return ChatRepository::SaveChat(app.data_root, chat);
}

static std::vector<ChatSession> LoadChats(const AppState& app) {
  return ChatRepository::LoadLocalChats(app.data_root);
}

static ChatSession CreateNewChat(const std::string& folder_id) {
  ChatSession chat;
  chat.id = NewSessionId();
  chat.parent_chat_id.clear();
  chat.branch_root_chat_id = chat.id;
  chat.branch_from_message_index = -1;
  chat.folder_id = folder_id;
  chat.created_at = TimestampNow();
  chat.updated_at = chat.created_at;
  chat.title = "Chat " + chat.created_at;
  return chat;
}

static bool IsLocalDraftChatId(const std::string& chat_id) {
  return chat_id.rfind("chat-", 0) == 0;
}

static std::string ResolveResumeSessionIdForChat(const AppState& app, const ChatSession& chat) {
  if (!ActiveProviderUsesGeminiHistory(app)) {
    return "";
  }
  if (chat.uses_native_session) {
    if (!chat.native_session_id.empty()) {
      return chat.native_session_id;
    }
    if (!chat.id.empty()) {
      return chat.id;
    }
    return "";
  }

  // Legacy compatibility: older local snapshots of native chats may not persist native flags.
  if (!chat.messages.empty() && !chat.id.empty() && !IsLocalDraftChatId(chat.id)) {
    return chat.id;
  }
  return "";
}

static void SelectChatById(AppState& app, const std::string& chat_id) {
  const ChatSession* previously_selected = SelectedChat(app);
  const std::string previous_id = (previously_selected != nullptr) ? previously_selected->id : "";
  app.selected_chat_index = FindChatIndexById(app, chat_id);
  if (app.selected_chat_index >= 0) {
    app.chats_with_unseen_updates.erase(app.chats[app.selected_chat_index].id);
  }
  if (previous_id != chat_id) {
    app.composer_text.clear();
  }
  RefreshRememberedSelection(app);
}

static void AddMessage(ChatSession& chat, const MessageRole role, const std::string& text) {
  Message message;
  message.role = role;
  message.content = text;
  message.created_at = TimestampNow();
  chat.messages.push_back(std::move(message));
  chat.updated_at = TimestampNow();

  if (chat.messages.size() == 1 && role == MessageRole::User) {
    std::string maybe_title = Trim(text);
    if (maybe_title.size() > 48) {
      maybe_title = maybe_title.substr(0, 45) + "...";
    }
    if (!maybe_title.empty()) {
      chat.title = maybe_title;
    }
  }
}

static std::string BuildProviderPrompt(const ProviderProfile& provider,
                                       const std::string& user_prompt,
                                       const std::vector<std::string>& files) {
  return ProviderRuntime::BuildPrompt(provider, user_prompt, files);
}

static std::string BuildProviderCommand(
    const ProviderProfile& provider,
    const AppSettings& settings,
    const std::string& prompt,
    const std::vector<std::string>& files,
    const std::string& resume_session_id) {
  return ProviderRuntime::BuildCommand(provider, settings, prompt, files, resume_session_id);
}

enum class TemplatePreflightOutcome {
  ReadyWithTemplate,
  ReadyWithoutTemplate,
  BlockingError
};

static TemplatePreflightOutcome PreflightWorkspaceTemplateForChat(AppState& app,
                                                                  const ChatSession& chat,
                                                                  std::string* status_out = nullptr) {
  if (!EnsureWorkspaceGeminiLayout(app, chat, status_out)) {
    return TemplatePreflightOutcome::BlockingError;
  }
  RefreshTemplateCatalog(app);

  std::string effective_template_id = chat.template_override_id;
  if (effective_template_id.empty()) {
    effective_template_id = app.settings.default_gemini_template_id;
  }
  if (effective_template_id.empty()) {
    if (status_out != nullptr) {
      *status_out = "No Gemini template selected. Set a default in Templates.";
    }
    return TemplatePreflightOutcome::ReadyWithoutTemplate;
  }

  const TemplateCatalogEntry* entry = FindTemplateEntryById(app, effective_template_id);
  if (entry == nullptr) {
    if (status_out != nullptr) {
      *status_out = "Selected template is missing: " + effective_template_id + ". Choose one in Templates.";
    }
    app.open_template_manager_popup = true;
    return TemplatePreflightOutcome::BlockingError;
  }

  std::error_code ec;
  fs::copy_file(entry->absolute_path, WorkspaceGeminiTemplatePath(app, chat), fs::copy_options::overwrite_existing, ec);
  if (ec) {
    if (status_out != nullptr) {
      *status_out = "Failed to materialize .gemini/gemini.md: " + ec.message();
    }
    return TemplatePreflightOutcome::BlockingError;
  }

  return TemplatePreflightOutcome::ReadyWithTemplate;
}

static bool QueueGeminiPromptForChat(AppState& app,
                                     ChatSession& chat,
                                     const std::string& prompt,
                                     const bool template_control_message = false) {
  if (HasPendingCallForChat(app, chat.id)) {
    app.status_line = "Gemini command already running for this chat.";
    return false;
  }
  const std::string prompt_text = Trim(prompt);
  if (prompt_text.empty()) {
    app.status_line = "Prompt is empty.";
    return false;
  }

  std::string template_status;
  const TemplatePreflightOutcome template_outcome = PreflightWorkspaceTemplateForChat(app, chat, &template_status);
  if (template_outcome == TemplatePreflightOutcome::BlockingError) {
    app.status_line = template_status.empty() ? "Gemini template preflight failed." : template_status;
    return false;
  }

  const bool should_bootstrap_template =
      !template_control_message &&
      !chat.gemini_md_bootstrapped &&
      chat.messages.empty() &&
      template_outcome == TemplatePreflightOutcome::ReadyWithTemplate;
  std::string runtime_prompt = prompt_text;
  if (!template_control_message) {
    runtime_prompt = BuildRagEnhancedPrompt(app, chat, prompt_text);
  }
  if (should_bootstrap_template) {
    runtime_prompt = "@.gemini/gemini.md\n\n" + runtime_prompt;
  }

  AddMessage(chat, MessageRole::User, prompt_text);
  SaveAndUpdateStatus(app, chat, "Prompt queued for provider runtime.", "Saved message locally, but failed to persist chat data.");

  const ProviderProfile& provider = ActiveProviderOrDefault(app);
  std::vector<ChatSession> native_before;
  if (ActiveProviderUsesGeminiHistory(app)) {
    RefreshGeminiChatsDir(app);
    native_before = LoadNativeGeminiChats(app.gemini_chats_dir, provider);
  }
  const std::string resume_session_id = ResolveResumeSessionIdForChat(app, chat);
  const std::string provider_prompt = BuildProviderPrompt(provider, runtime_prompt, chat.linked_files);
  const std::string provider_command = BuildProviderCommand(provider, app.settings, provider_prompt, chat.linked_files, resume_session_id);
  const std::string command = BuildShellCommandWithWorkingDirectory(ResolveWorkspaceRootPath(app, chat), provider_command);
  const std::string chat_id = chat.id;

  PendingGeminiCall pending;
  pending.chat_id = chat_id;
  pending.resume_session_id = resume_session_id;
  pending.session_ids_before = SessionIdsFromChats(native_before);
  pending.command_preview = command;
  pending.completed = std::make_shared<std::atomic<bool>>(false);
  pending.output = std::make_shared<std::string>();
  {
    std::shared_ptr<std::atomic<bool>> completed = pending.completed;
    std::shared_ptr<std::string> output = pending.output;
    std::thread([command, completed, output]() {
      *output = ExecuteCommandCaptureOutput(command);
      completed->store(true, std::memory_order_release);
    }).detach();
  }
  app.pending_calls.push_back(std::move(pending));

  if (should_bootstrap_template) {
    chat.gemini_md_bootstrapped = true;
    SaveChat(app, chat);
  }

  if (template_control_message) {
    app.status_line = "Template updated. Sent @.gemini/gemini.md to Gemini.";
  } else if (template_outcome == TemplatePreflightOutcome::ReadyWithoutTemplate && !template_status.empty()) {
    app.status_line = template_status;
  }
  app.scroll_to_bottom = true;
  return true;
}

static void SaveAndUpdateStatus(AppState& app, const ChatSession& chat, const std::string& success, const std::string& failure) {
  if (SaveChat(app, chat)) {
    app.status_line = success;
  } else {
    app.status_line = failure;
  }
}

static void ApplyLocalOverrides(AppState& app, std::vector<ChatSession>& native_chats) {
  native_chats = DeduplicateChatsById(std::move(native_chats));
  std::vector<ChatSession> local_chats = DeduplicateChatsById(LoadChats(app));
  std::unordered_map<std::string, const ChatSession*> local_map;
  for (const ChatSession& local : local_chats) {
    local_map[local.id] = &local;
  }

  std::unordered_set<std::string> native_ids;
  for (ChatSession& native : native_chats) {
    native_ids.insert(native.id);
    const auto it = local_map.find(native.id);
    if (it == local_map.end()) {
      continue;
    }
    const ChatSession& local = *it->second;
    if (!Trim(local.title).empty()) {
      native.title = local.title;
    }
    if (!local.linked_files.empty()) {
      native.linked_files = local.linked_files;
    }
    if (!local.template_override_id.empty()) {
      native.template_override_id = local.template_override_id;
    }
    if (!local.parent_chat_id.empty()) {
      native.parent_chat_id = local.parent_chat_id;
    }
    if (!local.branch_root_chat_id.empty()) {
      native.branch_root_chat_id = local.branch_root_chat_id;
    }
    if (local.branch_from_message_index >= 0) {
      native.branch_from_message_index = local.branch_from_message_index;
    }
    if (!local.native_session_id.empty()) {
      native.native_session_id = local.native_session_id;
      native.uses_native_session = true;
    } else if (local.uses_native_session && native.native_session_id.empty()) {
      native.native_session_id = native.id;
      native.uses_native_session = true;
    }
    if (local.gemini_md_bootstrapped) {
      native.gemini_md_bootstrapped = true;
    }
    if (!local.folder_id.empty()) {
      native.folder_id = local.folder_id;
    }
    if (native.created_at.empty() && !local.created_at.empty()) {
      native.created_at = local.created_at;
    }
    if (native.updated_at.empty() && !local.updated_at.empty()) {
      native.updated_at = local.updated_at;
    }
  }

  std::vector<ChatSession> merged = native_chats;
  for (const ChatSession& chat : local_chats) {
    if (native_ids.find(chat.id) != native_ids.end()) {
      continue;
    }
    if (chat.uses_native_session || !chat.native_session_id.empty()) {
      continue;
    }
    // In Gemini-history mode, only explicit in-app drafts (chat-*) should appear as local-only chats.
    if (!IsLocalDraftChatId(chat.id)) {
      continue;
    }
    merged.push_back(chat);
  }

  app.chats = DeduplicateChatsById(std::move(merged));
  NormalizeChatBranchMetadata(app);
  NormalizeChatFolderAssignments(app);
}

static void RefreshGeminiChatsDir(AppState& app) {
  const auto tmp_dir = ResolveGeminiProjectTmpDir(fs::current_path());
  if (tmp_dir.has_value()) {
    app.gemini_chats_dir = tmp_dir.value() / "chats";
    std::error_code ec;
    fs::create_directories(app.gemini_chats_dir, ec);
  } else {
    app.gemini_chats_dir.clear();
  }
}

static std::vector<std::string> CollectNewSessionIds(
    const std::vector<ChatSession>& loaded_chats,
    const std::vector<std::string>& existing_ids) {
  std::unordered_set<std::string> seen(existing_ids.begin(), existing_ids.end());
  std::vector<std::string> discovered;
  for (const ChatSession& chat : loaded_chats) {
    if (!chat.native_session_id.empty() && seen.find(chat.native_session_id) == seen.end()) {
      discovered.push_back(chat.native_session_id);
    }
  }
  return discovered;
}

static std::string PickFirstUnblockedSessionId(const std::vector<std::string>& candidate_ids,
                                               const std::unordered_set<std::string>& blocked_ids) {
  for (const std::string& candidate : candidate_ids) {
    if (!candidate.empty() && blocked_ids.find(candidate) == blocked_ids.end()) {
      return candidate;
    }
  }
  return "";
}

static bool SessionIdExistsInLoadedChats(const std::vector<ChatSession>& loaded_chats,
                                         const std::string& session_id) {
  if (session_id.empty()) {
    return false;
  }
  for (const ChatSession& chat : loaded_chats) {
    if (chat.uses_native_session && chat.native_session_id == session_id) {
      return true;
    }
  }
  return false;
}

static std::optional<fs::path> FindNativeSessionFilePathInDirectory(const fs::path& chats_dir, const std::string& session_id) {
  if (session_id.empty() || chats_dir.empty() || !fs::exists(chats_dir)) {
    return std::nullopt;
  }
  std::error_code ec;
  for (const auto& item : fs::directory_iterator(chats_dir, ec)) {
    if (ec || !item.is_regular_file() || item.path().extension() != ".json") {
      continue;
    }
    const std::string file_text = ReadTextFile(item.path());
    if (file_text.empty()) {
      continue;
    }
    const std::optional<JsonValue> root_opt = ParseJson(file_text);
    if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
      continue;
    }
    if (JsonStringOrEmpty(root_opt->Find("sessionId")) == session_id) {
      return item.path();
    }
  }
  return std::nullopt;
}

static std::optional<fs::path> FindNativeSessionFilePath(const AppState& app, const std::string& session_id) {
  return FindNativeSessionFilePathInDirectory(app.gemini_chats_dir, session_id);
}

static MessageRole NativeMessageRoleFromType(const ProviderProfile& provider, const std::string& type) {
  return ProviderRuntime::RoleFromNativeType(provider, type);
}

static bool TruncateNativeSessionFromDisplayedMessage(const AppState& app,
                                                      const ChatSession& chat,
                                                      const int displayed_message_index,
                                                      std::string* error_out) {
  if (!chat.uses_native_session || chat.native_session_id.empty()) {
    if (error_out != nullptr) {
      *error_out = "Chat is not linked to a native Gemini session.";
    }
    return false;
  }

  const auto session_file = FindNativeSessionFilePath(app, chat.native_session_id);
  if (!session_file.has_value()) {
    if (error_out != nullptr) {
      *error_out = "Could not locate native Gemini session file.";
    }
    return false;
  }

  const std::string file_text = ReadTextFile(session_file.value());
  if (file_text.empty()) {
    if (error_out != nullptr) {
      *error_out = "Native Gemini session file is empty.";
    }
    return false;
  }
  const std::optional<JsonValue> root_opt = ParseJson(file_text);
  if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
    if (error_out != nullptr) {
      *error_out = "Failed to parse native Gemini session JSON.";
    }
    return false;
  }
  JsonValue root = root_opt.value();

  auto messages_it = root.object_value.find("messages");
  if (messages_it == root.object_value.end() || messages_it->second.type != JsonValue::Type::Array) {
    if (error_out != nullptr) {
      *error_out = "Native Gemini session has no messages array.";
    }
    return false;
  }
  JsonValue& messages_array = messages_it->second;
  const ProviderProfile& provider = ActiveProviderOrDefault(app);

  std::vector<int> visible_raw_indices;
  std::vector<MessageRole> visible_roles;
  visible_raw_indices.reserve(messages_array.array_value.size());
  visible_roles.reserve(messages_array.array_value.size());
  for (int i = 0; i < static_cast<int>(messages_array.array_value.size()); ++i) {
    const JsonValue& raw_message = messages_array.array_value[i];
    if (raw_message.type != JsonValue::Type::Object) {
      continue;
    }
    const std::string content = Trim(ExtractGeminiContentText(raw_message.Find("content")));
    if (content.empty()) {
      continue;
    }
    visible_raw_indices.push_back(i);
    visible_roles.push_back(NativeMessageRoleFromType(provider, JsonStringOrEmpty(raw_message.Find("type"))));
  }

  if (displayed_message_index < 0 || displayed_message_index >= static_cast<int>(visible_raw_indices.size())) {
    if (error_out != nullptr) {
      *error_out = "Selected message no longer matches native session timeline.";
    }
    return false;
  }
  if (visible_roles[displayed_message_index] != MessageRole::User) {
    if (error_out != nullptr) {
      *error_out = "Only user messages can be edited.";
    }
    return false;
  }

  const int raw_cut_index = visible_raw_indices[displayed_message_index];
  messages_array.array_value.erase(messages_array.array_value.begin() + raw_cut_index, messages_array.array_value.end());

  if (!WriteTextFile(session_file.value(), SerializeJson(root))) {
    if (error_out != nullptr) {
      *error_out = "Failed to write updated native Gemini session.";
    }
    return false;
  }
  return true;
}

#include "common/runtime/terminal_runtime.h"

#include "common/ui/ui_sections.h"

static fs::path ResolveWindowIconPath() {
  std::vector<fs::path> candidates;
  if (char* base_path = SDL_GetBasePath(); base_path != nullptr) {
    candidates.emplace_back(fs::path(base_path) / "app_icon.bmp");
    SDL_free(base_path);
  }
  candidates.emplace_back("app_icon.bmp");
  candidates.emplace_back(fs::path("assets") / "app_icon.bmp");

  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }
  return {};
}

static void ApplyWindowIcon(SDL_Window* window) {
  if (window == nullptr) {
    return;
  }
  const fs::path icon_path = ResolveWindowIconPath();
  if (icon_path.empty()) {
    return;
  }
  const std::string icon_utf8 = icon_path.string();
  SDL_Surface* icon_surface = SDL_LoadBMP(icon_utf8.c_str());
  if (icon_surface == nullptr) {
    std::fprintf(stderr, "Warning: could not load window icon '%s': %s\n", icon_utf8.c_str(), SDL_GetError());
    return;
  }
  SDL_SetWindowIcon(window, icon_surface);
  SDL_FreeSurface(icon_surface);
}

int main(int, char**) {
  AppState app;
  std::vector<fs::path> data_root_candidates;
  if (const char* data_dir_env = std::getenv("UAM_DATA_DIR")) {
    const std::string env_root = Trim(data_dir_env);
    if (!env_root.empty()) {
      data_root_candidates.push_back(fs::path(env_root));
    }
  }
  if (data_root_candidates.empty()) {
    std::error_code cwd_ec;
    const fs::path cwd = fs::current_path(cwd_ec);
    if (!cwd_ec) {
      data_root_candidates.push_back(cwd / "data");
    }
    data_root_candidates.push_back(DefaultDataRootPath());
  }
  data_root_candidates.push_back(TempFallbackDataRootPath());

  std::unordered_set<std::string> tried_roots;
  std::string last_data_root_error = "Unknown data directory initialization failure.";
  bool initialized_data_root = false;
  for (const fs::path& candidate_root : data_root_candidates) {
    if (candidate_root.empty()) {
      continue;
    }
    const std::string key = candidate_root.lexically_normal().string();
    if (tried_roots.find(key) != tried_roots.end()) {
      continue;
    }
    tried_roots.insert(key);

    std::string error;
    if (EnsureDataRootLayout(candidate_root, &error)) {
      app.data_root = candidate_root;
      initialized_data_root = true;
      break;
    }
    last_data_root_error = std::move(error);
  }

  if (!initialized_data_root) {
    std::fprintf(stderr, "Failed to initialize application data directories: %s\n", last_data_root_error.c_str());
    return 1;
  }
  LoadSettings(app);
  const bool had_provider_file = fs::exists(ProviderProfileFilePath(app));
  app.provider_profiles = ProviderProfileStore::Load(app.data_root);
  ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
  if (ActiveProvider(app) == nullptr && !app.provider_profiles.empty()) {
    app.settings.active_provider_id = app.provider_profiles.front().id;
  }
  if (ProviderProfile* active_profile = ActiveProvider(app); active_profile != nullptr) {
    if (!had_provider_file && !app.settings.gemini_command_template.empty()) {
      active_profile->command_template = app.settings.gemini_command_template;
    }
    app.settings.gemini_command_template = active_profile->command_template;
  }
  if (!had_provider_file) {
    SaveProviders(app);
  }
  LoadFrontendActions(app);
  RefreshTemplateCatalog(app, true);
  app.folders = ChatFolderStore::Load(app.data_root);
  EnsureDefaultFolder(app);
  SaveFolders(app);
  if (ActiveProviderUsesGeminiHistory(app)) {
    RefreshGeminiChatsDir(app);
    app.chats = LoadNativeGeminiChats(app.gemini_chats_dir, ActiveProviderOrDefault(app));
    ApplyLocalOverrides(app, app.chats);
    NormalizeChatBranchMetadata(app);
    NormalizeChatFolderAssignments(app);
    if (app.gemini_chats_dir.empty()) {
      app.status_line = "Gemini native session directory not found yet. Run Gemini CLI in this project once.";
    }
  } else {
    app.chats = LoadChats(app);
    NormalizeChatBranchMetadata(app);
    NormalizeChatFolderAssignments(app);
  }
  if (!app.chats.empty()) {
    if (app.settings.remember_last_chat && !app.settings.last_selected_chat_id.empty()) {
      app.selected_chat_index = FindChatIndexById(app, app.settings.last_selected_chat_id);
    }
    if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size())) {
      app.selected_chat_index = 0;
    }
    RefreshRememberedSelection(app);
  }
  if (app.center_view_mode == CenterViewMode::CliConsole) {
    MarkSelectedCliTerminalForLaunch(app);
  }

#if defined(_WIN32)
  SetProcessDPIAware();
#endif

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

#if defined(__APPLE__)
  const char* glsl_version = "#version 150";
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
  const char* glsl_version = "#version 130";
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_Window* window = SDL_CreateWindow(
      "Universal Agent Manager",
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      app.settings.window_width,
      app.settings.window_height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  ApplyWindowIcon(window);

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (gl_context == nullptr) {
    std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  const float platform_ui_scale = DetectUiScale(window);
  g_platform_layout_scale = std::clamp(platform_ui_scale, 1.0f, 2.25f);
  ConfigureFonts(io, platform_ui_scale);
  ApplyThemeFromSettings(app);
  if (platform_ui_scale > 1.01f) {
    ImGui::GetStyle().ScaleAllSizes(platform_ui_scale);
  }
  CaptureUiScaleBaseStyle();
  ApplyUserUiScale(io, app.settings.ui_scale_multiplier);

  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init(glsl_version);

  if (app.settings.window_maximized) {
    SDL_MaximizeWindow(window);
  }

  bool done = false;
  bool terminals_stopped_for_shutdown = false;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (ForwardEscapeToSelectedCliTerminal(app, event)) {
        continue;
      }
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) {
        done = true;
      }
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window)) {
        done = true;
      }
      if (event.type == SDL_WINDOWEVENT && event.window.windowID == SDL_GetWindowID(window)) {
        const Uint8 window_event = event.window.event;
        if (window_event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            window_event == SDL_WINDOWEVENT_RESIZED ||
            window_event == SDL_WINDOWEVENT_MAXIMIZED ||
            window_event == SDL_WINDOWEVENT_RESTORED) {
          CaptureWindowState(app, window);
          SaveSettings(app);
        }
      }
    }

#if defined(_WIN32)
    if (done) {
      // Windows-only close guard: avoid blocking teardown while a ConPTY-backed
      // Gemini process is still alive. We issue async /quit attempts and force
      // terminate child CLI processes, then exit the render loop immediately.
      // If macOS develops the same shutdown race, we can make this universal.
      FastStopCliTerminalsForExitWindows(app);
      terminals_stopped_for_shutdown = true;
      break;
    }
#endif

    PollPendingGeminiCall(app);
    PollAllCliTerminals(app);
    PollGeminiCompatibilityTasks(app);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ApplyUserUiScale(io, app.settings.ui_scale_multiplier);

    HandleGlobalShortcuts(app);
    DrawDesktopMenuBar(app, done);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    DrawAmbientBackdrop(viewport->Pos, viewport->Size, static_cast<float>(ImGui::GetTime()));

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y));

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("UAM Gemini Manager", nullptr, window_flags);

    const float layout_w = ImGui::GetContentRegionAvail().x;
    float sidebar_w = std::clamp(layout_w * 0.25f, 250.0f, 360.0f);
    if (layout_w < 1020.0f) {
      sidebar_w = std::clamp(layout_w * 0.30f, 230.0f, 320.0f);
    }
#if defined(_WIN32)
    // Windows-only split tuning: keep chat list readable at larger DPI/user
    // scales without changing the macOS baseline layout.
    const float effective_scale = std::max(1.0f, EffectiveUiScale());
    const float width_bias = 1.0f + ((effective_scale - 1.0f) * 0.36f);
    const float sidebar_ratio = (layout_w < 1180.0f) ? 0.35f : 0.30f;
    sidebar_w = std::clamp(layout_w * sidebar_ratio, 280.0f * width_bias, 470.0f * width_bias);
    const float max_sidebar_from_main_floor = std::max(220.0f, layout_w - 560.0f);
    sidebar_w = std::clamp(sidebar_w, 220.0f, max_sidebar_from_main_floor);
#endif

    if (ImGui::BeginTable("layout_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX |
                                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBody)) {
      ImGui::TableSetupColumn("Chats", ImGuiTableColumnFlags_WidthFixed, sidebar_w);
      ImGui::TableSetupColumn("Conversation", ImGuiTableColumnFlags_WidthStretch, 0.72f);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      DrawLeftPane(app);

      ChatSession* selected = SelectedChat(app);
      ImGui::TableSetColumnIndex(1);
      if (selected == nullptr) {
        BeginPanel("empty_main", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, 0, ImVec2(ui::kSpace24, ui::kSpace24));
        ImGui::TextWrapped("No chat selected. Create one from the left panel.");
        EndPanel();
      } else {
        DrawChatDetailPane(app, *selected);
      }

      ImGui::EndTable();
    }

    ImGui::End();
    DrawAboutModal(app);
    DrawDeleteChatConfirmationModal(app);
    DrawDeleteFolderConfirmationModal(app);
    DrawFolderSettingsModal(app);
    DrawTemplateChangeWarningModal(app);
    DrawTemplateManagerModal(app);
    DrawVcsOutputModal(app);
    DrawAppSettingsModal(app, platform_ui_scale);
    ConsumePendingBranchRequest(app);

    ImGui::Render();
    int display_w = 0;
    int display_h = 0;
    SDL_GL_GetDrawableSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(ui::kMainBackground.x, ui::kMainBackground.y, ui::kMainBackground.z, ui::kMainBackground.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  CaptureWindowState(app, window);
  SaveSettings(app);

  app.pending_calls.clear();
  app.resolved_native_sessions_by_chat_id.clear();
  if (!terminals_stopped_for_shutdown) {
    StopAllCliTerminals(app, true);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
