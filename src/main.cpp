#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <vterm.h>
#include <curl/curl.h>
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
#include <mach-o/dyld.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
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
static const ProviderProfile* ProviderForChat(const AppState& app, const ChatSession& chat);
static const ProviderProfile& ProviderForChatOrDefault(const AppState& app, const ChatSession& chat);
static bool ActiveProviderUsesGeminiHistory(const AppState& app);
static bool ActiveProviderUsesInternalEngine(const AppState& app);
static bool ChatUsesGeminiHistory(const AppState& app, const ChatSession& chat);
static bool ChatUsesInternalEngine(const AppState& app, const ChatSession& chat);
static bool ChatUsesCliOutput(const AppState& app, const ChatSession& chat);
static int FindChatIndexById(const AppState& app, const std::string& chat_id);
static ChatSession* SelectedChat(AppState& app);
static const ChatSession* SelectedChat(const AppState& app);
static ChatSession CreateNewChat(const std::string& folder_id, const std::string& provider_id);
static std::string CompactPreview(const std::string& text, std::size_t max_len);
static void MarkSelectedCliTerminalForLaunch(AppState& app);
static void SelectChatById(AppState& app, const std::string& chat_id);
static void SaveSettings(AppState& app);
static bool SaveChat(const AppState& app, const ChatSession& chat);
static bool ProviderUsesOpenCodeLocalBridge(const ProviderProfile& provider);
static bool EnsureSelectedLocalRuntimeModelForProvider(AppState& app);
static bool SendPromptToCliRuntime(AppState& app, ChatSession& chat, const std::string& prompt, std::string* error_out);
static bool EnsureOpenCodeBridgeRunning(AppState& app, std::string* error_out = nullptr);
static void StopOpenCodeBridge(AppState& app);
static bool RestartOpenCodeBridgeIfModelChanged(AppState& app, std::string* error_out = nullptr);
static void StartAsyncCommandTask(AsyncCommandTask& task, const std::string& command);
static bool TryConsumeAsyncCommandTaskOutput(AsyncCommandTask& task, std::string& output_out);
static std::optional<std::string> ExtractSemverVersion(const std::string& text);
static std::string BuildGeminiVersionCheckCommand();
static std::string BuildGeminiDowngradeCommand();
static void StartGeminiVersionCheck(AppState& app, const bool force);
static void StartGeminiDowngradeToSupported(AppState& app);
static void PollGeminiCompatibilityTasks(AppState& app);
static bool IsLocalDraftChatId(const std::string& chat_id);
static std::optional<std::string> InferNativeSessionIdForLocalDraft(const ChatSession& local_chat,
                                                                    const std::vector<ChatSession>& native_chats);
static bool PersistLocalDraftNativeSessionLink(const AppState& app, ChatSession& local_chat, const std::string& native_session_id);
static std::string ResolveResumeSessionIdForChat(const AppState& app, const ChatSession& chat);
static bool HasPendingCallForChat(const AppState& app, const std::string& chat_id);
static bool HasAnyPendingCall(const AppState& app);
static const PendingGeminiCall* FirstPendingCallForChat(const AppState& app, const std::string& chat_id);
static void NormalizeChatBranchMetadata(AppState& app);
static bool CreateBranchFromMessage(AppState& app, const std::string& source_chat_id, int message_index);
static void ConsumePendingBranchRequest(AppState& app);
static std::string BuildRagContextBlock(const std::vector<RagSnippet>& snippets);
static std::string BuildRagEnhancedPrompt(AppState& app, const ChatSession& chat, const std::string& prompt_text);
static bool IsRagEnabledForChat(const AppState& app, const ChatSession& chat);
static RagIndexService::Config RagConfigFromSettings(const AppSettings& settings);
static void SyncRagServiceConfig(AppState& app);
static std::filesystem::path ResolveProjectRagSourceRoot(const AppState& app,
                                                         const std::filesystem::path& fallback_source_root = {});
static std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path);
static std::vector<std::filesystem::path> ResolveRagSourceRootsForChat(const AppState& app,
                                                                        const ChatSession& chat,
                                                                        const std::filesystem::path& fallback_source_root = {});
static std::vector<std::filesystem::path> DiscoverRagSourceFolders(const std::filesystem::path& workspace_root);
static std::string RagDatabaseNameForSourceRoot(const AppSettings& settings, const std::filesystem::path& source_root);
static bool ChatHasRagSourceDirectory(const ChatSession& chat, const std::filesystem::path& source_root);
static bool AddChatRagSourceDirectory(ChatSession& chat, const std::filesystem::path& source_root);
static bool RemoveChatRagSourceDirectoryAt(ChatSession& chat, std::size_t index);
static bool RemoveChatRagSourceDirectory(ChatSession& chat, const std::filesystem::path& source_root);
static bool TriggerProjectRagScan(AppState& app,
                                  bool reuse_previous_source,
                                  const std::filesystem::path& fallback_source_root,
                                  std::string* error_out = nullptr);
static void PollRagScanState(AppState& app);
static RagScanState EffectiveRagScanState(const AppState& app);
static std::string BuildRagStatusText(const AppState& app);
static std::filesystem::path ResolveCurrentRagFallbackSourceRoot(const AppState& app);
static std::filesystem::path BuildRagTokenCappedStagingRoot(const AppState& app, const std::string& workspace_key);
static bool BuildRagTokenCappedStagingTree(const std::filesystem::path& source_root,
                                           const std::filesystem::path& staging_root,
                                           int max_tokens,
                                           std::size_t* indexed_files_out,
                                           std::string* error_out);
static void EnsureRagManualQueryWorkspaceState(AppState& app, const std::string& workspace_key);
static void AppendRagScanReport(AppState& app, const std::string& message);
static void RunRagManualTestQuery(AppState& app, const std::filesystem::path& workspace_root);
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

static std::string NormalizeVectorDatabaseName(std::string value) {
  value = Trim(value);
  value.erase(std::remove_if(value.begin(), value.end(), [](const char ch) {
                const unsigned char c = static_cast<unsigned char>(ch);
                return !(std::isalnum(c) != 0 || ch == '_' || ch == '-' || ch == '.');
              }),
              value.end());
  return value;
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
  fs::path candidate = ExpandLeadingTildePath(settings.prompt_profile_root_path);
  if (candidate.empty()) {
    candidate = ExpandLeadingTildePath(settings.gemini_global_root_path);
  }
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

static fs::path ResolveCurrentRagFallbackSourceRoot(const AppState& app) {
  if (const ChatSession* selected = SelectedChat(app); selected != nullptr) {
    return ResolveWorkspaceRootPath(app, *selected);
  }
  std::error_code cwd_ec;
  const fs::path cwd = fs::current_path(cwd_ec);
  return cwd_ec ? fs::path{} : cwd;
}

static std::uint64_t Fnv1a64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ULL;
  }
  return hash;
}

static std::string Hex64(const std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << value;
  return out.str();
}

static std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

static bool IsLikelyBinaryBlob(const std::string& content) {
  return content.find('\0') != std::string::npos;
}

static bool IsRagIndexableTextFile(const fs::path& path) {
  static const std::unordered_set<std::string> kAllowedExtensions = {
      ".c",      ".cc",     ".cpp",    ".cxx",   ".h",      ".hh",     ".hpp",    ".hxx",   ".ixx",  ".ipp",
      ".m",      ".mm",     ".java",   ".kt",    ".kts",    ".go",     ".rs",     ".swift", ".cs",   ".py",
      ".js",     ".ts",     ".tsx",    ".jsx",   ".php",    ".rb",     ".lua",    ".sh",    ".zsh",  ".bash",
      ".ps1",    ".sql",    ".json",   ".yaml",  ".yml",    ".toml",   ".ini",    ".cfg",   ".conf", ".xml",
      ".html",   ".css",    ".scss",   ".md",    ".markdown", ".txt", ".rst", ".adoc", ".cmake", ".mk", ".make"};
  const std::string extension = ToLowerAscii(path.extension().string());
  return kAllowedExtensions.find(extension) != kAllowedExtensions.end();
}

static std::string TruncateToApproxTokenCount(const std::string& content, const std::size_t max_tokens) {
  if (max_tokens == 0 || content.empty()) {
    return content;
  }
  std::size_t token_count = 0;
  bool in_token = false;
  std::size_t token_start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(content[i]);
    const bool whitespace = (std::isspace(ch) != 0);
    if (!whitespace && !in_token) {
      token_start = i;
      ++token_count;
      if (token_count > max_tokens) {
        std::size_t end = token_start;
        while (end > 0 && std::isspace(static_cast<unsigned char>(content[end - 1])) != 0) {
          --end;
        }
        return content.substr(0, end);
      }
      in_token = true;
    } else if (whitespace) {
      in_token = false;
    }
  }
  return content;
}

static std::filesystem::path BuildRagTokenCappedStagingRoot(const AppState& app, const std::string& workspace_key) {
  return app.data_root / "rag_scan_staging" / ("ws_" + Hex64(Fnv1a64(workspace_key)));
}

static bool BuildRagTokenCappedStagingTree(const std::filesystem::path& source_root,
                                           const std::filesystem::path& staging_root,
                                           const int max_tokens,
                                           std::size_t* indexed_files_out,
                                           std::string* error_out) {
  if (indexed_files_out != nullptr) {
    *indexed_files_out = 0;
  }
  if (max_tokens <= 0) {
    if (error_out != nullptr) {
      *error_out = "Token cap must be greater than zero.";
    }
    return false;
  }

  std::error_code ec;
  fs::remove_all(staging_root, ec);
  ec.clear();
  fs::create_directories(staging_root, ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to prepare token-capped staging directory: " + staging_root.string();
    }
    return false;
  }

  std::size_t copied_files = 0;
  fs::recursive_directory_iterator it(source_root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (!ec && it != end) {
    const fs::directory_entry entry = *it;
    ++it;
    if (ec) {
      ec.clear();
      continue;
    }
    if (!entry.is_regular_file(ec)) {
      ec.clear();
      continue;
    }
    if (!IsRagIndexableTextFile(entry.path())) {
      continue;
    }

    const fs::path absolute = entry.path();
    const fs::path relative = fs::relative(absolute, source_root, ec);
    if (ec || relative.empty()) {
      ec.clear();
      continue;
    }
    const fs::path destination = staging_root / relative;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
      ec.clear();
      continue;
    }

    std::ifstream in(absolute, std::ios::binary);
    if (!in.good()) {
      continue;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string content = buffer.str();
    if (IsLikelyBinaryBlob(content)) {
      continue;
    }
    content = TruncateToApproxTokenCount(content, static_cast<std::size_t>(max_tokens));
    if (!WriteTextFile(destination, content)) {
      continue;
    }
    ++copied_files;
  }

  if (indexed_files_out != nullptr) {
    *indexed_files_out = copied_files;
  }
  return true;
}

static fs::path ResolveProjectRagSourceRoot(const AppState& app, const fs::path& fallback_source_root) {
  fs::path source_root = ExpandLeadingTildePath(app.settings.rag_project_source_directory);
  if (source_root.empty()) {
    source_root = fallback_source_root;
  }
  if (source_root.empty()) {
    std::error_code cwd_ec;
    source_root = fs::current_path(cwd_ec);
  }
  std::error_code ec;
  const fs::path absolute_root = fs::absolute(source_root, ec);
  return ec ? source_root.lexically_normal() : absolute_root.lexically_normal();
}

static fs::path NormalizeAbsolutePath(const fs::path& path) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  const fs::path absolute = fs::absolute(path, ec);
  return ec ? path.lexically_normal() : absolute.lexically_normal();
}

static std::vector<fs::path> ResolveRagSourceRootsForChat(const AppState& app,
                                                          const ChatSession& chat,
                                                          const fs::path& fallback_source_root) {
  std::vector<fs::path> roots;
  std::unordered_set<std::string> seen;
  roots.reserve(chat.rag_source_directories.size() + 1);

  for (const std::string& raw_source : chat.rag_source_directories) {
    fs::path source_root = NormalizeAbsolutePath(ExpandLeadingTildePath(raw_source));
    if (source_root.empty()) {
      continue;
    }
    const std::string source_key = source_root.generic_string();
    if (source_key.empty()) {
      continue;
    }
    if (seen.insert(source_key).second) {
      roots.push_back(source_root);
    }
  }

  if (roots.empty()) {
    roots.push_back(ResolveProjectRagSourceRoot(app, fallback_source_root));
  }
  return roots;
}

static std::vector<fs::path> DiscoverRagSourceFolders(const fs::path& workspace_root) {
  std::vector<fs::path> folders;
  std::error_code ec;
  const fs::path normalized_workspace = NormalizeAbsolutePath(workspace_root);
  if (normalized_workspace.empty() || !fs::exists(normalized_workspace, ec) || !fs::is_directory(normalized_workspace, ec)) {
    return folders;
  }
  folders.push_back(normalized_workspace);

  static const std::unordered_set<std::string> kExcluded = {
      ".git", ".svn", ".hg", "node_modules", "dist", "build", "out", "target", "__pycache__", ".venv", "venv"};

  for (const auto& entry : fs::directory_iterator(normalized_workspace, fs::directory_options::skip_permission_denied, ec)) {
    if (ec || !entry.is_directory(ec)) {
      ec.clear();
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.empty() || name[0] == '.' || kExcluded.find(name) != kExcluded.end()) {
      continue;
    }
    folders.push_back(NormalizeAbsolutePath(entry.path()));
  }

  std::sort(folders.begin(), folders.end(), [](const fs::path& lhs, const fs::path& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  folders.erase(std::remove_if(folders.begin(), folders.end(), [](const fs::path& path) { return path.empty(); }), folders.end());
  folders.erase(std::unique(folders.begin(), folders.end(), [](const fs::path& lhs, const fs::path& rhs) {
                  return lhs.generic_string() == rhs.generic_string();
                }),
                folders.end());
  return folders;
}

static std::string RagDatabaseNameForSourceRoot(const AppSettings& settings, const fs::path& source_root) {
  const std::string override_name = Trim(settings.vector_database_name_override);
  if (!override_name.empty()) {
    return override_name;
  }
  const std::string source_key = source_root.lexically_normal().generic_string();
  if (source_key.empty()) {
    return "";
  }
  return "uam_" + Hex64(Fnv1a64(source_key));
}

static bool ChatHasRagSourceDirectory(const ChatSession& chat, const fs::path& source_root) {
  const std::string candidate_key = NormalizeAbsolutePath(source_root).generic_string();
  if (candidate_key.empty()) {
    return false;
  }
  for (const std::string& existing_source : chat.rag_source_directories) {
    const fs::path existing_path = NormalizeAbsolutePath(ExpandLeadingTildePath(existing_source));
    if (!existing_path.empty() && existing_path.generic_string() == candidate_key) {
      return true;
    }
  }
  return false;
}

static bool AddChatRagSourceDirectory(ChatSession& chat, const fs::path& source_root) {
  const fs::path normalized_root = NormalizeAbsolutePath(source_root);
  const std::string candidate_key = normalized_root.generic_string();
  if (candidate_key.empty()) {
    return false;
  }
  if (ChatHasRagSourceDirectory(chat, normalized_root)) {
    return false;
  }
  chat.rag_source_directories.push_back(normalized_root.string());
  return true;
}

static bool RemoveChatRagSourceDirectoryAt(ChatSession& chat, const std::size_t index) {
  if (index >= chat.rag_source_directories.size()) {
    return false;
  }
  chat.rag_source_directories.erase(chat.rag_source_directories.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

static bool RemoveChatRagSourceDirectory(ChatSession& chat, const fs::path& source_root) {
  const std::string remove_key = NormalizeAbsolutePath(source_root).generic_string();
  if (remove_key.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < chat.rag_source_directories.size(); ++i) {
    const fs::path existing = NormalizeAbsolutePath(ExpandLeadingTildePath(chat.rag_source_directories[i]));
    if (!existing.empty() && existing.generic_string() == remove_key) {
      return RemoveChatRagSourceDirectoryAt(chat, i);
    }
  }
  return false;
}

static bool DirectoryContainsGguf(const fs::path& directory) {
  std::error_code ec;
  if (directory.empty() || !fs::exists(directory, ec) || !fs::is_directory(directory, ec)) {
    return false;
  }
  fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (!ec && it != end) {
    const fs::directory_entry entry = *it;
    ++it;
    if (ec) {
      ec.clear();
      continue;
    }
    if (!entry.is_regular_file(ec)) {
      ec.clear();
      continue;
    }
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (extension == ".gguf") {
      return true;
    }
  }
  return false;
}

static fs::path ResolveRagModelFolder(const AppState& app, const AppSettings* settings_override = nullptr) {
  const AppSettings& settings = (settings_override != nullptr) ? *settings_override : app.settings;
  const fs::path configured_model_folder = NormalizeAbsolutePath(ExpandLeadingTildePath(settings.models_folder_directory));
  if (!configured_model_folder.empty()) {
    std::error_code configured_ec;
    fs::create_directories(configured_model_folder, configured_ec);
    return configured_model_folder;
  }

  std::vector<fs::path> candidates;
  candidates.push_back(app.data_root / "models");
  if (const char* env_models = std::getenv("UAM_OLLAMA_ENGINE_MODELS_DIR")) {
    const std::string value = Trim(env_models);
    if (!value.empty()) {
      candidates.push_back(ExpandLeadingTildePath(value));
    }
  }
  std::error_code cwd_ec;
  const fs::path cwd = fs::current_path(cwd_ec);
  if (!cwd_ec) {
    candidates.push_back(cwd / "models");
    candidates.push_back(cwd / "build" / "models");
  }
  for (const fs::path& candidate : candidates) {
    if (DirectoryContainsGguf(candidate)) {
      return candidate;
    }
  }

  std::error_code ec;
  fs::create_directories(app.data_root / "models", ec);
  return app.data_root / "models";
}

static RagIndexService::Config RagConfigFromSettings(const AppSettings& settings) {
  RagIndexService::Config config;
  config.enabled = settings.rag_enabled;
  config.vector_backend = (settings.vector_db_backend == "none") ? "none" : "ollama-engine";
  config.vector_enabled = (config.vector_backend != "none");
  config.top_k = std::clamp(settings.rag_top_k, 1, 20);
  config.max_snippet_chars = static_cast<std::size_t>(std::clamp(settings.rag_max_snippet_chars, 120, 4000));
  config.max_file_bytes = static_cast<std::size_t>(std::clamp(settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024));
  config.vector_max_tokens = static_cast<std::size_t>(std::clamp(settings.rag_scan_max_tokens, 0, 32768));
  config.vector_model_id = Trim(settings.selected_vector_model_id);
  config.vector_database_name_override = Trim(settings.vector_database_name_override);
  return config;
}

static void SyncRagServiceConfig(AppState& app) {
  app.rag_index_service.SetConfig(RagConfigFromSettings(app.settings));
  const fs::path model_folder = ResolveRagModelFolder(app);
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
    out << (i + 1) << ". ";
    if (!snippet.relative_path.empty()) {
      out << snippet.relative_path;
      if (snippet.start_line > 0 && snippet.end_line >= snippet.start_line) {
        out << ":" << snippet.start_line << "-" << snippet.end_line;
      }
    } else {
      out << "Snippet";
    }
    out << "\n";
    out << snippet.text << "\n\n";
  }
  return out.str();
}

static bool IsRagEnabledForChat(const AppState& app, const ChatSession& chat) {
  return app.settings.rag_enabled && chat.rag_enabled;
}

static std::string BuildRagEnhancedPrompt(AppState& app, const ChatSession& chat, const std::string& prompt_text) {
  if (!IsRagEnabledForChat(app, chat)) {
    return prompt_text;
  }

  const fs::path chat_workspace_root = ResolveWorkspaceRootPath(app, chat);
  const std::vector<fs::path> source_roots = ResolveRagSourceRootsForChat(app, chat, chat_workspace_root);
  const std::size_t top_k = static_cast<std::size_t>(std::clamp(app.settings.rag_top_k, 1, 20));
  const bool multiple_sources = source_roots.size() > 1;

  std::vector<std::vector<RagSnippet>> snippets_by_source;
  snippets_by_source.reserve(source_roots.size());

  for (const fs::path& source_root : source_roots) {
    const std::string source_key = source_root.lexically_normal().generic_string();
    std::error_code ec;
    if (source_root.empty() || !fs::exists(source_root, ec) || !fs::is_directory(source_root, ec)) {
      if (!source_key.empty()) {
        app.rag_last_refresh_by_workspace[source_key] = "RAG source directory is invalid or missing.";
      }
      continue;
    }

    std::string rag_error;
    std::vector<RagSnippet> snippets = app.rag_index_service.Retrieve(source_root, prompt_text, top_k, 1, &rag_error);
    if (!rag_error.empty() && !source_key.empty()) {
      app.rag_last_refresh_by_workspace[source_key] = rag_error;
    }
    if (snippets.empty()) {
      continue;
    }

    if (multiple_sources) {
      std::string source_label = source_root.filename().string();
      if (source_label.empty()) {
        source_label = source_root.string();
      }
      for (RagSnippet& snippet : snippets) {
        if (snippet.relative_path.empty()) {
          snippet.relative_path = source_label;
        } else {
          snippet.relative_path = source_label + "/" + snippet.relative_path;
        }
      }
    }
    snippets_by_source.push_back(std::move(snippets));
  }

  std::vector<RagSnippet> merged_snippets;
  merged_snippets.reserve(top_k);
  std::vector<std::size_t> source_offsets(snippets_by_source.size(), 0);
  while (merged_snippets.size() < top_k) {
    bool added_any = false;
    for (std::size_t i = 0; i < snippets_by_source.size(); ++i) {
      if (source_offsets[i] >= snippets_by_source[i].size()) {
        continue;
      }
      merged_snippets.push_back(snippets_by_source[i][source_offsets[i]]);
      ++source_offsets[i];
      added_any = true;
      if (merged_snippets.size() >= top_k) {
        break;
      }
    }
    if (!added_any) {
      break;
    }
  }

  if (merged_snippets.empty()) {
    return prompt_text;
  }

  return BuildRagContextBlock(merged_snippets) + "User prompt:\n" + prompt_text;
}

static bool TriggerProjectRagScan(AppState& app,
                                  const bool reuse_previous_source,
                                  const fs::path& fallback_source_root,
                                  std::string* error_out) {
  const auto normalize_root = [](const fs::path& source_root) {
    if (source_root.empty()) {
      return fs::path{};
    }
    std::error_code ec;
    const fs::path absolute_root = fs::absolute(source_root, ec);
    return ec ? source_root.lexically_normal() : absolute_root.lexically_normal();
  };

  std::vector<fs::path> source_roots;
  const fs::path requested_root = normalize_root(fallback_source_root);
  if (const ChatSession* selected_chat = SelectedChat(app); selected_chat != nullptr) {
    source_roots = ResolveRagSourceRootsForChat(app, *selected_chat, fallback_source_root);
  }
  if (source_roots.empty()) {
    source_roots.push_back(requested_root.empty() ? ResolveProjectRagSourceRoot(app, fallback_source_root) : requested_root);
  }
  if (!requested_root.empty()) {
    const auto requested_it = std::find_if(source_roots.begin(), source_roots.end(), [&](const fs::path& source_root) {
      return normalize_root(source_root).generic_string() == requested_root.generic_string();
    });
    if (requested_it != source_roots.end() && requested_it != source_roots.begin()) {
      std::rotate(source_roots.begin(), requested_it, requested_it + 1);
    }
  }
  const fs::path workspace_root = source_roots.front();
  const std::string workspace_display = workspace_root.empty() ? "<unset>" : workspace_root.string();
  const std::string workspace_key = workspace_root.lexically_normal().generic_string();
  if (source_roots.size() > 1) {
    AppendRagScanReport(
        app,
        "Multiple RAG source folders are selected for this chat; scan action targets the first folder: " + workspace_display);
  }
  std::error_code ec;
  if (workspace_root.empty() || !fs::exists(workspace_root, ec) || !fs::is_directory(workspace_root, ec)) {
    AppendRagScanReport(app, "Scan start rejected: source directory is invalid (" + workspace_display + ").");
    app.open_rag_console_popup = true;
    if (error_out != nullptr) {
      *error_out = "RAG source directory is invalid or missing.";
    }
    return false;
  }

  app.settings.rag_scan_max_tokens = std::clamp(app.settings.rag_scan_max_tokens, 0, 32768);
  if (!reuse_previous_source) {
    if (app.settings.rag_scan_max_tokens > 0) {
      const fs::path staging_root = BuildRagTokenCappedStagingRoot(app, workspace_key);
      std::size_t staged_files = 0;
      std::string stage_error;
      if (!BuildRagTokenCappedStagingTree(
              workspace_root, staging_root, app.settings.rag_scan_max_tokens, &staged_files, &stage_error)) {
        const std::string failure = stage_error.empty() ? "Failed to build token-capped staging source." : stage_error;
        AppendRagScanReport(app, "Scan start rejected: " + failure);
        app.open_rag_console_popup = true;
        if (error_out != nullptr) {
          *error_out = failure;
        }
        return false;
      }
      app.rag_index_service.SetScanSourceOverride(workspace_root, staging_root);
      std::ostringstream report;
      report << "Using token-capped staging source: " << staging_root.string() << " (" << staged_files
             << " files) | embedding token cap=" << app.settings.rag_scan_max_tokens;
      AppendRagScanReport(app, report.str());
    } else {
      app.rag_index_service.ClearScanSourceOverride(workspace_root);
    }
  }

  const bool has_local_models = !app.rag_index_service.ListModels().empty();
  const RagRefreshResult refresh = reuse_previous_source ? app.rag_index_service.RescanPreviousSource(workspace_root)
                                                         : app.rag_index_service.RebuildIndex(workspace_root);
  if (!refresh.ok) {
    app.rag_last_refresh_by_workspace[workspace_key] = refresh.error;
    AppendRagScanReport(app, "Scan start failed: " + refresh.error);
    app.open_rag_console_popup = true;
    if (error_out != nullptr) {
      *error_out = refresh.error;
    }
    return false;
  }

  app.rag_scan_workspace_key = workspace_key;
  app.rag_scan_state = app.rag_index_service.FetchState();
  app.rag_scan_status_last_emit_s = ImGui::GetTime();
  app.rag_finished_visible_until_s = 0.0;
  app.rag_last_refresh_by_workspace[workspace_key] =
      reuse_previous_source ? "RAG rescan started (previous source)." : "RAG scan started.";
  app.status_line = reuse_previous_source ? "RAG rescan started (previous source)." : "RAG scan started.";
  AppendRagScanReport(
      app,
      (reuse_previous_source ? "Rescan started (previous source)." : "Scan started.") + std::string(" Source: ") + workspace_root.string());
  app.open_rag_console_popup = true;
  if (!has_local_models) {
    app.status_line += " (no local .gguf detected; relying on llama server if available)";
    AppendRagScanReport(app, "No local .gguf models detected; scan relies on configured llama server.");
  }
  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

static void PollRagScanState(AppState& app) {
  const RagScanState previous_state = app.rag_scan_state;
  app.rag_scan_state = app.rag_index_service.FetchState();
  const double now = ImGui::GetTime();
  const bool transitioned_to_finished =
      previous_state.lifecycle != RagScanLifecycleState::Finished && app.rag_scan_state.lifecycle == RagScanLifecycleState::Finished;

  if (transitioned_to_finished) {
    app.rag_finished_visible_until_s = now + 8.0;
    if (!app.rag_scan_workspace_key.empty()) {
      std::string finished = "Finished";
      if (app.rag_scan_state.vector_database_size > 0) {
        finished += " | " + std::to_string(app.rag_scan_state.vector_database_size) + " vectors";
      }
      app.rag_last_refresh_by_workspace[app.rag_scan_workspace_key] = finished;
      app.rag_last_rebuild_at_by_workspace[app.rag_scan_workspace_key] = TimestampNow();
    }
    app.status_line = "RAG scan finished: " + std::to_string(app.rag_scan_state.files_processed) + "/" +
                      std::to_string(app.rag_scan_state.total_files) + " files";
    if (app.rag_scan_state.vector_database_size > 0) {
      app.status_line += " | " + std::to_string(app.rag_scan_state.vector_database_size) + " vectors";
    }
    AppendRagScanReport(app, app.status_line);
    app.rag_scan_status_last_emit_s = now;
    return;
  }

  if (app.rag_scan_state.lifecycle == RagScanLifecycleState::Running && !app.rag_scan_workspace_key.empty()) {
    std::ostringstream running;
    running << "Running";
    if (app.rag_scan_state.total_files > 0) {
      running << " " << app.rag_scan_state.files_processed << "/" << app.rag_scan_state.total_files << " files";
    }
    if (app.rag_scan_state.vector_database_size > 0) {
      running << " | " << app.rag_scan_state.vector_database_size << " vectors";
    }
    app.rag_last_refresh_by_workspace[app.rag_scan_workspace_key] = running.str();

    const bool changed_progress = previous_state.lifecycle != RagScanLifecycleState::Running ||
                                  previous_state.files_processed != app.rag_scan_state.files_processed ||
                                  previous_state.total_files != app.rag_scan_state.total_files ||
                                  previous_state.vector_database_size != app.rag_scan_state.vector_database_size;
    if (changed_progress && (now - app.rag_scan_status_last_emit_s >= 0.33 || previous_state.lifecycle != RagScanLifecycleState::Running)) {
      app.status_line = "RAG scan: " + running.str();
      AppendRagScanReport(app, app.status_line);
      app.rag_scan_status_last_emit_s = now;
    }
    return;
  }

  if (previous_state.lifecycle == RagScanLifecycleState::Running &&
      app.rag_scan_state.lifecycle == RagScanLifecycleState::Stopped) {
    if (!app.rag_scan_state.error.empty()) {
      app.status_line = "RAG scan failed: " + app.rag_scan_state.error;
    } else if (previous_state.total_files == 0) {
      app.status_line =
          "RAG scan stopped quickly: no indexable files found (.cpp/.h/.md/.txt/etc) in source directory.";
    } else if (previous_state.vector_database_size == 0) {
      app.status_line =
          "RAG scan stopped with 0 vectors. Check embedding model (.gguf) or UAM_EMBEDDING_MODEL_PATH.";
    } else {
      app.status_line = "RAG scan stopped.";
    }
    AppendRagScanReport(app, app.status_line);
    app.rag_scan_status_last_emit_s = now;
  }
}

static RagScanState EffectiveRagScanState(const AppState& app) {
  RagScanState state = app.rag_scan_state;
  if (state.lifecycle == RagScanLifecycleState::Stopped && app.rag_finished_visible_until_s > ImGui::GetTime()) {
    state.lifecycle = RagScanLifecycleState::Finished;
  }
  return state;
}

static std::string BuildRagStatusText(const AppState& app) {
  const RagScanState state = EffectiveRagScanState(app);
  if (state.lifecycle == RagScanLifecycleState::Finished) {
    return "RAG: Finished";
  }
  if (state.lifecycle == RagScanLifecycleState::Running) {
    std::ostringstream out;
    out << "RAG: Running";
    if (state.total_files > 0) {
      out << " " << state.files_processed << "/" << state.total_files << " files";
    } else {
      out << " (scanning...)";
    }
    if (state.vector_database_size > 0) {
      if (state.total_files > 0) {
        out << " | ";
      } else {
        out << " ";
      }
      out << state.vector_database_size << " vectors";
    }
    return out.str();
  }
  return "RAG: Stopped";
}

static void EnsureRagManualQueryWorkspaceState(AppState& app, const std::string& workspace_key) {
  if (app.rag_manual_query_workspace_key == workspace_key) {
    return;
  }
  app.rag_manual_query_workspace_key = workspace_key;
  app.rag_manual_query_results.clear();
  app.rag_manual_query_error.clear();
  app.rag_manual_query_last_query.clear();
}

static void AppendRagScanReport(AppState& app, const std::string& message) {
  const std::string trimmed = Trim(message);
  if (trimmed.empty()) {
    return;
  }

  if (!app.rag_scan_reports.empty()) {
    const std::string& last = app.rag_scan_reports.back();
    const std::size_t separator = last.find(" | ");
    if (separator != std::string::npos && last.substr(separator + 3) == trimmed) {
      return;
    }
  }

  app.rag_scan_reports.push_back(TimestampNow() + " | " + trimmed);
  constexpr std::size_t kMaxRagReports = 320;
  if (app.rag_scan_reports.size() > kMaxRagReports) {
    const std::size_t trim_count = app.rag_scan_reports.size() - kMaxRagReports;
    app.rag_scan_reports.erase(app.rag_scan_reports.begin(),
                               app.rag_scan_reports.begin() + static_cast<std::ptrdiff_t>(trim_count));
  }
  app.rag_scan_reports_scroll_to_bottom = true;
}

static void RunRagManualTestQuery(AppState& app, const std::filesystem::path& workspace_root) {
  app.rag_manual_query_max = std::clamp(app.rag_manual_query_max, 1, 50);
  app.rag_manual_query_min = std::clamp(app.rag_manual_query_min, 1, app.rag_manual_query_max);
  app.rag_manual_query_running = true;
  app.rag_manual_query_error.clear();
  app.rag_manual_query_last_query = app.rag_manual_query_input;
  std::string query_error;
  app.rag_manual_query_results = app.rag_index_service.Retrieve(
      workspace_root,
      app.rag_manual_query_input,
      static_cast<std::size_t>(app.rag_manual_query_max),
      static_cast<std::size_t>(app.rag_manual_query_min),
      &query_error);
  app.rag_manual_query_running = false;
  app.rag_manual_query_error = query_error;
  if (query_error.empty()) {
    app.status_line = "RAG test query completed.";
    AppendRagScanReport(app, "Manual query returned " + std::to_string(app.rag_manual_query_results.size()) + " snippet(s).");
  } else {
    app.status_line = "RAG test query failed: " + query_error;
    AppendRagScanReport(app, "Manual query failed: " + query_error);
  }
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

  ChatSession branch = CreateNewChat(source.folder_id, source.provider_id);
  branch.uses_native_session = false;
  branch.native_session_id.clear();
  branch.parent_chat_id = source.id;
  branch.branch_root_chat_id = source.branch_root_chat_id.empty() ? source.id : source.branch_root_chat_id;
  branch.branch_from_message_index = message_index;
  branch.template_override_id = source.template_override_id;
  branch.gemini_md_bootstrapped = source.gemini_md_bootstrapped;
  branch.rag_enabled = source.rag_enabled;
  branch.rag_source_directories = source.rag_source_directories;
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
  if (ChatUsesCliOutput(app, app.chats[app.selected_chat_index])) {
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

static std::string OpenCodeBridgeRandomHex(const std::size_t length) {
  static thread_local std::mt19937 lRng(std::random_device{}());
  std::uniform_int_distribution<int> lNibble(0, 15);
  std::string lSValue;
  lSValue.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    const int lValue = lNibble(lRng);
    lSValue.push_back(static_cast<char>((lValue < 10) ? ('0' + lValue) : ('a' + (lValue - 10))));
  }
  return lSValue;
}

static std::string OpenCodeBridgeTimestampStamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &tt);
#else
  localtime_r(&tt, &tm_snapshot);
#endif
  std::ostringstream out;
  out << std::put_time(&tm_snapshot, "%Y%m%d-%H%M%S");
  return out.str();
}

static fs::path ResolveCurrentExecutablePathForBridge() {
#if defined(_WIN32)
  std::wstring lWBuffer(static_cast<std::size_t>(MAX_PATH), L'\0');
  const DWORD lDwLength = GetModuleFileNameW(nullptr, lWBuffer.data(), static_cast<DWORD>(lWBuffer.size()));
  if (lDwLength == 0 || lDwLength >= lWBuffer.size()) {
    return {};
  }
  lWBuffer.resize(static_cast<std::size_t>(lDwLength));
  return fs::path(lWBuffer);
#elif defined(__APPLE__)
  uint32_t lUiBufferSize = 0;
  (void)_NSGetExecutablePath(nullptr, &lUiBufferSize);
  if (lUiBufferSize == 0) {
    return {};
  }
  std::string lSBuffer(static_cast<std::size_t>(lUiBufferSize), '\0');
  if (_NSGetExecutablePath(lSBuffer.data(), &lUiBufferSize) != 0) {
    return {};
  }
  return fs::path(lSBuffer.c_str());
#elif defined(__linux__)
  std::array<char, 4096> lBuffer{};
  const ssize_t lRead = readlink("/proc/self/exe", lBuffer.data(), lBuffer.size() - 1);
  if (lRead <= 0) {
    return {};
  }
  lBuffer[static_cast<std::size_t>(lRead)] = '\0';
  return fs::path(lBuffer.data());
#else
  return {};
#endif
}

static fs::path ResolveOpenCodeBridgeExecutablePath() {
#if defined(_WIN32)
  const std::string lSBridgeBinaryName = "uam_ollama_engine_bridge.exe";
#else
  const std::string lSBridgeBinaryName = "uam_ollama_engine_bridge";
#endif

  std::vector<fs::path> lVecPathCandidates;
  if (const fs::path lPathExecutable = ResolveCurrentExecutablePathForBridge(); !lPathExecutable.empty()) {
    lVecPathCandidates.push_back(lPathExecutable.parent_path() / lSBridgeBinaryName);
  }
  if (char* base_path = SDL_GetBasePath(); base_path != nullptr) {
    lVecPathCandidates.push_back(fs::path(base_path) / lSBridgeBinaryName);
    SDL_free(base_path);
  }
  std::error_code lEcCwd;
  const fs::path lPathCwd = fs::current_path(lEcCwd);
  if (!lEcCwd) {
    lVecPathCandidates.push_back(lPathCwd / lSBridgeBinaryName);
    lVecPathCandidates.push_back(lPathCwd / "build" / lSBridgeBinaryName);
    lVecPathCandidates.push_back(lPathCwd / "build" / "ollama_engine" / lSBridgeBinaryName);
    lVecPathCandidates.push_back(lPathCwd / "build-release" / lSBridgeBinaryName);
    lVecPathCandidates.push_back(lPathCwd / "build-release" / "ollama_engine" / lSBridgeBinaryName);
  }

  for (const fs::path& lPathCandidate : lVecPathCandidates) {
    std::error_code lEcExists;
    if (!lPathCandidate.empty() && fs::exists(lPathCandidate, lEcExists) && !lEcExists) {
      return lPathCandidate;
    }
  }

  return fs::path(lSBridgeBinaryName);
}

static fs::path ResolveOpenCodeConfigPath() {
#if defined(_WIN32)
  if (const std::optional<fs::path> lOptPathHome = ResolveWindowsHomePath(); lOptPathHome.has_value()) {
    return lOptPathHome.value() / ".config" / "opencode" / "opencode.json";
  }
#endif
  if (const char* lPtrCHome = std::getenv("HOME")) {
    const std::string lSHome = Trim(lPtrCHome);
    if (!lSHome.empty()) {
      return fs::path(lSHome) / ".config" / "opencode" / "opencode.json";
    }
  }
  std::error_code lEcCwd;
  const fs::path lPathCwd = fs::current_path(lEcCwd);
  return lEcCwd ? fs::path("opencode.json") : (lPathCwd / ".config" / "opencode" / "opencode.json");
}

static fs::path BuildOpenCodeBridgeReadyFilePath(const AppState& app) {
  const fs::path lPathRuntimeDir = app.data_root / "runtime";
  std::error_code lEc;
  fs::create_directories(lPathRuntimeDir, lEc);
  return lPathRuntimeDir /
         ("opencode_bridge_ready_" + OpenCodeBridgeTimestampStamp() + "_" + OpenCodeBridgeRandomHex(8) + ".json");
}

static JsonValue JsonObjectValue() {
  JsonValue value;
  value.type = JsonValue::Type::Object;
  return value;
}

static JsonValue JsonStringValue(const std::string& text) {
  JsonValue value;
  value.type = JsonValue::Type::String;
  value.string_value = text;
  return value;
}

static JsonValue* EnsureJsonObjectEntry(JsonValue& root, const std::string& key, bool* changed_out = nullptr) {
  if (root.type != JsonValue::Type::Object) {
    root = JsonObjectValue();
    if (changed_out != nullptr) {
      *changed_out = true;
    }
  }
  auto it = root.object_value.find(key);
  if (it == root.object_value.end() || it->second.type != JsonValue::Type::Object) {
    root.object_value[key] = JsonObjectValue();
    if (changed_out != nullptr) {
      *changed_out = true;
    }
  }
  return &root.object_value[key];
}

static bool SetJsonStringEntry(JsonValue& root, const std::string& key, const std::string& value) {
  auto it = root.object_value.find(key);
  if (it != root.object_value.end() && it->second.type == JsonValue::Type::String && it->second.string_value == value) {
    return false;
  }
  root.object_value[key] = JsonStringValue(value);
  return true;
}

static bool RemoveJsonEntry(JsonValue& root, const std::string& key) {
  return root.object_value.erase(key) > 0;
}

static std::string JsonErrorStringMessage(const JsonValue* root_error) {
  if (root_error == nullptr || root_error->type != JsonValue::Type::Object) {
    return "";
  }
  return JsonStringOrEmpty(root_error->Find("error"));
}

struct OpenCodeBridgeReadyInfo {
  std::string endpoint;
  std::string api_base;
  std::string model;
  std::string error;
  bool ok = false;
};

static std::optional<OpenCodeBridgeReadyInfo> ParseOpenCodeBridgeReadyInfo(const std::string& file_text) {
  const std::optional<JsonValue> root_opt = ParseJson(file_text);
  if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object) {
    return std::nullopt;
  }
  OpenCodeBridgeReadyInfo info;
  const JsonValue& root = root_opt.value();
  if (const JsonValue* ok_value = root.Find("ok"); ok_value != nullptr && ok_value->type == JsonValue::Type::Bool) {
    info.ok = ok_value->bool_value;
  }
  info.endpoint = Trim(JsonStringOrEmpty(root.Find("endpoint")));
  info.api_base = Trim(JsonStringOrEmpty(root.Find("api_base")));
  info.model = Trim(JsonStringOrEmpty(root.Find("model")));
  info.error = Trim(JsonStringOrEmpty(root.Find("error")));
  if (!info.ok && info.error.empty()) {
    info.error = JsonErrorStringMessage(&root);
  }
  return info;
}

static size_t CurlAppendToStringCallback(void* ptr, const size_t size, const size_t nmemb, void* userdata) {
  if (ptr == nullptr || userdata == nullptr || size == 0 || nmemb == 0) {
    return 0;
  }
  const size_t total = size * nmemb;
  auto* output = static_cast<std::string*>(userdata);
  output->append(static_cast<const char*>(ptr), total);
  return total;
}

static bool CurlHttpGet(const std::string& url,
                        const std::string& bearer_token,
                        long* status_code_out,
                        std::string* body_out,
                        std::string* error_out) {
  if (status_code_out != nullptr) {
    *status_code_out = 0;
  }
  if (body_out != nullptr) {
    body_out->clear();
  }
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Failed to initialize libcurl.";
    }
    return false;
  }

  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlAppendToStringCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  struct curl_slist* headers = nullptr;
  if (!bearer_token.empty()) {
    const std::string auth = "Authorization: Bearer " + bearer_token;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  const CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  if (headers != nullptr) {
    curl_slist_free_all(headers);
  }
  curl_easy_cleanup(curl);

  if (body_out != nullptr) {
    *body_out = body;
  }
  if (status_code_out != nullptr) {
    *status_code_out = status_code;
  }
  if (code != CURLE_OK) {
    if (error_out != nullptr) {
      *error_out = std::string("HTTP probe failed: ") + curl_easy_strerror(code);
    }
    return false;
  }
  return true;
}

static bool ProbeOpenCodeBridgeHealth(const AppState& app, std::string* error_out = nullptr) {
  const std::string endpoint = Trim(app.opencode_bridge.endpoint);
  if (endpoint.empty()) {
    if (error_out != nullptr) {
      *error_out = "OpenCode bridge endpoint is empty.";
    }
    return false;
  }

  long status_code = 0;
  std::string body;
  std::string curl_error;
  if (!CurlHttpGet(endpoint + "/healthz", "", &status_code, &body, &curl_error)) {
    if (error_out != nullptr) {
      *error_out = curl_error;
    }
    return false;
  }
  if (status_code != 200) {
    if (error_out != nullptr) {
      std::ostringstream out;
      out << "OpenCode bridge health check failed (status " << status_code << ").";
      const std::string trimmed_body = Trim(body);
      if (!trimmed_body.empty()) {
        out << " Body: " << CompactPreview(trimmed_body, 180);
      }
      *error_out = out.str();
    }
    return false;
  }
  return true;
}

#if defined(_WIN32)
static std::wstring OpenCodeBridgeWideFromUtf8(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
  if (wide_len <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(),
                          wide_len) <= 0) {
    return std::wstring();
  }
  return wide;
}

static std::string OpenCodeBridgeQuoteWindowsArg(const std::string& arg) {
  if (arg.empty()) {
    return "\"\"";
  }
  const bool needs_quotes = (arg.find_first_of(" \t\"") != std::string::npos);
  if (!needs_quotes) {
    return arg;
  }
  std::string result = "\"";
  int backslashes = 0;
  for (const char ch : arg) {
    if (ch == '\\') {
      backslashes++;
    } else if (ch == '"') {
      result.append(backslashes * 2 + 1, '\\');
      result.push_back('"');
      backslashes = 0;
    } else {
      if (backslashes > 0) {
        result.append(backslashes, '\\');
        backslashes = 0;
      }
      result.push_back(ch);
    }
  }
  if (backslashes > 0) {
    result.append(backslashes * 2, '\\');
  }
  result.push_back('"');
  return result;
}

static std::string OpenCodeBridgeBuildWindowsCommandLine(const std::vector<std::string>& argv) {
  std::ostringstream out;
  bool first = true;
  for (const std::string& arg : argv) {
    if (!first) {
      out << ' ';
    }
    out << OpenCodeBridgeQuoteWindowsArg(arg);
    first = false;
  }
  return out.str();
}
#endif

static bool StartOpenCodeBridgeProcess(AppState& app, const std::vector<std::string>& argv, std::string* error_out = nullptr) {
  if (argv.empty() || Trim(argv.front()).empty()) {
    if (error_out != nullptr) {
      *error_out = "OpenCode bridge command is empty.";
    }
    return false;
  }

#if defined(_WIN32)
  std::vector<wchar_t> command_line;
  const std::wstring command_w = OpenCodeBridgeWideFromUtf8(OpenCodeBridgeBuildWindowsCommandLine(argv));
  if (command_w.empty()) {
    if (error_out != nullptr) {
      *error_out = "Failed to encode OpenCode bridge command line.";
    }
    return false;
  }
  command_line.assign(command_w.begin(), command_w.end());
  command_line.push_back(L'\0');

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(nullptr,
                                      command_line.data(),
                                      nullptr,
                                      nullptr,
                                      FALSE,
                                      CREATE_NO_WINDOW,
                                      nullptr,
                                      nullptr,
                                      &startup_info,
                                      &process_info);
  if (!created) {
    const DWORD launch_error = GetLastError();
    if (error_out != nullptr) {
      *error_out = "Failed to launch OpenCode bridge process (Win32 error " + std::to_string(launch_error) + ").";
    }
    return false;
  }

  app.opencode_bridge.process_handle = process_info.hProcess;
  app.opencode_bridge.process_thread = process_info.hThread;
  app.opencode_bridge.process_id = process_info.dwProcessId;
#else
  const pid_t child_pid = fork();
  if (child_pid < 0) {
    if (error_out != nullptr) {
      *error_out = "fork failed while starting OpenCode bridge.";
    }
    return false;
  }
  if (child_pid == 0) {
    setsid();
    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO) {
        close(null_fd);
      }
    }

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
    }
    argv_ptrs.push_back(nullptr);
    if (argv.front().find('/') != std::string::npos) {
      execv(argv_ptrs.front(), argv_ptrs.data());
    } else {
      execvp(argv_ptrs.front(), argv_ptrs.data());
    }
    _exit(127);
  }
  app.opencode_bridge.process_id = child_pid;
#endif

  app.opencode_bridge.running = true;
  return true;
}

static bool IsOpenCodeBridgeProcessRunning(AppState& app) {
#if defined(_WIN32)
  if (app.opencode_bridge.process_handle == INVALID_HANDLE_VALUE || app.opencode_bridge.process_handle == nullptr) {
    app.opencode_bridge.running = false;
    return false;
  }
  const DWORD wait_result = WaitForSingleObject(app.opencode_bridge.process_handle, 0);
  if (wait_result == WAIT_TIMEOUT) {
    return true;
  }
  app.opencode_bridge.running = false;
  return false;
#else
  if (app.opencode_bridge.process_id <= 0) {
    app.opencode_bridge.running = false;
    return false;
  }
  int status = 0;
  const pid_t wait_result = waitpid(app.opencode_bridge.process_id, &status, WNOHANG);
  if (wait_result == 0) {
    return true;
  }
  if (wait_result == app.opencode_bridge.process_id || (wait_result < 0 && errno == ECHILD)) {
    app.opencode_bridge.process_id = -1;
    app.opencode_bridge.running = false;
    return false;
  }
  return true;
#endif
}

static void ResetOpenCodeBridgeRuntimeFields(AppState& app, const bool keep_token = true) {
  const std::string preserved_token = keep_token ? app.opencode_bridge.token : "";
  app.opencode_bridge.running = false;
  app.opencode_bridge.healthy = false;
  app.opencode_bridge.endpoint.clear();
  app.opencode_bridge.api_base.clear();
  app.opencode_bridge.selected_model.clear();
  app.opencode_bridge.requested_model.clear();
  app.opencode_bridge.model_folder.clear();
  app.opencode_bridge.ready_file.clear();
  app.opencode_bridge.last_error.clear();
  if (keep_token) {
    app.opencode_bridge.token = preserved_token;
  } else {
    app.opencode_bridge.token.clear();
  }
}

static bool WaitForOpenCodeBridgeReadyFile(AppState& app,
                                           const fs::path& ready_file,
                                           OpenCodeBridgeReadyInfo* info_out,
                                           std::string* error_out = nullptr) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code ec;
    if (fs::exists(ready_file, ec) && !ec) {
      const std::string text = Trim(ReadTextFile(ready_file));
      if (!text.empty()) {
        const std::optional<OpenCodeBridgeReadyInfo> info = ParseOpenCodeBridgeReadyInfo(text);
        if (info.has_value()) {
          if (!info->ok && !info->error.empty()) {
            if (error_out != nullptr) {
              *error_out = info->error;
            }
            return false;
          }
          if (!info->endpoint.empty()) {
            if (info_out != nullptr) {
              *info_out = info.value();
            }
            return true;
          }
        }
      }
    }
    if (!IsOpenCodeBridgeProcessRunning(app)) {
      if (error_out != nullptr) {
        *error_out = "OpenCode bridge process exited before readiness handshake.";
      }
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  if (error_out != nullptr) {
    *error_out = "Timed out waiting for OpenCode bridge ready file.";
  }
  return false;
}

static bool EnsureOpenCodeConfigProvisioned(AppState& app, std::string* error_out = nullptr) {
  const std::string api_base = Trim(app.opencode_bridge.api_base);
  std::string model_id = Trim(app.opencode_bridge.selected_model);
  if (model_id.empty()) {
    model_id = Trim(app.settings.selected_model_id);
  }
  if (api_base.empty()) {
    if (error_out != nullptr) {
      *error_out = "OpenCode bridge API base URL is empty.";
    }
    return false;
  }
  if (model_id.empty()) {
    if (error_out != nullptr) {
      *error_out = "OpenCode bridge has no selected model.";
    }
    return false;
  }

  const fs::path config_path = ResolveOpenCodeConfigPath();
  std::error_code ec;
  fs::create_directories(config_path.parent_path(), ec);
  if (ec) {
    if (error_out != nullptr) {
      *error_out = "Failed to create OpenCode config directory: " + ec.message();
    }
    return false;
  }

  JsonValue root = JsonObjectValue();
  bool changed = false;
  bool parse_failed = false;

  const std::string existing_text = ReadTextFile(config_path);
  if (!Trim(existing_text).empty()) {
    const std::optional<JsonValue> parsed = ParseJson(existing_text);
    if (!parsed.has_value() || parsed->type != JsonValue::Type::Object) {
      parse_failed = true;
      const fs::path backup_path =
          config_path.parent_path() /
          (config_path.stem().string() + ".backup-" + OpenCodeBridgeTimestampStamp() + config_path.extension().string());
      std::error_code copy_ec;
      fs::copy_file(config_path, backup_path, fs::copy_options::overwrite_existing, copy_ec);
      if (copy_ec) {
        app.status_line = "OpenCode config parse failed; backup copy also failed: " + copy_ec.message();
      } else {
        app.status_line = "OpenCode config parse failed; created backup at " + backup_path.string() + ".";
      }
      changed = true;
    } else {
      root = parsed.value();
    }
  }

  JsonValue* provider = EnsureJsonObjectEntry(root, "provider", &changed);
  JsonValue* uam_local = EnsureJsonObjectEntry(*provider, "uam_local", &changed);
  changed = SetJsonStringEntry(*uam_local, "npm", "@ai-sdk/openai-compatible") || changed;
  changed = SetJsonStringEntry(*uam_local, "name", "UAM Local (Ollama Engine)") || changed;
  changed = SetJsonStringEntry(*uam_local, "api", api_base) || changed;
  JsonValue* options = EnsureJsonObjectEntry(*uam_local, "options", &changed);
  changed = SetJsonStringEntry(*options, "baseURL", api_base) || changed;
  if (!app.opencode_bridge.token.empty()) {
    changed = SetJsonStringEntry(*options, "apiKey", app.opencode_bridge.token) || changed;
  } else {
    changed = RemoveJsonEntry(*options, "apiKey") || changed;
  }
  JsonValue* models = EnsureJsonObjectEntry(*uam_local, "models", &changed);
  JsonValue* model_entry = EnsureJsonObjectEntry(*models, model_id, &changed);
  fs::path model_path(model_id);
  std::string model_display_name = model_path.filename().string();
  if (model_display_name.empty()) {
    model_display_name = model_id;
  }
  changed = SetJsonStringEntry(*model_entry, "name", model_display_name) || changed;
  changed = SetJsonStringEntry(root, "model", "uam_local/" + model_id) || changed;

  if (!changed && !parse_failed) {
    return true;
  }

  if (!WriteTextFile(config_path, SerializeJson(root))) {
    if (error_out != nullptr) {
      *error_out = "Failed to write OpenCode config file: " + config_path.string();
    }
    return false;
  }
  return true;
}

static std::vector<std::string> BuildOpenCodeBridgeArgv(const fs::path& bridge_executable,
                                                         const fs::path& model_folder,
                                                         const std::string& requested_model,
                                                         const std::string& token,
                                                         const fs::path& ready_file) {
  std::vector<std::string> argv;
  argv.push_back(bridge_executable.string());
  argv.push_back("--host");
  argv.push_back("127.0.0.1");
  argv.push_back("--port");
  argv.push_back("0");
  argv.push_back("--model-folder");
  argv.push_back(model_folder.string());
  if (!Trim(requested_model).empty()) {
    argv.push_back("--default-model");
    argv.push_back(Trim(requested_model));
  }
  if (!Trim(token).empty()) {
    argv.push_back("--token");
    argv.push_back(token);
  }
  argv.push_back("--ready-file");
  argv.push_back(ready_file.string());
  return argv;
}

static bool StartOpenCodeBridge(AppState& app,
                                const fs::path& model_folder,
                                const std::string& requested_model,
                                std::string* error_out = nullptr) {
  fs::path normalized_model_folder = NormalizeAbsolutePath(model_folder);
  if (normalized_model_folder.empty()) {
    normalized_model_folder = model_folder;
  }

  const fs::path bridge_executable = ResolveOpenCodeBridgeExecutablePath();
  if (bridge_executable.empty()) {
    if (error_out != nullptr) {
      *error_out = "Could not resolve uam_ollama_engine_bridge executable.";
    }
    return false;
  }

  if (app.opencode_bridge.token.empty()) {
    app.opencode_bridge.token = OpenCodeBridgeRandomHex(48);
  }
  const fs::path ready_file = BuildOpenCodeBridgeReadyFilePath(app);
  std::error_code rm_ec;
  fs::remove(ready_file, rm_ec);

  const std::vector<std::string> argv =
      BuildOpenCodeBridgeArgv(bridge_executable, normalized_model_folder, requested_model, app.opencode_bridge.token, ready_file);
  if (!StartOpenCodeBridgeProcess(app, argv, error_out)) {
    app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode bridge process launch failed.";
    return false;
  }

  OpenCodeBridgeReadyInfo ready_info;
  std::string ready_error;
  if (!WaitForOpenCodeBridgeReadyFile(app, ready_file, &ready_info, &ready_error)) {
    StopOpenCodeBridge(app);
    if (error_out != nullptr) {
      *error_out = ready_error.empty() ? "OpenCode bridge did not become ready." : ready_error;
    }
    app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode bridge startup failed.";
    return false;
  }

  app.opencode_bridge.endpoint = ready_info.endpoint;
  app.opencode_bridge.api_base = ready_info.api_base.empty() ? (ready_info.endpoint + "/v1") : ready_info.api_base;
  app.opencode_bridge.selected_model = ready_info.model.empty() ? Trim(requested_model) : ready_info.model;
  app.opencode_bridge.requested_model = Trim(requested_model);
  app.opencode_bridge.model_folder = normalized_model_folder.string();
  app.opencode_bridge.ready_file = ready_file.string();
  app.opencode_bridge.running = true;
  app.opencode_bridge.healthy = false;

  std::string health_error;
  if (!ProbeOpenCodeBridgeHealth(app, &health_error)) {
    StopOpenCodeBridge(app);
    if (error_out != nullptr) {
      *error_out = health_error.empty() ? "OpenCode bridge health check failed." : health_error;
    }
    app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode bridge health check failed.";
    return false;
  }
  app.opencode_bridge.healthy = true;

  std::string config_error;
  if (!EnsureOpenCodeConfigProvisioned(app, &config_error)) {
    StopOpenCodeBridge(app);
    if (error_out != nullptr) {
      *error_out = config_error.empty() ? "OpenCode config provisioning failed." : config_error;
    }
    app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode config provisioning failed.";
    return false;
  }

  app.opencode_bridge.last_error.clear();
  return true;
}

static void StopOpenCodeBridge(AppState& app) {
#if defined(_WIN32)
  if (app.opencode_bridge.process_handle != INVALID_HANDLE_VALUE && app.opencode_bridge.process_handle != nullptr) {
    const DWORD wait_result = WaitForSingleObject(app.opencode_bridge.process_handle, 0);
    if (wait_result == WAIT_TIMEOUT) {
      TerminateProcess(app.opencode_bridge.process_handle, 1);
      WaitForSingleObject(app.opencode_bridge.process_handle, 1000);
    }
    CloseHandle(app.opencode_bridge.process_handle);
    app.opencode_bridge.process_handle = INVALID_HANDLE_VALUE;
  }
  if (app.opencode_bridge.process_thread != INVALID_HANDLE_VALUE && app.opencode_bridge.process_thread != nullptr) {
    CloseHandle(app.opencode_bridge.process_thread);
    app.opencode_bridge.process_thread = INVALID_HANDLE_VALUE;
  }
  app.opencode_bridge.process_id = 0;
#else
  if (app.opencode_bridge.process_id > 0) {
    const pid_t pid = app.opencode_bridge.process_id;
    int status = 0;
    kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t wait_result = waitpid(pid, &status, WNOHANG);
      if (wait_result == pid || (wait_result < 0 && errno == ECHILD)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const pid_t wait_result = waitpid(pid, &status, WNOHANG);
    if (wait_result == 0) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, WNOHANG);
    }
  }
  app.opencode_bridge.process_id = -1;
#endif

  const std::string preserved_token = app.opencode_bridge.token;
  ResetOpenCodeBridgeRuntimeFields(app, true);
  app.opencode_bridge.token = preserved_token;
}

static bool RestartOpenCodeBridgeIfModelChanged(AppState& app, std::string* error_out) {
  const fs::path desired_model_folder_path = ResolveRagModelFolder(app);
  fs::path desired_model_folder_norm = NormalizeAbsolutePath(desired_model_folder_path);
  if (desired_model_folder_norm.empty()) {
    desired_model_folder_norm = desired_model_folder_path;
  }
  const std::string desired_model_folder = desired_model_folder_norm.string();
  const std::string desired_requested_model = Trim(app.settings.selected_model_id);

  const bool process_running = IsOpenCodeBridgeProcessRunning(app);
  const bool signature_matches =
      process_running &&
      app.opencode_bridge.model_folder == desired_model_folder &&
      app.opencode_bridge.requested_model == desired_requested_model &&
      !Trim(app.opencode_bridge.endpoint).empty() &&
      !Trim(app.opencode_bridge.api_base).empty();

  if (signature_matches) {
    std::string health_error;
    if (!ProbeOpenCodeBridgeHealth(app, &health_error)) {
      StopOpenCodeBridge(app);
      if (!StartOpenCodeBridge(app, desired_model_folder_norm, desired_requested_model, error_out)) {
        return false;
      }
      return true;
    }
    app.opencode_bridge.healthy = true;
    std::string config_error;
    if (!EnsureOpenCodeConfigProvisioned(app, &config_error)) {
      if (error_out != nullptr) {
        *error_out = config_error;
      }
      app.opencode_bridge.last_error = config_error;
      return false;
    }
    return true;
  }

  StopOpenCodeBridge(app);
  return StartOpenCodeBridge(app, desired_model_folder_norm, desired_requested_model, error_out);
}

static bool EnsureOpenCodeBridgeRunning(AppState& app, std::string* error_out) {
  const bool ok = RestartOpenCodeBridgeIfModelChanged(app, error_out);
  if (!ok && error_out != nullptr && error_out->empty()) {
    *error_out = "Failed to ensure OpenCode bridge is running.";
  }
  if (!ok) {
    app.opencode_bridge.healthy = false;
  }
  return ok;
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
  chat.provider_id = provider.id;
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

static bool IsLegacyBuiltInProviderId(const std::string& provider_id) {
  const std::string lowered = ToLowerAscii(Trim(provider_id));
  return lowered == "gemini" || lowered == "codex" || lowered == "claude" || lowered == "opencode";
}

static bool IsGeminiProviderId(const std::string& provider_id) {
  const std::string lowered = ToLowerAscii(Trim(provider_id));
  return lowered == "gemini" || lowered == "gemini-cli" || lowered == "gemini-structured";
}

static std::string MapLegacyProviderId(const std::string& provider_id, const bool prefer_cli_for_gemini) {
  const std::string trimmed = Trim(provider_id);
  const std::string lowered = ToLowerAscii(trimmed);
  if (lowered == "gemini") {
    return prefer_cli_for_gemini ? "gemini-cli" : "gemini-structured";
  }
  if (lowered == "codex") {
    return "codex-cli";
  }
  if (lowered == "claude") {
    return "claude-cli";
  }
  if (lowered == "opencode") {
    return "opencode-cli";
  }
  return trimmed;
}

static std::string DefaultGeminiProviderIdForLegacyViewHint(const AppState& app) {
  return (app.center_view_mode == CenterViewMode::CliConsole) ? "gemini-cli" : "gemini-structured";
}

static bool ChatHasCliViewHint(const AppState& app, const ChatSession& chat) {
  for (const auto& terminal : app.cli_terminals) {
    if (terminal == nullptr || terminal->attached_chat_id != chat.id) {
      continue;
    }
    if (terminal->running || terminal->should_launch || terminal->input_ready || terminal->generation_in_progress) {
      return true;
    }
  }
  return false;
}

static bool ShouldShowProviderProfileInUi(const ProviderProfile& profile) {
  return !IsLegacyBuiltInProviderId(profile.id);
}

static bool MigrateProviderProfilesToFixedModeIds(AppState& app) {
  bool changed = false;
  std::vector<ProviderProfile> migrated;
  migrated.reserve(app.provider_profiles.size());
  std::unordered_set<std::string> seen_ids;
  for (ProviderProfile profile : app.provider_profiles) {
    const std::string original_id = Trim(profile.id);
    const std::string mapped_id = MapLegacyProviderId(original_id, false);
    if (mapped_id != original_id) {
      changed = true;
    }
    if (mapped_id.empty()) {
      changed = true;
      continue;
    }
    profile.id = mapped_id;
    const auto assign_if_changed = [&](auto& field, const auto& value) {
      if (field != value) {
        field = value;
        changed = true;
      }
    };
    if (mapped_id == "gemini-structured") {
      assign_if_changed(profile.output_mode, std::string("structured"));
      assign_if_changed(profile.supports_interactive, false);
      if (Trim(profile.command_template).empty() || profile.command_template == "gemini {resume} {flags} {prompt}") {
        assign_if_changed(profile.command_template, std::string("gemini {resume} {flags} -p {prompt}"));
      }
      if (Trim(profile.history_adapter).empty()) {
        assign_if_changed(profile.history_adapter, std::string("gemini-cli-json"));
      }
      if (Trim(profile.prompt_bootstrap).empty()) {
        assign_if_changed(profile.prompt_bootstrap, std::string("gemini-at-path"));
      }
      if (Trim(profile.prompt_bootstrap_path).empty()) {
        assign_if_changed(profile.prompt_bootstrap_path, std::string("@.gemini/gemini.md"));
      }
    } else if (mapped_id == "gemini-cli") {
      assign_if_changed(profile.output_mode, std::string("cli"));
      assign_if_changed(profile.supports_interactive, true);
      if (Trim(profile.command_template).empty()) {
        assign_if_changed(profile.command_template, std::string("gemini {resume} {flags} {prompt}"));
      }
      if (Trim(profile.history_adapter).empty()) {
        assign_if_changed(profile.history_adapter, std::string("gemini-cli-json"));
      }
      if (Trim(profile.prompt_bootstrap).empty()) {
        assign_if_changed(profile.prompt_bootstrap, std::string("gemini-at-path"));
      }
      if (Trim(profile.prompt_bootstrap_path).empty()) {
        assign_if_changed(profile.prompt_bootstrap_path, std::string("@.gemini/gemini.md"));
      }
    } else if (mapped_id == "codex-cli" || mapped_id == "claude-cli" || mapped_id == "opencode-cli" ||
               mapped_id == "opencode-local") {
      assign_if_changed(profile.output_mode, std::string("cli"));
      assign_if_changed(profile.supports_interactive, true);
    } else if (mapped_id == "ollama-engine") {
      assign_if_changed(profile.output_mode, std::string("structured"));
      assign_if_changed(profile.supports_interactive, false);
    }
    const std::string dedupe_key = ToLowerAscii(profile.id);
    if (!seen_ids.insert(dedupe_key).second) {
      changed = true;
      continue;
    }
    migrated.push_back(std::move(profile));
  }
  if (migrated.size() != app.provider_profiles.size()) {
    changed = true;
  }
  app.provider_profiles = std::move(migrated);
  ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);

  std::vector<ProviderProfile> filtered;
  filtered.reserve(app.provider_profiles.size());
  for (const ProviderProfile& profile : app.provider_profiles) {
    if (IsLegacyBuiltInProviderId(profile.id)) {
      changed = true;
      continue;
    }
    filtered.push_back(profile);
  }
  app.provider_profiles = std::move(filtered);
  ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
  return changed;
}

static bool MigrateActiveProviderIdToFixedModes(AppState& app) {
  bool changed = false;
  const std::string mapped_id = MapLegacyProviderId(app.settings.active_provider_id, app.center_view_mode == CenterViewMode::CliConsole);
  if (mapped_id != app.settings.active_provider_id) {
    app.settings.active_provider_id = mapped_id;
    changed = true;
  }
  if (Trim(app.settings.active_provider_id).empty()) {
    app.settings.active_provider_id = DefaultGeminiProviderIdForLegacyViewHint(app);
    changed = true;
  }
  if (ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id) == nullptr) {
    app.settings.active_provider_id = DefaultGeminiProviderIdForLegacyViewHint(app);
    if (ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id) == nullptr &&
        !app.provider_profiles.empty()) {
      app.settings.active_provider_id = app.provider_profiles.front().id;
    }
    changed = true;
  }
  return changed;
}

static ProviderProfile* ActiveProvider(AppState& app) {
  ProviderProfile* found = ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
  if (found != nullptr) {
    return found;
  }
  ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
  app.settings.active_provider_id = "gemini-structured";
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

static const ProviderProfile* ProviderForChat(const AppState& app, const ChatSession& chat) {
  const std::string preferred = Trim(chat.provider_id);
  if (!preferred.empty()) {
    if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, preferred); profile != nullptr) {
      return profile;
    }
  }
  return ActiveProvider(app);
}

static const ProviderProfile& ProviderForChatOrDefault(const AppState& app, const ChatSession& chat) {
  if (const ProviderProfile* profile = ProviderForChat(app, chat); profile != nullptr) {
    return *profile;
  }
  return ActiveProviderOrDefault(app);
}

static bool ActiveProviderUsesGeminiHistory(const AppState& app) {
  const ProviderProfile* profile = ActiveProvider(app);
  return profile != nullptr && ProviderRuntime::SupportsGeminiJsonHistory(*profile);
}

static bool ActiveProviderUsesInternalEngine(const AppState& app) {
  const ProviderProfile* profile = ActiveProvider(app);
  return profile != nullptr && ProviderRuntime::UsesInternalEngine(*profile);
}

static bool ChatUsesGeminiHistory(const AppState& app, const ChatSession& chat) {
  const ProviderProfile* profile = ProviderForChat(app, chat);
  return profile != nullptr && ProviderRuntime::SupportsGeminiJsonHistory(*profile);
}

static bool ChatUsesInternalEngine(const AppState& app, const ChatSession& chat) {
  const ProviderProfile* profile = ProviderForChat(app, chat);
  return profile != nullptr && ProviderRuntime::UsesInternalEngine(*profile);
}

static bool ChatUsesCliOutput(const AppState& app, const ChatSession& chat) {
  const ProviderProfile* profile = ProviderForChat(app, chat);
  return profile != nullptr && ProviderRuntime::UsesCliOutput(*profile);
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
  if (candidate.provider_id != existing.provider_id) {
    return !candidate.provider_id.empty();
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
  std::unordered_map<std::string, std::size_t> index_by_native_session_id;

  for (ChatSession& chat : chats) {
    chat.id = Trim(chat.id);
    if (chat.id.empty()) {
      continue;
    }
    const std::string native_session_id = Trim(chat.native_session_id);
    const bool has_native_identity = chat.uses_native_session && !native_session_id.empty();
    const std::string native_key = has_native_identity ? ("native:" + native_session_id) : std::string{};

    if (has_native_identity) {
      const auto native_it = index_by_native_session_id.find(native_key);
      if (native_it != index_by_native_session_id.end()) {
        ChatSession& existing = deduped[native_it->second];
        if (ShouldReplaceChatForDuplicateId(chat, existing) || existing.id != existing.native_session_id) {
          existing = std::move(chat);
          if (existing.id != existing.native_session_id && !existing.native_session_id.empty()) {
            existing.id = existing.native_session_id;
          }
          index_by_id[existing.id] = native_it->second;
        }
        continue;
      }
    }

    const auto it = index_by_id.find(chat.id);
    if (it == index_by_id.end()) {
      if (has_native_identity && chat.id != native_session_id) {
        chat.id = native_session_id;
      }
      const std::size_t next_index = deduped.size();
      index_by_id[chat.id] = next_index;
      if (has_native_identity) {
        index_by_native_session_id[native_key] = next_index;
      }
      deduped.push_back(std::move(chat));
      continue;
    }
    ChatSession& existing = deduped[it->second];
    if (ShouldReplaceChatForDuplicateId(chat, existing)) {
      existing = std::move(chat);
      if (existing.uses_native_session && !existing.native_session_id.empty() && existing.id != existing.native_session_id) {
        existing.id = existing.native_session_id;
      }
    }
    if (existing.uses_native_session && !existing.native_session_id.empty()) {
      index_by_native_session_id["native:" + existing.native_session_id] = it->second;
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
  app.settings.runtime_backend = ActiveProviderUsesInternalEngine(app) ? "ollama-engine" : "provider-cli";
  app.settings.vector_db_backend = (app.settings.vector_db_backend == "none") ? "none" : "ollama-engine";
  app.settings.selected_model_id = Trim(app.settings.selected_model_id);
  app.settings.models_folder_directory = Trim(app.settings.models_folder_directory);
  app.settings.selected_vector_model_id = Trim(app.settings.selected_vector_model_id);
  app.settings.vector_database_name_override = NormalizeVectorDatabaseName(app.settings.vector_database_name_override);
  app.settings.cli_idle_timeout_seconds = std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600);
  app.settings.rag_top_k = std::clamp(app.settings.rag_top_k, 1, 20);
  app.settings.rag_max_snippet_chars = std::clamp(app.settings.rag_max_snippet_chars, 120, 4000);
  app.settings.rag_max_file_bytes = std::clamp(app.settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024);
  app.settings.rag_scan_max_tokens = std::clamp(app.settings.rag_scan_max_tokens, 0, 32768);
  ClampWindowSettings(app.settings);
  SyncRagServiceConfig(app);
  if (app.opencode_bridge.running) {
    fs::path desired_model_folder = NormalizeAbsolutePath(ResolveRagModelFolder(app));
    if (desired_model_folder.empty()) {
      desired_model_folder = ResolveRagModelFolder(app);
    }
    const std::string desired_model = Trim(app.settings.selected_model_id);
    if (app.opencode_bridge.model_folder != desired_model_folder.string() ||
        app.opencode_bridge.requested_model != desired_model) {
      std::string bridge_error;
      if (!RestartOpenCodeBridgeIfModelChanged(app, &bridge_error) && !bridge_error.empty()) {
        app.status_line = bridge_error;
      }
    }
  }
  RefreshRememberedSelection(app);
  SettingsStore::Save(SettingsFilePath(app), app.settings, app.center_view_mode);
}

static void LoadSettings(AppState& app) {
  SettingsStore::Load(SettingsFilePath(app), app.settings, app.center_view_mode);
  app.settings.vector_db_backend = (app.settings.vector_db_backend == "none") ? "none" : "ollama-engine";
  app.settings.selected_model_id = Trim(app.settings.selected_model_id);
  app.settings.models_folder_directory = Trim(app.settings.models_folder_directory);
  app.settings.selected_vector_model_id = Trim(app.settings.selected_vector_model_id);
  app.settings.vector_database_name_override = NormalizeVectorDatabaseName(app.settings.vector_database_name_override);
  app.settings.cli_idle_timeout_seconds = std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600);
  if (Trim(app.settings.prompt_profile_root_path).empty()) {
    app.settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
  }
  SyncRagServiceConfig(app);
  app.rag_manual_query_max = std::clamp(app.settings.rag_top_k, 1, 20);
  app.rag_manual_query_min = 1;
}

static bool SaveChat(const AppState& app, const ChatSession& chat) {
  return ChatRepository::SaveChat(app.data_root, chat);
}

static std::vector<ChatSession> LoadChats(const AppState& app) {
  return ChatRepository::LoadLocalChats(app.data_root);
}

static bool MigrateChatProviderBindingsToFixedModes(AppState& app) {
  bool changed = false;
  const std::string fallback_provider_id =
      Trim(app.settings.active_provider_id).empty() ? DefaultGeminiProviderIdForLegacyViewHint(app) : app.settings.active_provider_id;
  for (ChatSession& chat : app.chats) {
    const std::string original_provider_id = chat.provider_id;
    const bool prefer_cli_for_legacy_gemini = ChatHasCliViewHint(app, chat) || app.center_view_mode == CenterViewMode::CliConsole;
    std::string mapped_provider_id = MapLegacyProviderId(original_provider_id, prefer_cli_for_legacy_gemini);
    if (mapped_provider_id.empty()) {
      mapped_provider_id = fallback_provider_id;
    }
    if (ProviderProfileStore::FindById(app.provider_profiles, mapped_provider_id) == nullptr) {
      mapped_provider_id = fallback_provider_id;
    }
    if (mapped_provider_id.empty()) {
      continue;
    }
    if (mapped_provider_id != original_provider_id) {
      chat.provider_id = mapped_provider_id;
      if (chat.updated_at.empty()) {
        chat.updated_at = TimestampNow();
      }
      if (!SaveChat(app, chat)) {
        app.status_line = "Failed to persist migrated provider id for chat " + CompactPreview(chat.id, 24) + ".";
      }
      changed = true;
    }
  }
  return changed;
}

static ChatSession CreateNewChat(const std::string& folder_id, const std::string& provider_id) {
  ChatSession chat;
  chat.id = NewSessionId();
  chat.provider_id = provider_id;
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

static bool MessagesEquivalentForNativeLinking(const Message& local_message, const Message& native_message) {
  return local_message.role == native_message.role && Trim(local_message.content) == Trim(native_message.content);
}

static bool IsMessagePrefixForNativeLinking(const std::vector<Message>& local_messages,
                                            const std::vector<Message>& native_messages) {
  if (local_messages.empty() || local_messages.size() > native_messages.size()) {
    return false;
  }
  for (std::size_t i = 0; i < local_messages.size(); ++i) {
    if (!MessagesEquivalentForNativeLinking(local_messages[i], native_messages[i])) {
      return false;
    }
  }
  return true;
}

static std::optional<std::string> SingleSessionIdFromSet(const std::unordered_set<std::string>& session_ids) {
  if (session_ids.size() != 1) {
    return std::nullopt;
  }
  return *session_ids.begin();
}

static std::optional<std::string> InferNativeSessionIdForLocalDraft(const ChatSession& local_chat,
                                                                    const std::vector<ChatSession>& native_chats) {
  if (!IsLocalDraftChatId(local_chat.id) || local_chat.messages.size() < 2) {
    return std::nullopt;
  }

  std::unordered_set<std::string> exact_match_ids;
  std::unordered_set<std::string> prefix_match_ids;
  for (const ChatSession& native_chat : native_chats) {
    if (!native_chat.uses_native_session || native_chat.native_session_id.empty()) {
      continue;
    }
    if (!IsMessagePrefixForNativeLinking(local_chat.messages, native_chat.messages)) {
      continue;
    }
    prefix_match_ids.insert(native_chat.native_session_id);
    if (local_chat.messages.size() == native_chat.messages.size()) {
      exact_match_ids.insert(native_chat.native_session_id);
    }
  }

  if (const auto exact_match = SingleSessionIdFromSet(exact_match_ids); exact_match.has_value()) {
    return exact_match;
  }
  if (local_chat.messages.size() >= 3) {
    return SingleSessionIdFromSet(prefix_match_ids);
  }
  return std::nullopt;
}

static bool PersistLocalDraftNativeSessionLink(const AppState& app,
                                               ChatSession& local_chat,
                                               const std::string& native_session_id) {
  const std::string session_id = Trim(native_session_id);
  if (session_id.empty() || !IsLocalDraftChatId(local_chat.id)) {
    return false;
  }

  bool changed = false;
  if (!local_chat.uses_native_session) {
    local_chat.uses_native_session = true;
    changed = true;
  }
  if (local_chat.native_session_id != session_id) {
    local_chat.native_session_id = session_id;
    changed = true;
  }
  if (!changed) {
    return true;
  }

  if (local_chat.updated_at.empty()) {
    local_chat.updated_at = TimestampNow();
  }
  return SaveChat(app, local_chat);
}

static std::string ResolveResumeSessionIdForChat(const AppState& app, const ChatSession& chat) {
  if (!ChatUsesGeminiHistory(app, chat)) {
    return "";
  }
  const auto session_exists = [&](const std::string& session_id) {
    if (session_id.empty() || app.gemini_chats_dir.empty()) {
      return false;
    }
    std::error_code ec;
    return fs::exists(app.gemini_chats_dir / (session_id + ".json"), ec) && !ec;
  };
  if (chat.uses_native_session) {
    if (!chat.native_session_id.empty()) {
      return session_exists(chat.native_session_id) ? chat.native_session_id : "";
    }
    if (!chat.id.empty()) {
      return session_exists(chat.id) ? chat.id : "";
    }
    return "";
  }

  // Legacy compatibility: older local snapshots of native chats may not persist native flags.
  if (!chat.messages.empty() && !chat.id.empty() && !IsLocalDraftChatId(chat.id)) {
    return session_exists(chat.id) ? chat.id : "";
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

static bool RuntimeUsesLocalEngine(const AppState& app) {
  return ActiveProviderUsesInternalEngine(app);
}

static bool EnsureLocalRuntimeModelLoaded(AppState& app, std::string* error_out = nullptr) {
  const fs::path model_folder = ResolveRagModelFolder(app);
  app.local_runtime_engine.SetModelFolder(model_folder);
  app.local_runtime_engine.SetEmbeddingDimensions(256);
  if (Trim(app.settings.selected_model_id).empty()) {
    return true;
  }
  if (app.loaded_runtime_model_id == app.settings.selected_model_id) {
    return true;
  }
  if (!app.local_runtime_engine.Load(app.settings.selected_model_id, error_out)) {
    return false;
  }
  app.loaded_runtime_model_id = app.settings.selected_model_id;
  return true;
}

static bool ProviderUsesOpenCodeLocalBridge(const ProviderProfile& provider) {
  return ToLowerAscii(Trim(provider.id)) == "opencode-local";
}

static bool EnsureSelectedLocalRuntimeModelForProvider(AppState& app) {
  const fs::path model_folder = ResolveRagModelFolder(app);
  app.local_runtime_engine.SetModelFolder(model_folder);
  const std::vector<std::string> runtime_models = app.local_runtime_engine.ListModels();
  const std::string selected_model = Trim(app.settings.selected_model_id);
  const bool selected_model_valid =
      !selected_model.empty() &&
      std::find(runtime_models.begin(), runtime_models.end(), selected_model) != runtime_models.end();
  if (selected_model_valid) {
    return true;
  }

  if (runtime_models.empty()) {
    app.runtime_model_selection_id.clear();
    app.status_line = "No local runtime models found. Add one, then retry.";
  } else {
    if (Trim(app.runtime_model_selection_id).empty() ||
        std::find(runtime_models.begin(), runtime_models.end(), app.runtime_model_selection_id) == runtime_models.end()) {
      app.runtime_model_selection_id = runtime_models.front();
    }
    if (selected_model.empty()) {
      app.status_line = "Select a local runtime model to continue.";
    } else {
      app.status_line = "Selected model is unavailable. Choose a local runtime model to continue.";
    }
  }
  app.open_runtime_model_selection_popup = true;
  return false;
}

enum class TemplatePreflightOutcome {
  ReadyWithTemplate,
  ReadyWithoutTemplate,
  BlockingError
};

static TemplatePreflightOutcome PreflightWorkspaceTemplateForChat(AppState& app,
                                                                  const ProviderProfile& provider,
                                                                  const ChatSession& chat,
                                                                  std::string* bootstrap_prompt_out = nullptr,
                                                                  std::string* status_out = nullptr) {
  RefreshTemplateCatalog(app);

  std::string effective_template_id = chat.template_override_id;
  if (effective_template_id.empty()) {
    effective_template_id = app.settings.default_prompt_profile_id;
  }
  if (effective_template_id.empty()) {
    effective_template_id = app.settings.default_gemini_template_id;
  }
  if (effective_template_id.empty()) {
    if (status_out != nullptr) {
      *status_out = "No prompt profile selected. Set a default in Templates.";
    }
    return TemplatePreflightOutcome::ReadyWithoutTemplate;
  }

  const TemplateCatalogEntry* entry = FindTemplateEntryById(app, effective_template_id);
  if (entry == nullptr) {
    if (status_out != nullptr) {
      *status_out = "Selected prompt profile is missing: " + effective_template_id + ". Choose one in Templates.";
    }
    app.open_template_manager_popup = true;
    return TemplatePreflightOutcome::BlockingError;
  }

  if (ProviderRuntime::UsesGeminiPathBootstrap(provider)) {
    if (!EnsureWorkspaceGeminiLayout(app, chat, status_out)) {
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
    if (bootstrap_prompt_out != nullptr) {
      *bootstrap_prompt_out = provider.prompt_bootstrap_path.empty() ? "@.gemini/gemini.md" : provider.prompt_bootstrap_path;
    }
    return TemplatePreflightOutcome::ReadyWithTemplate;
  }

  if (bootstrap_prompt_out != nullptr) {
    *bootstrap_prompt_out = ReadTextFile(entry->absolute_path);
    if (bootstrap_prompt_out->empty()) {
      if (status_out != nullptr) {
        *status_out = "Selected prompt profile is empty.";
      }
      return TemplatePreflightOutcome::ReadyWithoutTemplate;
    }
  }
  return TemplatePreflightOutcome::ReadyWithTemplate;
}

static bool QueueGeminiPromptForChat(AppState& app,
                                     ChatSession& chat,
                                     const std::string& prompt,
                                     const bool template_control_message = false) {
  if (HasPendingCallForChat(app, chat.id)) {
    app.status_line = "Provider command already running for this chat.";
    return false;
  }
  const std::string prompt_text = Trim(prompt);
  if (prompt_text.empty()) {
    app.status_line = "Prompt is empty.";
    return false;
  }

  const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);
  const bool use_local_runtime = ChatUsesInternalEngine(app, chat);
  const bool use_opencode_local_bridge = ProviderUsesOpenCodeLocalBridge(provider);
  std::string template_status;
  std::string bootstrap_prompt;
  TemplatePreflightOutcome template_outcome = TemplatePreflightOutcome::ReadyWithoutTemplate;
  if (!template_control_message || !chat.gemini_md_bootstrapped) {
    template_outcome = PreflightWorkspaceTemplateForChat(app, provider, chat, &bootstrap_prompt, &template_status);
    if (template_outcome == TemplatePreflightOutcome::BlockingError) {
      app.status_line = template_status.empty() ? "Prompt profile preflight failed." : template_status;
      return false;
    }
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
  if (should_bootstrap_template && !bootstrap_prompt.empty()) {
    runtime_prompt = bootstrap_prompt + "\n\n" + runtime_prompt;
  }

  if ((use_local_runtime || use_opencode_local_bridge) && !EnsureSelectedLocalRuntimeModelForProvider(app)) {
    return false;
  }

  AddMessage(chat, MessageRole::User, prompt_text);
  SaveAndUpdateStatus(app, chat, "Prompt queued for provider runtime.", "Saved message locally, but failed to persist chat data.");

  const bool use_shared_cli_session =
      !use_local_runtime && ProviderRuntime::UsesCliOutput(provider) && provider.supports_interactive;
  if (!use_local_runtime && ProviderRuntime::UsesCliOutput(provider) && !provider.supports_interactive) {
    AddMessage(chat, MessageRole::System, "Provider is configured for CLI output but has no interactive runtime command.");
    SaveChat(app, chat);
    app.status_line = "Provider runtime configuration error.";
    return false;
  }
  if (use_shared_cli_session) {
    std::string terminal_error;
    if (!SendPromptToCliRuntime(app, chat, runtime_prompt, &terminal_error)) {
      AddMessage(chat, MessageRole::System,
                 "Provider terminal send failed: " + (terminal_error.empty() ? std::string("unknown error") : terminal_error));
      SaveChat(app, chat);
      app.status_line = "Provider terminal send failed.";
      return false;
    }
    if (should_bootstrap_template) {
      chat.gemini_md_bootstrapped = true;
      SaveChat(app, chat);
    }
    if (template_control_message) {
      app.status_line = "Prompt profile updated in live provider terminal session.";
    } else {
      app.status_line = "Prompt sent to live provider terminal session.";
    }
    app.scroll_to_bottom = true;
    return true;
  }

  if (use_local_runtime) {
    std::string load_error;
    if (!EnsureLocalRuntimeModelLoaded(app, &load_error)) {
      AddMessage(chat, MessageRole::System,
                 "Local runtime model load failed: " + (load_error.empty() ? std::string("unknown error") : load_error));
      SaveChat(app, chat);
      app.status_line = "Local runtime model load failed.";
      return false;
    }
    const ollama_engine::SendMessageResponse response = app.local_runtime_engine.SendMessage(runtime_prompt);
    if (response.pbOk) {
      AddMessage(chat, MessageRole::Assistant, response.pSText);
      SaveAndUpdateStatus(app, chat, "Local response generated.", "Local response generated, but chat save failed.");
      app.scroll_to_bottom = true;
      return true;
    }
    AddMessage(chat, MessageRole::System, "Local runtime error: " + response.pSError);
    SaveChat(app, chat);
    app.status_line = "Local runtime command failed.";
    app.scroll_to_bottom = true;
    return false;
  }

  std::vector<ChatSession> native_before;
  if (ChatUsesGeminiHistory(app, chat)) {
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
    app.status_line = "Prompt profile updated and synced to provider bootstrap flow.";
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
  const std::string selected_chat_id = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  native_chats = DeduplicateChatsById(std::move(native_chats));
  std::vector<ChatSession> local_chats = LoadChats(app);
  for (ChatSession& local : local_chats) {
    if (local.uses_native_session || !local.native_session_id.empty() || !IsLocalDraftChatId(local.id)) {
      continue;
    }
    const std::string normalized_provider_id = MapLegacyProviderId(local.provider_id, false);
    const ProviderProfile* local_provider = ProviderProfileStore::FindById(app.provider_profiles, normalized_provider_id);
    const bool local_chat_uses_gemini_history =
        Trim(local.provider_id).empty() || (local_provider != nullptr && ProviderRuntime::SupportsGeminiJsonHistory(*local_provider));
    if (!local_chat_uses_gemini_history) {
      continue;
    }
    const auto inferred_session_id = InferNativeSessionIdForLocalDraft(local, native_chats);
    if (inferred_session_id.has_value()) {
      PersistLocalDraftNativeSessionLink(app, local, inferred_session_id.value());
    }
  }
  local_chats = DeduplicateChatsById(std::move(local_chats));
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
    if (!local.provider_id.empty()) {
      native.provider_id = local.provider_id;
    }
    if (!local.template_override_id.empty()) {
      native.template_override_id = local.template_override_id;
    }
    native.rag_enabled = local.rag_enabled;
    native.rag_source_directories = local.rag_source_directories;
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
    const bool local_messages_are_newer =
        !local.messages.empty() &&
        (local.messages.size() > native.messages.size() ||
         (local.messages.size() == native.messages.size() && local.updated_at > native.updated_at));
    if (local_messages_are_newer) {
      // Keep optimistic local history visible until native provider history catches up.
      native.messages = local.messages;
      if (!local.updated_at.empty()) {
        native.updated_at = local.updated_at;
      }
      if (native.created_at.empty() && !local.created_at.empty()) {
        native.created_at = local.created_at;
      }
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
    const std::string normalized_provider_id = MapLegacyProviderId(chat.provider_id, false);
    const ProviderProfile* local_provider = ProviderProfileStore::FindById(app.provider_profiles, normalized_provider_id);
    const bool local_chat_uses_gemini_history =
        (local_provider == nullptr) ? true : ProviderRuntime::SupportsGeminiJsonHistory(*local_provider);
    // In Gemini-history mode, only explicit in-app drafts (chat-*) should appear as local-only chats.
    if (local_chat_uses_gemini_history && !IsLocalDraftChatId(chat.id) && !Trim(chat.provider_id).empty()) {
      continue;
    }
    bool has_running_terminal = false;
    for (const auto& terminal : app.cli_terminals) {
      if (terminal != nullptr && terminal->attached_chat_id == chat.id && terminal->running) {
        has_running_terminal = true;
        break;
      }
    }
    if (chat.messages.empty() && !HasPendingCallForChat(app, chat.id) && !has_running_terminal &&
        chat.id != selected_chat_id) {
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
  const CURLcode curl_init_code = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (curl_init_code != CURLE_OK) {
    std::fprintf(stderr, "Failed to initialize libcurl: %s\n", curl_easy_strerror(curl_init_code));
    return 1;
  }
  struct CurlGlobalCleanupGuard {
    ~CurlGlobalCleanupGuard() {
      curl_global_cleanup();
    }
  } curl_cleanup_guard;
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
  bool settings_dirty = false;
  const bool had_provider_file = fs::exists(ProviderProfileFilePath(app));
  app.provider_profiles = ProviderProfileStore::Load(app.data_root);
  bool providers_dirty = MigrateProviderProfilesToFixedModeIds(app);
  if (MigrateActiveProviderIdToFixedModes(app)) {
    settings_dirty = true;
  }
  if (ActiveProvider(app) == nullptr && !app.provider_profiles.empty()) {
    app.settings.active_provider_id = app.provider_profiles.front().id;
    settings_dirty = true;
  }
  if (ProviderProfile* active_profile = ActiveProvider(app); active_profile != nullptr) {
    if (!had_provider_file &&
        !app.settings.provider_command_template.empty() &&
        IsGeminiProviderId(active_profile->id) &&
        ProviderRuntime::UsesStructuredOutput(*active_profile)) {
      active_profile->command_template = app.settings.provider_command_template;
      providers_dirty = true;
    }
    app.settings.provider_command_template = active_profile->command_template;
    app.settings.gemini_command_template = app.settings.provider_command_template;
    app.settings.runtime_backend = ProviderRuntime::UsesInternalEngine(*active_profile) ? "ollama-engine" : "provider-cli";
  }
  if (!had_provider_file || providers_dirty) {
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
  if (MigrateChatProviderBindingsToFixedModes(app)) {
    settings_dirty = true;
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
  if (const ChatSession* selected_chat = SelectedChat(app);
      selected_chat != nullptr && ChatUsesCliOutput(app, *selected_chat)) {
    MarkSelectedCliTerminalForLaunch(app);
  }
  if (settings_dirty) {
    SaveSettings(app);
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

    if (done) {
      // Exit guard: stop all CLI terminals using a non-blocking fast path so
      // window close cannot hang behind child process teardown.
      FastStopCliTerminalsForExit(app);
      terminals_stopped_for_shutdown = true;
      break;
    }

    PollPendingGeminiCall(app);
    PollAllCliTerminals(app);
    PollGeminiCompatibilityTasks(app);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ApplyUserUiScale(io, app.settings.ui_scale_multiplier);
    PollRagScanState(app);

    HandleGlobalShortcuts(app);
    DrawDesktopMenuBar(app, done);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    DrawAmbientBackdrop(viewport->Pos, viewport->Size, static_cast<float>(ImGui::GetTime()));

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y));

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("Universal Agent Manager", nullptr, window_flags);

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
    DrawRuntimeModelSelectionModal(app);
    DrawRagConsoleModal(app);
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
  StopOpenCodeBridge(app);

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
