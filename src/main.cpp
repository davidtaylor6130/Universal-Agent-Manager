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
#include "common/local_chat_store.h"
#include "common/frontend_actions.h"
#include "common/gemini_command_builder.h"
#include "common/gemini_native_history_store.h"
#include "common/gemini_template_catalog.h"
#include "common/provider_profile.h"
#include "common/provider_runtime.h"
#include "common/ollama_engine_service.h"
#include "common/rag_index_service.h"
#include "common/settings_store.h"
#include "common/vcs_workspace_service.h"
#include "common/platform/platform_services.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "common/platform/sdl_includes.h"
#include "common/platform/gl_includes.h"

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

#ifndef UAM_ENABLE_ENGINE_RAG
#define UAM_ENABLE_ENGINE_RAG 1
#endif

namespace fs = std::filesystem;

using uam::AppState;
using uam::AsyncCommandTask;
using uam::CliTerminalState;
using uam::kTerminalScrollbackMaxLines;
using uam::TerminalScrollbackLine;

static bool StartCliTerminalPlatform(AppState& app, CliTerminalState& terminal, const ChatSession& chat);

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

static void SortChatsByRecent(std::vector<ChatSession>& p_chats);
static std::vector<ChatSession> DeduplicateChatsById(std::vector<ChatSession> p_chats);
static void ClampWindowSettings(AppSettings& p_settings);
static std::string NormalizeThemeChoice(std::string p_value);
static void DrawSessionSidePane(AppState& p_app, ChatSession& p_chat);
static void SaveAndUpdateStatus(AppState& p_app, const ChatSession& p_chat, const std::string& p_success, const std::string& p_failure);
static void RefreshGeminiChatsDir(AppState& p_app);
static const ChatFolder* FindFolderById(const AppState& p_app, const std::string& p_folderId);
static const ProviderProfile& ActiveProviderOrDefault(const AppState& p_app);
static const ProviderProfile* ProviderForChat(const AppState& p_app, const ChatSession& p_chat);
static const ProviderProfile& ProviderForChatOrDefault(const AppState& p_app, const ChatSession& p_chat);
static bool ActiveProviderUsesGeminiHistory(const AppState& p_app);
static bool ActiveProviderUsesInternalEngine(const AppState& p_app);
static bool ChatUsesGeminiHistory(const AppState& p_app, const ChatSession& p_chat);
static bool ChatUsesInternalEngine(const AppState& p_app, const ChatSession& p_chat);
static bool ChatUsesCliOutput(const AppState& p_app, const ChatSession& p_chat);
static int FindChatIndexById(const AppState& p_app, const std::string& p_chatId);
static ChatSession* SelectedChat(AppState& p_app);
static const ChatSession* SelectedChat(const AppState& p_app);
static ChatSession CreateNewChat(const std::string& p_folderId, const std::string& p_providerId);
static std::string CompactPreview(const std::string& p_text, std::size_t p_maxLen);
static void MarkSelectedCliTerminalForLaunch(AppState& p_app);
static void SelectChatById(AppState& p_app, const std::string& p_chatId);
static void SaveSettings(AppState& p_app);
static bool SaveChat(const AppState& p_app, const ChatSession& p_chat);
static bool ProviderUsesOpenCodeLocalBridge(const ProviderProfile& p_provider);
static bool IsRuntimeEnabledForProvider(const ProviderProfile& p_provider, std::string* p_reasonOut = nullptr);
static bool EnsureSelectedLocalRuntimeModelForProvider(AppState& p_app);
static bool SendPromptToCliRuntime(AppState& p_app, ChatSession& p_chat, const std::string& p_prompt, std::string* p_errorOut);
static bool EnsureOpenCodeBridgeRunning(AppState& p_app, std::string* p_errorOut = nullptr);
static void StopOpenCodeBridge(AppState& p_app);
static bool RestartOpenCodeBridgeIfModelChanged(AppState& p_app, std::string* p_errorOut = nullptr);
static std::filesystem::path ResolveWorkspaceRootPath(const AppState& p_app, const ChatSession& p_chat);
static std::vector<ChatSession> LoadNativeGeminiChats(const std::filesystem::path& p_chatsDir, const ProviderProfile& p_provider);
static std::vector<ChatSession> LoadChats(const AppState& p_app);
static bool MigrateChatProviderBindingsToFixedModes(AppState& p_app);
static void NormalizeChatFolderAssignments(AppState& p_app);
static void StartAsyncCommandTask(AsyncCommandTask& p_task, const std::string& p_command);
static bool TryConsumeAsyncCommandTaskOutput(AsyncCommandTask& p_task, std::string& p_outputOut);
static std::optional<std::string> ExtractSemverVersion(const std::string& p_text);
static std::string BuildGeminiVersionCheckCommand();
static std::string BuildGeminiDowngradeCommand();
static void StartGeminiVersionCheck(AppState& p_app, const bool p_force);
static void StartGeminiDowngradeToSupported(AppState& p_app);
static void PollGeminiCompatibilityTasks(AppState& p_app);
static bool IsLocalDraftChatId(const std::string& p_chatId);
static std::optional<std::string> InferNativeSessionIdForLocalDraft(const ChatSession& p_localChat, const std::vector<ChatSession>& p_nativeChats);
static bool PersistLocalDraftNativeSessionLink(const AppState& p_app, ChatSession& p_localChat, const std::string& p_nativeSessionId);
static std::string ResolveResumeSessionIdForChat(const AppState& p_app, const ChatSession& p_chat);
static bool HasPendingCallForChat(const AppState& p_app, const std::string& p_chatId);
static bool HasAnyPendingCall(const AppState& p_app);
static const PendingGeminiCall* FirstPendingCallForChat(const AppState& p_app, const std::string& p_chatId);
static void NormalizeChatBranchMetadata(AppState& p_app);
static bool CreateBranchFromMessage(AppState& p_app, const std::string& p_sourceChatId, int p_messageIndex);
static void ConsumePendingBranchRequest(AppState& p_app);
static void ApplyLocalOverrides(AppState& p_app, std::vector<ChatSession>& p_nativeChats);
static std::vector<std::string> CollectNewSessionIds(const std::vector<ChatSession>& p_loadedChats, const std::vector<std::string>& p_existingIds);
static std::string PickFirstUnblockedSessionId(const std::vector<std::string>& p_candidateIds, const std::unordered_set<std::string>& p_blockedIds);
static std::string BuildRagContextBlock(const std::vector<RagSnippet>& p_snippets);
static std::string BuildRagEnhancedPrompt(AppState& p_app, const ChatSession& p_chat, const std::string& p_promptText);
static bool IsRagEnabledForChat(const AppState& p_app, const ChatSession& p_chat);
static RagIndexService::Config RagConfigFromSettings(const AppSettings& p_settings);
static void SyncRagServiceConfig(AppState& p_app);
static std::filesystem::path ResolveProjectRagSourceRoot(const AppState& p_app, const std::filesystem::path& p_fallbackSourceRoot = {});
static std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& p_path);
static std::vector<std::filesystem::path> ResolveRagSourceRootsForChat(const AppState& p_app, const ChatSession& p_chat, const std::filesystem::path& p_fallbackSourceRoot = {});
static std::vector<std::filesystem::path> DiscoverRagSourceFolders(const std::filesystem::path& p_workspaceRoot);
static std::string RagDatabaseNameForSourceRoot(const AppSettings& p_settings, const std::filesystem::path& p_sourceRoot);
static bool ChatHasRagSourceDirectory(const ChatSession& p_chat, const std::filesystem::path& p_sourceRoot);
static bool AddChatRagSourceDirectory(ChatSession& p_chat, const std::filesystem::path& p_sourceRoot);
static bool RemoveChatRagSourceDirectoryAt(ChatSession& p_chat, std::size_t p_index);
static bool RemoveChatRagSourceDirectory(ChatSession& p_chat, const std::filesystem::path& p_sourceRoot);
static bool TriggerProjectRagScan(AppState& p_app, bool p_reusePreviousSource, const std::filesystem::path& p_fallbackSourceRoot, std::string* p_errorOut = nullptr);
static void PollRagScanState(AppState& p_app);
static RagScanState EffectiveRagScanState(const AppState& p_app);
static std::string BuildRagStatusText(const AppState& p_app);
static std::filesystem::path ResolveCurrentRagFallbackSourceRoot(const AppState& p_app);
static std::filesystem::path BuildRagTokenCappedStagingRoot(const AppState& p_app, const std::string& p_workspaceKey);
static bool BuildRagTokenCappedStagingTree(const std::filesystem::path& p_sourceRoot, const std::filesystem::path& p_stagingRoot, int p_maxTokens, std::size_t* p_indexedFilesOut, std::string* p_errorOut);
static void EnsureRagManualQueryWorkspaceState(AppState& p_app, const std::string& p_workspaceKey);
static void AppendRagScanReport(AppState& p_app, const std::string& p_message);
static void RunRagManualTestQuery(AppState& p_app, const std::filesystem::path& p_workspaceRoot);
static bool RefreshWorkspaceVcsSnapshot(AppState& p_app, const std::filesystem::path& p_workspaceRoot, bool p_force);
static void ShowVcsCommandOutput(AppState& p_app, const std::string& p_title, const VcsCommandResult& p_result);
enum class TemplatePreflightOutcome
{
	ReadyWithTemplate,
	ReadyWithoutTemplate,
	BlockingError
};

struct OpenCodeBridgeReadyInfo;
static TemplatePreflightOutcome PreflightWorkspaceTemplateForChat(AppState& p_app, const ProviderProfile& p_provider, const ChatSession& p_chat, std::string* p_bootstrapPromptOut, std::string* p_statusOut);
static bool QueueGeminiPromptForChat(AppState& p_app, ChatSession& p_chat, const std::string& p_prompt, const bool p_templateControlMessage);
static bool TruncateNativeSessionFromDisplayedMessage(const AppState& p_app, const ChatSession& p_chat, const int p_displayedMessageIndex, std::string* p_errorOut);
static std::string Trim(const std::string& p_value);
static void AddMessage(ChatSession& p_chat, const MessageRole p_role, const std::string& p_text);
static float PlatformUiSpacingScale();
static float ScaleUiLength(float p_value);
static ImVec2 ScaleUiSize(const ImVec2& p_value);
static void CaptureUiScaleBaseStyle();
static void ApplyUserUiScale(ImGuiIO& p_io, float p_userScaleMultiplier);

#include "common/runtime/json_runtime.h"

static std::string NormalizeVectorDatabaseName(std::string p_value);
static std::string TimestampNow();
static std::string NewSessionId();
static std::string ReadTextFile(const fs::path& p_path);
static bool WriteTextFile(const fs::path& p_path, const std::string& p_content);
static std::optional<fs::path> ResolveWindowsHomePath();
static fs::path ExpandLeadingTildePath(const std::string& p_rawPath);
static fs::path ResolveGeminiGlobalRootPath(const AppSettings& p_settings);
static fs::path WorkspaceGeminiRootPath(const AppState& p_app, const ChatSession& p_chat);
static fs::path WorkspaceGeminiTemplatePath(const AppState& p_app, const ChatSession& p_chat);
static std::uint64_t Fnv1a64(const std::string& p_text);
static std::string Hex64(const std::uint64_t p_value);
static std::string ToLowerAscii(std::string p_value);
static bool IsLikelyBinaryBlob(const std::string& p_content);
static bool IsRagIndexableTextFile(const fs::path& p_path);
static std::string TruncateToApproxTokenCount(const std::string& p_content, const std::size_t p_maxTokens);
static bool DirectoryContainsGguf(const fs::path& p_directory);
static fs::path ResolveRagModelFolder(const AppState& p_app, const AppSettings* p_settingsOverride = nullptr);
static std::string BuildShellCommandWithWorkingDirectory(const fs::path& p_workingDirectory, const std::string& p_command);
static bool EnsureWorkspaceGeminiLayout(const AppState& p_app, const ChatSession& p_chat, std::string* p_errorOut);
static void MarkTemplateCatalogDirty(AppState& p_app);
static bool RefreshTemplateCatalog(AppState& p_app, const bool p_force = false);
static const TemplateCatalogEntry* FindTemplateEntryById(const AppState& p_app, const std::string& p_templateId);
static std::string TemplateLabelOrFallback(const AppState& p_app, const std::string& p_templateId);
static std::string ExecuteCommandCaptureOutput(const std::string& p_command);
static bool OutputContainsNonZeroExit(const std::string& p_output);
static std::string OpenCodeBridgeRandomHex(const std::size_t p_length);
static std::string OpenCodeBridgeTimestampStamp();
static fs::path ResolveCurrentExecutablePathForBridge();
static fs::path ResolveOpenCodeBridgeExecutablePath();
static fs::path ResolveOpenCodeConfigPath();
static fs::path BuildOpenCodeBridgeReadyFilePath(const AppState& p_app);
static JsonValue JsonObjectValue();
static JsonValue JsonStringValue(const std::string& p_text);
static JsonValue* EnsureJsonObjectEntry(JsonValue& p_root, const std::string& p_key, bool* p_changedOut);
static bool SetJsonStringEntry(JsonValue& p_root, const std::string& p_key, const std::string& p_value);
static bool RemoveJsonEntry(JsonValue& p_root, const std::string& p_key);
static std::string JsonErrorStringMessage(const JsonValue* p_rootError);
static std::optional<OpenCodeBridgeReadyInfo> ParseOpenCodeBridgeReadyInfo(const std::string& p_fileText);
static size_t CurlAppendToStringCallback(void* p_ptr, const size_t p_size, const size_t p_nmemb, void* p_userdata);
static bool CurlHttpGet(const std::string& p_url, const std::string& p_bearerToken, long* p_statusCodeOut, std::string* p_bodyOut, std::string* p_errorOut);
static bool ProbeOpenCodeBridgeHealth(const AppState& p_app, std::string* p_errorOut);
static std::wstring OpenCodeBridgeWideFromUtf8(const std::string& value);
static std::string OpenCodeBridgeQuoteWindowsArg(const std::string& arg);
static std::string OpenCodeBridgeBuildWindowsCommandLine(const std::vector<std::string>& argv);
static bool StartOpenCodeBridgeProcess(AppState& p_app, const std::vector<std::string>& p_argv, std::string* p_errorOut);
static bool IsOpenCodeBridgeProcessRunning(AppState& p_app);
static void ResetOpenCodeBridgeRuntimeFields(AppState& p_app, const bool p_keepToken);
static bool WaitForOpenCodeBridgeReadyFile(AppState& p_app, const fs::path& p_readyFile, OpenCodeBridgeReadyInfo* p_infoOut, std::string* p_errorOut);
static bool EnsureOpenCodeConfigProvisioned(AppState& p_app, std::string* p_errorOut);
static std::vector<std::string> BuildOpenCodeBridgeArgv(const fs::path& p_bridgeExecutable, const fs::path& p_modelFolder, const std::string& p_requestedModel, const std::string& p_token, const fs::path& p_readyFile);
static bool StartOpenCodeBridge(AppState& p_app, const fs::path& p_modelFolder, const std::string& p_requestedModel, std::string* p_errorOut);
static fs::path SettingsFilePath(const AppState& p_app);
static fs::path ChatsRootPath(const AppState& p_app);
static fs::path ChatPath(const AppState& p_app, const ChatSession& p_chat);
static fs::path DefaultDataRootPath();
static fs::path TempFallbackDataRootPath();
static bool EnsureDataRootLayout(const fs::path& p_dataRoot, std::string* p_errorOut);
static std::optional<fs::path> ResolveGeminiProjectTmpDir(const fs::path& p_projectRoot);
static ProviderRuntimeHistoryLoadOptions RuntimeHistoryLoadOptions();
static std::optional<ChatSession> ParseGeminiSessionFile(const fs::path& p_filePath, const ProviderProfile& p_provider);
static bool StartAsyncNativeChatLoad(AppState& app);
static bool TryConsumeAsyncNativeChatLoad(AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out);
static std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& p_chats);
static std::string NewFolderId();
static int FindFolderIndexById(const AppState& p_app, const std::string& p_folderId);
static void EnsureDefaultFolder(AppState& p_app);
static void EnsureNewChatFolderSelection(AppState& p_app);
static void SaveFolders(const AppState& p_app);
static void SaveProviders(const AppState& p_app);
static fs::path ProviderProfileFilePath(const AppState& p_app);
static fs::path FrontendActionFilePath(const AppState& p_app);
static bool IsLegacyBuiltInProviderId(const std::string& p_providerId);
static bool IsGeminiProviderId(const std::string& p_providerId);
static std::string MapLegacyProviderId(const std::string& p_providerId, const bool p_preferCliForGemini);
static std::string DefaultGeminiProviderIdForLegacyViewHint(const AppState& p_app);
static bool ChatHasCliViewHint(const AppState& p_app, const ChatSession& p_chat);
static bool ShouldShowProviderProfileInUi(const ProviderProfile& p_profile);
static bool MigrateProviderProfilesToFixedModeIds(AppState& p_app);
static bool MigrateActiveProviderIdToFixedModes(AppState& p_app);
static ProviderProfile* ActiveProvider(AppState& p_app);
static const uam::FrontendAction* FindFrontendAction(const AppState& p_app, const std::string& p_key);
static bool FrontendActionVisible(const AppState& p_app, const std::string& p_key, const bool p_fallbackVisible = true);
static std::string FrontendActionLabel(const AppState& p_app, const std::string& p_key, const std::string& p_fallbackLabel);
static void LoadFrontendActions(AppState& p_app);
static std::string FolderForNewChat(const AppState& p_app);
static int CountChatsInFolder(const AppState& p_app, const std::string& p_folderId);
static std::string FolderTitleOrFallback(const ChatFolder& p_folder);
static bool ShouldReplaceChatForDuplicateId(const ChatSession& p_candidate, const ChatSession& p_existing);
static void RefreshRememberedSelection(AppState& p_app);
static void LoadSettings(AppState& p_app);
static bool MessagesEquivalentForNativeLinking(const Message& p_localMessage, const Message& p_nativeMessage);
static bool IsMessagePrefixForNativeLinking(const std::vector<Message>& p_localMessages, const std::vector<Message>& p_nativeMessages);
static std::optional<std::string> SingleSessionIdFromSet(const std::unordered_set<std::string>& p_sessionIds);
static std::string BuildProviderPrompt(const ProviderProfile& p_provider, const std::string& p_userPrompt, const std::vector<std::string>& p_files);
static std::string BuildProviderCommand(const ProviderProfile& p_provider, const AppSettings& p_settings, const std::string& p_prompt, const std::vector<std::string>& p_files, const std::string& p_resumeSessionId);
static bool RuntimeUsesLocalEngine(const AppState& p_app);
static bool EnsureLocalRuntimeModelLoaded(AppState& p_app, std::string* p_errorOut);
static bool SessionIdExistsInLoadedChats(const std::vector<ChatSession>& p_loadedChats, const std::string& p_sessionId);
static std::optional<fs::path> FindNativeSessionFilePathInDirectory(const fs::path& p_chatsDir, const std::string& p_sessionId);
static std::optional<fs::path> FindNativeSessionFilePath(const AppState& p_app, const std::string& p_sessionId);
static MessageRole NativeMessageRoleFromType(const ProviderProfile& p_provider, const std::string& p_type);
static fs::path ResolveWindowIconPath();
static void ApplyWindowIcon(SDL_Window* p_window);

#include "common/runtime/terminal_runtime.h"
#include "common/ui/ui_sections.h"

static std::string Trim(const std::string& p_value)
{
	const auto l_start = p_value.find_first_not_of(" \t\r\n");

	if (l_start == std::string::npos)
	{
		return "";
	}

	const auto l_end = p_value.find_last_not_of(" \t\r\n");
	return p_value.substr(l_start, l_end - l_start + 1);
}

static std::string NormalizeVectorDatabaseName(std::string p_value)
{
	p_value = Trim(p_value);
	auto l_shouldStripCharacter = [](const char p_ch)
	{
		const unsigned char l_c = static_cast<unsigned char>(p_ch);
		return !(std::isalnum(l_c) != 0 || p_ch == '_' || p_ch == '-' || p_ch == '.');
	};

	p_value.erase(std::remove_if(p_value.begin(), p_value.end(), l_shouldStripCharacter), p_value.end());
	return p_value;
}

static std::string TimestampNow()
{
	const auto l_now = std::chrono::system_clock::now();
	const std::time_t l_tt = std::chrono::system_clock::to_time_t(l_now);
	std::tm l_tmSnapshot{};

	if (!PlatformServicesFactory::Instance().process_service.PopulateLocalTime(l_tt, &l_tmSnapshot))
	{
		return "";
	}

	std::ostringstream l_out;
	l_out << std::put_time(&l_tmSnapshot, "%Y-%m-%d %H:%M:%S");
	return l_out.str();
}

static std::string NewSessionId()
{
	const auto l_now = std::chrono::system_clock::now().time_since_epoch();
	const auto l_epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(l_now).count();
	std::mt19937 l_rng(std::random_device{}());
	std::uniform_int_distribution<int> l_hexDigit(0, 15);
	std::ostringstream l_id;
	l_id << "chat-" << l_epochMs << "-";

	for (int l_i = 0; l_i < 6; ++l_i)
	{
		l_id << std::hex << l_hexDigit(l_rng);
	}

	return l_id.str();
}

static std::string ReadTextFile(const fs::path& p_path)
{
	std::ifstream l_in(p_path, std::ios::binary);

	if (!l_in.good())
	{
		return "";
	}

	std::ostringstream l_buffer;
	l_buffer << l_in.rdbuf();
	return l_buffer.str();
}

static bool WriteTextFile(const fs::path& p_path, const std::string& p_content)
{
	std::ofstream l_out(p_path, std::ios::binary | std::ios::trunc);

	if (!l_out.good())
	{
		return false;
	}

	l_out << p_content;
	return l_out.good();
}

static fs::path ExpandLeadingTildePath(const std::string& p_rawPath)
{
	return PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(p_rawPath);
}

static fs::path ResolveGeminiGlobalRootPath(const AppSettings& p_settings)
{
	fs::path l_candidate = ExpandLeadingTildePath(p_settings.prompt_profile_root_path);

	if (l_candidate.empty())
	{
		l_candidate = ExpandLeadingTildePath(p_settings.gemini_global_root_path);
	}

	if (!l_candidate.empty())
	{
		return l_candidate;
	}

	return AppPaths::DefaultGeminiUniversalRootPath();
}

static fs::path ResolveWorkspaceRootPath(const AppState& p_app, const ChatSession& p_chat)
{
	fs::path l_workspaceRoot;

	if (const ChatFolder* lcp_folder = FindFolderById(p_app, p_chat.folder_id); lcp_folder != nullptr)
	{
		l_workspaceRoot = ExpandLeadingTildePath(lcp_folder->directory);
	}

	if (l_workspaceRoot.empty())
	{
		l_workspaceRoot = fs::current_path();
	}

	std::error_code l_ec;
	const fs::path l_absoluteRoot = fs::absolute(l_workspaceRoot, l_ec);
	return l_ec ? l_workspaceRoot : l_absoluteRoot;
}

static fs::path WorkspaceGeminiRootPath(const AppState& p_app, const ChatSession& p_chat)
{
	return ResolveWorkspaceRootPath(p_app, p_chat) / ".gemini";
}

static fs::path WorkspaceGeminiTemplatePath(const AppState& p_app, const ChatSession& p_chat)
{
	return WorkspaceGeminiRootPath(p_app, p_chat) / "gemini.md";
}

static fs::path ResolveCurrentRagFallbackSourceRoot(const AppState& p_app)
{
	if (const ChatSession* lcp_selected = SelectedChat(p_app); lcp_selected != nullptr)
	{
		return ResolveWorkspaceRootPath(p_app, *lcp_selected);
	}

	std::error_code l_cwdEc;
	const fs::path l_cwd = fs::current_path(l_cwdEc);
	return l_cwdEc ? fs::path{} : l_cwd;
}

static std::uint64_t Fnv1a64(const std::string& p_text)
{
	std::uint64_t l_hash = 1469598103934665603ULL;

	for (const unsigned char l_ch : p_text)
	{
		l_hash ^= static_cast<std::uint64_t>(l_ch);
		l_hash *= 1099511628211ULL;
	}

	return l_hash;
}

static std::string Hex64(const std::uint64_t p_value)
{
	std::ostringstream l_out;
	l_out << std::hex << p_value;
	return l_out.str();
}

static std::string ToLowerAscii(std::string p_value)
{
	std::transform(p_value.begin(), p_value.end(), p_value.begin(), [](const unsigned char p_ch) { return static_cast<char>(std::tolower(p_ch)); });
	return p_value;
}

static bool IsLikelyBinaryBlob(const std::string& p_content)
{
	return p_content.find('\0') != std::string::npos;
}

static bool IsRagIndexableTextFile(const fs::path& p_path)
{
	static const std::unordered_set<std::string> l_kAllowedExtensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ixx", ".ipp", ".m", ".mm", ".java", ".kt", ".kts", ".go", ".rs", ".swift", ".cs", ".py", ".js", ".ts", ".tsx", ".jsx", ".php", ".rb", ".lua", ".sh", ".zsh", ".bash", ".ps1", ".sql", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf", ".xml", ".html", ".css", ".scss", ".md", ".markdown", ".txt", ".rst", ".adoc", ".cmake", ".mk", ".make"};
	const std::string l_extension = ToLowerAscii(p_path.extension().string());
	return l_kAllowedExtensions.find(l_extension) != l_kAllowedExtensions.end();
}

static std::string TruncateToApproxTokenCount(const std::string& p_content, const std::size_t p_maxTokens)
{
	if (p_maxTokens == 0 || p_content.empty())
	{
		return p_content;
	}

	std::size_t l_tokenCount = 0;
	bool l_inToken = false;
	std::size_t l_tokenStart = 0;

	for (std::size_t l_i = 0; l_i < p_content.size(); ++l_i)
	{
		const unsigned char l_ch = static_cast<unsigned char>(p_content[l_i]);
		const bool l_whitespace = (std::isspace(l_ch) != 0);

		if (!l_whitespace && !l_inToken)
		{
			l_tokenStart = l_i;
			++l_tokenCount;

			if (l_tokenCount > p_maxTokens)
			{
				std::size_t l_end = l_tokenStart;

				while (l_end > 0 && std::isspace(static_cast<unsigned char>(p_content[l_end - 1])) != 0)
				{
					--l_end;
				}

				return p_content.substr(0, l_end);
			}

			l_inToken = true;
		}
		else if (l_whitespace)
		{
			l_inToken = false;
		}
	}

	return p_content;
}

static std::filesystem::path BuildRagTokenCappedStagingRoot(const AppState& p_app, const std::string& p_workspaceKey)
{
	return p_app.data_root / "rag_scan_staging" / ("ws_" + Hex64(Fnv1a64(p_workspaceKey)));
}

static bool BuildRagTokenCappedStagingTree(const std::filesystem::path& p_sourceRoot, const std::filesystem::path& p_stagingRoot, const int p_maxTokens, std::size_t* p_indexedFilesOut, std::string* p_errorOut)
{
	if (p_indexedFilesOut != nullptr)
	{
		*p_indexedFilesOut = 0;
	}

	if (p_maxTokens <= 0)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Token cap must be greater than zero.";
		}

		return false;
	}

	std::error_code l_ec;
	fs::remove_all(p_stagingRoot, l_ec);
	l_ec.clear();
	fs::create_directories(p_stagingRoot, l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to prepare token-capped staging directory: " + p_stagingRoot.string();
		}

		return false;
	}

	std::size_t l_copiedFiles = 0;
	fs::recursive_directory_iterator l_it(p_sourceRoot, fs::directory_options::skip_permission_denied, l_ec);
	const fs::recursive_directory_iterator l_end;

	while (!l_ec && l_it != l_end)
	{
		const fs::directory_entry l_entry = *l_it;
		++l_it;

		if (l_ec)
		{
			l_ec.clear();
			continue;
		}

		if (!l_entry.is_regular_file(l_ec))
		{
			l_ec.clear();
			continue;
		}

		if (!IsRagIndexableTextFile(l_entry.path()))
		{
			continue;
		}

		const fs::path l_absolute = l_entry.path();
		const fs::path l_relative = fs::relative(l_absolute, p_sourceRoot, l_ec);

		if (l_ec || l_relative.empty())
		{
			l_ec.clear();
			continue;
		}

		const fs::path l_destination = p_stagingRoot / l_relative;
		fs::create_directories(l_destination.parent_path(), l_ec);

		if (l_ec)
		{
			l_ec.clear();
			continue;
		}

		std::ifstream l_in(l_absolute, std::ios::binary);

		if (!l_in.good())
		{
			continue;
		}

		std::ostringstream l_buffer;
		l_buffer << l_in.rdbuf();
		std::string l_content = l_buffer.str();

		if (IsLikelyBinaryBlob(l_content))
		{
			continue;
		}

		l_content = TruncateToApproxTokenCount(l_content, static_cast<std::size_t>(p_maxTokens));

		if (!WriteTextFile(l_destination, l_content))
		{
			continue;
		}

		++l_copiedFiles;
	}

	if (p_indexedFilesOut != nullptr)
	{
		*p_indexedFilesOut = l_copiedFiles;
	}

	return true;
}

static fs::path ResolveProjectRagSourceRoot(const AppState& p_app, const fs::path& p_fallbackSourceRoot)
{
	fs::path l_sourceRoot = ExpandLeadingTildePath(p_app.settings.rag_project_source_directory);

	if (l_sourceRoot.empty())
	{
		l_sourceRoot = p_fallbackSourceRoot;
	}

	if (l_sourceRoot.empty())
	{
		std::error_code l_cwdEc;
		l_sourceRoot = fs::current_path(l_cwdEc);
	}

	std::error_code l_ec;
	const fs::path l_absoluteRoot = fs::absolute(l_sourceRoot, l_ec);
	return l_ec ? l_sourceRoot.lexically_normal() : l_absoluteRoot.lexically_normal();
}

static fs::path NormalizeAbsolutePath(const fs::path& p_path)
{
	if (p_path.empty())
	{
		return {};
	}

	std::error_code l_ec;
	const fs::path l_absolute = fs::absolute(p_path, l_ec);
	return l_ec ? p_path.lexically_normal() : l_absolute.lexically_normal();
}

static std::vector<fs::path> ResolveRagSourceRootsForChat(const AppState& p_app, const ChatSession& p_chat, const fs::path& p_fallbackSourceRoot)
{
	std::vector<fs::path> l_roots;
	std::unordered_set<std::string> l_seen;
	l_roots.reserve(p_chat.rag_source_directories.size() + 1);

	for (const std::string& l_rawSource : p_chat.rag_source_directories)
	{
		fs::path l_sourceRoot = NormalizeAbsolutePath(ExpandLeadingTildePath(l_rawSource));

		if (l_sourceRoot.empty())
		{
			continue;
		}

		const std::string l_sourceKey = l_sourceRoot.generic_string();

		if (l_sourceKey.empty())
		{
			continue;
		}

		if (l_seen.insert(l_sourceKey).second)
		{
			l_roots.push_back(l_sourceRoot);
		}
	}

	if (l_roots.empty())
	{
		l_roots.push_back(ResolveProjectRagSourceRoot(p_app, p_fallbackSourceRoot));
	}

	return l_roots;
}

static std::vector<fs::path> DiscoverRagSourceFolders(const fs::path& p_workspaceRoot)
{
	std::vector<fs::path> l_folders;
	std::error_code l_ec;
	const fs::path l_normalizedWorkspace = NormalizeAbsolutePath(p_workspaceRoot);

	if (l_normalizedWorkspace.empty() || !fs::exists(l_normalizedWorkspace, l_ec) || !fs::is_directory(l_normalizedWorkspace, l_ec))
	{
		return l_folders;
	}

	l_folders.push_back(l_normalizedWorkspace);

	static const std::unordered_set<std::string> l_kExcluded = {".git", ".svn", ".hg", "node_modules", "dist", "build", "Builds", "out", "target", "__pycache__", ".venv", "venv"};

	for (const auto& l_entry : fs::directory_iterator(l_normalizedWorkspace, fs::directory_options::skip_permission_denied, l_ec))
	{
		if (l_ec || !l_entry.is_directory(l_ec))
		{
			l_ec.clear();
			continue;
		}

		const std::string l_name = l_entry.path().filename().string();

		if (l_name.empty() || l_name[0] == '.' || l_kExcluded.find(l_name) != l_kExcluded.end())
		{
			continue;
		}

		l_folders.push_back(NormalizeAbsolutePath(l_entry.path()));
	}

	std::sort(l_folders.begin(), l_folders.end(), [](const fs::path& p_lhs, const fs::path& p_rhs) { return p_lhs.generic_string() < p_rhs.generic_string(); });
	l_folders.erase(std::remove_if(l_folders.begin(), l_folders.end(), [](const fs::path& p_path) { return p_path.empty(); }), l_folders.end());
	l_folders.erase(std::unique(l_folders.begin(), l_folders.end(), [](const fs::path& p_lhs, const fs::path& p_rhs) { return p_lhs.generic_string() == p_rhs.generic_string(); }), l_folders.end());
	return l_folders;
}

static std::string RagDatabaseNameForSourceRoot(const AppSettings& p_settings, const fs::path& p_sourceRoot)
{
	const std::string l_overrideName = Trim(p_settings.vector_database_name_override);

	if (!l_overrideName.empty())
	{
		return l_overrideName;
	}

	const std::string l_sourceKey = p_sourceRoot.lexically_normal().generic_string();

	if (l_sourceKey.empty())
	{
		return "";
	}

	return "uam_" + Hex64(Fnv1a64(l_sourceKey));
}

static bool ChatHasRagSourceDirectory(const ChatSession& p_chat, const fs::path& p_sourceRoot)
{
	const std::string l_candidateKey = NormalizeAbsolutePath(p_sourceRoot).generic_string();

	if (l_candidateKey.empty())
	{
		return false;
	}

	for (const std::string& l_existingSource : p_chat.rag_source_directories)
	{
		const fs::path l_existingPath = NormalizeAbsolutePath(ExpandLeadingTildePath(l_existingSource));

		if (!l_existingPath.empty() && l_existingPath.generic_string() == l_candidateKey)
		{
			return true;
		}
	}

	return false;
}

static bool AddChatRagSourceDirectory(ChatSession& p_chat, const fs::path& p_sourceRoot)
{
	const fs::path l_normalizedRoot = NormalizeAbsolutePath(p_sourceRoot);
	const std::string l_candidateKey = l_normalizedRoot.generic_string();

	if (l_candidateKey.empty())
	{
		return false;
	}

	if (ChatHasRagSourceDirectory(p_chat, l_normalizedRoot))
	{
		return false;
	}

	p_chat.rag_source_directories.push_back(l_normalizedRoot.string());
	return true;
}

static bool RemoveChatRagSourceDirectoryAt(ChatSession& p_chat, const std::size_t p_index)
{
	if (p_index >= p_chat.rag_source_directories.size())
	{
		return false;
	}

	p_chat.rag_source_directories.erase(p_chat.rag_source_directories.begin() + static_cast<std::ptrdiff_t>(p_index));
	return true;
}

static bool RemoveChatRagSourceDirectory(ChatSession& p_chat, const fs::path& p_sourceRoot)
{
	const std::string l_removeKey = NormalizeAbsolutePath(p_sourceRoot).generic_string();

	if (l_removeKey.empty())
	{
		return false;
	}

	for (std::size_t l_i = 0; l_i < p_chat.rag_source_directories.size(); ++l_i)
	{
		const fs::path l_existing = NormalizeAbsolutePath(ExpandLeadingTildePath(p_chat.rag_source_directories[l_i]));

		if (!l_existing.empty() && l_existing.generic_string() == l_removeKey)
		{
			return RemoveChatRagSourceDirectoryAt(p_chat, l_i);
		}
	}

	return false;
}

static bool DirectoryContainsGguf(const fs::path& p_directory)
{
	std::error_code l_ec;

	if (p_directory.empty() || !fs::exists(p_directory, l_ec) || !fs::is_directory(p_directory, l_ec))
	{
		return false;
	}

	fs::recursive_directory_iterator l_it(p_directory, fs::directory_options::skip_permission_denied, l_ec);
	const fs::recursive_directory_iterator l_end;

	while (!l_ec && l_it != l_end)
	{
		const fs::directory_entry l_entry = *l_it;
		++l_it;

		if (l_ec)
		{
			l_ec.clear();
			continue;
		}

		if (!l_entry.is_regular_file(l_ec))
		{
			l_ec.clear();
			continue;
		}

		std::string l_extension = l_entry.path().extension().string();
		std::transform(l_extension.begin(), l_extension.end(), l_extension.begin(), [](const unsigned char p_ch) { return static_cast<char>(std::tolower(p_ch)); });

		if (l_extension == ".gguf")
		{
			return true;
		}
	}

	return false;
}

static fs::path ResolveRagModelFolder(const AppState& p_app, const AppSettings* p_settingsOverride)
{
	const AppSettings& l_settings = (p_settingsOverride != nullptr) ? *p_settingsOverride : p_app.settings;
	const fs::path l_configuredModelFolder = NormalizeAbsolutePath(ExpandLeadingTildePath(l_settings.models_folder_directory));

	if (!l_configuredModelFolder.empty())
	{
		std::error_code l_configuredEc;
		fs::create_directories(l_configuredModelFolder, l_configuredEc);
		return l_configuredModelFolder;
	}

	std::vector<fs::path> l_candidates;
	l_candidates.push_back(p_app.data_root / "models");

	if (const char* lcp_envModels = std::getenv("UAM_OLLAMA_ENGINE_MODELS_DIR"))
	{
		const std::string l_value = Trim(lcp_envModels);

		if (!l_value.empty())
		{
			l_candidates.push_back(ExpandLeadingTildePath(l_value));
		}
	}

	std::error_code l_cwdEc;
	const fs::path l_cwd = fs::current_path(l_cwdEc);

	if (!l_cwdEc)
	{
		l_candidates.push_back(l_cwd / "models");
		l_candidates.push_back(l_cwd / "Builds" / "models");
		l_candidates.push_back(l_cwd / "build" / "models");
	}

	for (const fs::path& l_candidate : l_candidates)
	{
		if (DirectoryContainsGguf(l_candidate))
		{
			return l_candidate;
		}
	}

	std::error_code l_ec;
	fs::create_directories(p_app.data_root / "models", l_ec);
	return p_app.data_root / "models";
}

static RagIndexService::Config RagConfigFromSettings(const AppSettings& p_settings)
{
	RagIndexService::Config l_config;
	l_config.enabled = p_settings.rag_enabled;
#if UAM_ENABLE_ENGINE_RAG
	l_config.vector_backend = (p_settings.vector_db_backend == "none") ? "none" : "ollama-engine";
	l_config.vector_enabled = (l_config.vector_backend != "none");
#else
	l_config.vector_backend = "none";
	l_config.vector_enabled = false;
#endif
	l_config.top_k = std::clamp(p_settings.rag_top_k, 1, 20);
	l_config.max_snippet_chars = static_cast<std::size_t>(std::clamp(p_settings.rag_max_snippet_chars, 120, 4000));
	l_config.max_file_bytes = static_cast<std::size_t>(std::clamp(p_settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024));
	l_config.vector_max_tokens = static_cast<std::size_t>(std::clamp(p_settings.rag_scan_max_tokens, 0, 32768));
	l_config.vector_model_id = Trim(p_settings.selected_vector_model_id);
	l_config.vector_database_name_override = Trim(p_settings.vector_database_name_override);
	return l_config;
}

static void SyncRagServiceConfig(AppState& p_app)
{
	p_app.rag_index_service.SetConfig(RagConfigFromSettings(p_app.settings));
	const fs::path l_modelFolder = ResolveRagModelFolder(p_app);
	p_app.rag_index_service.SetModelFolder(l_modelFolder);
}

static void NormalizeChatBranchMetadata(AppState& p_app)
{
	ChatBranching::Normalize(p_app.chats);
}

static std::string BuildRagContextBlock(const std::vector<RagSnippet>& p_snippets)
{
	if (p_snippets.empty())
	{
		return "";
	}

	std::ostringstream l_out;
	l_out << "Retrieved context:\n";

	for (std::size_t l_i = 0; l_i < p_snippets.size(); ++l_i)
	{
		const RagSnippet& l_snippet = p_snippets[l_i];
		l_out << (l_i + 1) << ". ";

		if (!l_snippet.relative_path.empty())
		{
			l_out << l_snippet.relative_path;

			if (l_snippet.start_line > 0 && l_snippet.end_line >= l_snippet.start_line)
			{
				l_out << ":" << l_snippet.start_line << "-" << l_snippet.end_line;
			}
		}
		else
		{
			l_out << "Snippet";
		}

		l_out << "\n";
		l_out << l_snippet.text << "\n\n";
	}

	return l_out.str();
}

static bool IsRagEnabledForChat(const AppState& p_app, const ChatSession& p_chat)
{
	return p_app.settings.rag_enabled && p_chat.rag_enabled;
}

static std::string BuildRagEnhancedPrompt(AppState& p_app, const ChatSession& p_chat, const std::string& p_promptText)
{
	if (!IsRagEnabledForChat(p_app, p_chat))
	{
		return p_promptText;
	}

	const fs::path l_chatWorkspaceRoot = ResolveWorkspaceRootPath(p_app, p_chat);
	const std::vector<fs::path> l_sourceRoots = ResolveRagSourceRootsForChat(p_app, p_chat, l_chatWorkspaceRoot);
	const std::size_t l_topK = static_cast<std::size_t>(std::clamp(p_app.settings.rag_top_k, 1, 20));
	const bool l_multipleSources = l_sourceRoots.size() > 1;

	std::vector<std::vector<RagSnippet>> l_snippetsBySource;
	l_snippetsBySource.reserve(l_sourceRoots.size());

	for (const fs::path& l_sourceRoot : l_sourceRoots)
	{
		const std::string l_sourceKey = l_sourceRoot.lexically_normal().generic_string();
		std::error_code l_ec;

		if (l_sourceRoot.empty() || !fs::exists(l_sourceRoot, l_ec) || !fs::is_directory(l_sourceRoot, l_ec))
		{
			if (!l_sourceKey.empty())
			{
				p_app.rag_last_refresh_by_workspace[l_sourceKey] = "RAG source directory is invalid or missing.";
			}

			continue;
		}

		std::string l_ragError;
		std::vector<RagSnippet> l_snippets = p_app.rag_index_service.Retrieve(l_sourceRoot, p_promptText, l_topK, 1, &l_ragError);

		if (!l_ragError.empty() && !l_sourceKey.empty())
		{
			p_app.rag_last_refresh_by_workspace[l_sourceKey] = l_ragError;
		}

		if (l_snippets.empty())
		{
			continue;
		}

		if (l_multipleSources)
		{
			std::string l_sourceLabel = l_sourceRoot.filename().string();

			if (l_sourceLabel.empty())
			{
				l_sourceLabel = l_sourceRoot.string();
			}

			for (RagSnippet& l_snippet : l_snippets)
			{
				if (l_snippet.relative_path.empty())
				{
					l_snippet.relative_path = l_sourceLabel;
				}
				else
				{
					l_snippet.relative_path = l_sourceLabel + "/" + l_snippet.relative_path;
				}
			}
		}

		l_snippetsBySource.push_back(std::move(l_snippets));
	}

	std::vector<RagSnippet> l_mergedSnippets;
	l_mergedSnippets.reserve(l_topK);
	std::vector<std::size_t> l_sourceOffsets(l_snippetsBySource.size(), 0);

	while (l_mergedSnippets.size() < l_topK)
	{
		bool l_addedAny = false;

		for (std::size_t l_i = 0; l_i < l_snippetsBySource.size(); ++l_i)
		{
			if (l_sourceOffsets[l_i] >= l_snippetsBySource[l_i].size())
			{
				continue;
			}

			l_mergedSnippets.push_back(l_snippetsBySource[l_i][l_sourceOffsets[l_i]]);
			++l_sourceOffsets[l_i];
			l_addedAny = true;

			if (l_mergedSnippets.size() >= l_topK)
			{
				break;
			}
		}

		if (!l_addedAny)
		{
			break;
		}
	}

	if (l_mergedSnippets.empty())
	{
		return p_promptText;
	}

	return BuildRagContextBlock(l_mergedSnippets) + "User prompt:\n" + p_promptText;
}

static bool TriggerProjectRagScan(AppState& p_app, const bool p_reusePreviousSource, const fs::path& p_fallbackSourceRoot, std::string* p_errorOut)
{
	const auto l_normalizeRoot = [](const fs::path& p_sourceRoot)
	{
		if (p_sourceRoot.empty())
		{
			return fs::path{};
		}

		std::error_code l_ec;
		const fs::path l_absoluteRoot = fs::absolute(p_sourceRoot, l_ec);
		return l_ec ? p_sourceRoot.lexically_normal() : l_absoluteRoot.lexically_normal();
	};

	std::vector<fs::path> l_sourceRoots;
	const fs::path l_requestedRoot = l_normalizeRoot(p_fallbackSourceRoot);

	if (const ChatSession* lcp_selectedChat = SelectedChat(p_app); lcp_selectedChat != nullptr)
	{
		l_sourceRoots = ResolveRagSourceRootsForChat(p_app, *lcp_selectedChat, p_fallbackSourceRoot);
	}

	if (l_sourceRoots.empty())
	{
		l_sourceRoots.push_back(l_requestedRoot.empty() ? ResolveProjectRagSourceRoot(p_app, p_fallbackSourceRoot) : l_requestedRoot);
	}

	if (!l_requestedRoot.empty())
	{
		const auto l_requestedIt = std::find_if(l_sourceRoots.begin(), l_sourceRoots.end(), [&](const fs::path& p_sourceRoot) { return l_normalizeRoot(p_sourceRoot).generic_string() == l_requestedRoot.generic_string(); });

		if (l_requestedIt != l_sourceRoots.end() && l_requestedIt != l_sourceRoots.begin())
		{
			std::rotate(l_sourceRoots.begin(), l_requestedIt, l_requestedIt + 1);
		}
	}

	const fs::path l_workspaceRoot = l_sourceRoots.front();
	const std::string l_workspaceDisplay = l_workspaceRoot.empty() ? "<unset>" : l_workspaceRoot.string();
	const std::string l_workspaceKey = l_workspaceRoot.lexically_normal().generic_string();

	if (l_sourceRoots.size() > 1)
	{
		AppendRagScanReport(p_app, "Multiple RAG source folders are selected for this chat; scan action targets the first folder: " + l_workspaceDisplay);
	}

	std::error_code l_ec;

	if (l_workspaceRoot.empty() || !fs::exists(l_workspaceRoot, l_ec) || !fs::is_directory(l_workspaceRoot, l_ec))
	{
		AppendRagScanReport(p_app, "Scan start rejected: source directory is invalid (" + l_workspaceDisplay + ").");
		p_app.open_rag_console_popup = true;

		if (p_errorOut != nullptr)
		{
			*p_errorOut = "RAG source directory is invalid or missing.";
		}

		return false;
	}

	p_app.settings.rag_scan_max_tokens = std::clamp(p_app.settings.rag_scan_max_tokens, 0, 32768);

	if (!p_reusePreviousSource)
	{
		if (p_app.settings.rag_scan_max_tokens > 0)
		{
			const fs::path l_stagingRoot = BuildRagTokenCappedStagingRoot(p_app, l_workspaceKey);
			std::size_t l_stagedFiles = 0;
			std::string l_stageError;

			if (!BuildRagTokenCappedStagingTree(l_workspaceRoot, l_stagingRoot, p_app.settings.rag_scan_max_tokens, &l_stagedFiles, &l_stageError))
			{
				const std::string l_failure = l_stageError.empty() ? "Failed to build token-capped staging source." : l_stageError;
				AppendRagScanReport(p_app, "Scan start rejected: " + l_failure);
				p_app.open_rag_console_popup = true;

				if (p_errorOut != nullptr)
				{
					*p_errorOut = l_failure;
				}

				return false;
			}

			p_app.rag_index_service.SetScanSourceOverride(l_workspaceRoot, l_stagingRoot);
			std::ostringstream l_report;
			l_report << "Using token-capped staging source: " << l_stagingRoot.string() << " (" << l_stagedFiles << " files) | embedding token cap=" << p_app.settings.rag_scan_max_tokens;
			AppendRagScanReport(p_app, l_report.str());
		}
		else
		{
			p_app.rag_index_service.ClearScanSourceOverride(l_workspaceRoot);
		}
	}

	const bool l_hasLocalModels = !p_app.rag_index_service.ListModels().empty();
	const RagRefreshResult l_refresh = p_reusePreviousSource ? p_app.rag_index_service.RescanPreviousSource(l_workspaceRoot) : p_app.rag_index_service.RebuildIndex(l_workspaceRoot);

	if (!l_refresh.ok)
	{
		p_app.rag_last_refresh_by_workspace[l_workspaceKey] = l_refresh.error;
		AppendRagScanReport(p_app, "Scan start failed: " + l_refresh.error);
		p_app.open_rag_console_popup = true;

		if (p_errorOut != nullptr)
		{
			*p_errorOut = l_refresh.error;
		}

		return false;
	}

	p_app.rag_scan_workspace_key = l_workspaceKey;
	p_app.rag_scan_state = p_app.rag_index_service.FetchState();
	p_app.rag_scan_status_last_emit_s = ImGui::GetTime();
	p_app.rag_finished_visible_until_s = 0.0;
	p_app.rag_last_refresh_by_workspace[l_workspaceKey] = p_reusePreviousSource ? "RAG rescan started (previous source)." : "RAG scan started.";
	p_app.status_line = p_reusePreviousSource ? "RAG rescan started (previous source)." : "RAG scan started.";
	AppendRagScanReport(p_app, (p_reusePreviousSource ? "Rescan started (previous source)." : "Scan started.") + std::string(" Source: ") + l_workspaceRoot.string());
	p_app.open_rag_console_popup = true;

	if (!l_hasLocalModels)
	{
		p_app.status_line += " (no local .gguf detected; relying on llama server if available)";
		AppendRagScanReport(p_app, "No local .gguf models detected; scan relies on configured llama server.");
	}

	if (p_errorOut != nullptr)
	{
		p_errorOut->clear();
	}

	return true;
}

static void PollRagScanState(AppState& p_app)
{
	const RagScanState l_previousState = p_app.rag_scan_state;
	p_app.rag_scan_state = p_app.rag_index_service.FetchState();
	const double l_now = ImGui::GetTime();
	const bool l_transitionedToFinished = l_previousState.lifecycle != RagScanLifecycleState::Finished && p_app.rag_scan_state.lifecycle == RagScanLifecycleState::Finished;

	if (l_transitionedToFinished)
	{
		p_app.rag_finished_visible_until_s = l_now + 8.0;

		if (!p_app.rag_scan_workspace_key.empty())
		{
			std::string l_finished = "Finished";

			if (p_app.rag_scan_state.vector_database_size > 0)
			{
				l_finished += " | " + std::to_string(p_app.rag_scan_state.vector_database_size) + " vectors";
			}

			p_app.rag_last_refresh_by_workspace[p_app.rag_scan_workspace_key] = l_finished;
			p_app.rag_last_rebuild_at_by_workspace[p_app.rag_scan_workspace_key] = TimestampNow();
		}

		p_app.status_line = "RAG scan finished: " + std::to_string(p_app.rag_scan_state.files_processed) + "/" + std::to_string(p_app.rag_scan_state.total_files) + " files";

		if (p_app.rag_scan_state.vector_database_size > 0)
		{
			p_app.status_line += " | " + std::to_string(p_app.rag_scan_state.vector_database_size) + " vectors";
		}

		AppendRagScanReport(p_app, p_app.status_line);
		p_app.rag_scan_status_last_emit_s = l_now;
		return;
	}

	if (p_app.rag_scan_state.lifecycle == RagScanLifecycleState::Running && !p_app.rag_scan_workspace_key.empty())
	{
		std::ostringstream l_running;
		l_running << "Running";

		if (p_app.rag_scan_state.total_files > 0)
		{
			l_running << " " << p_app.rag_scan_state.files_processed << "/" << p_app.rag_scan_state.total_files << " files";
		}

		if (p_app.rag_scan_state.vector_database_size > 0)
		{
			l_running << " | " << p_app.rag_scan_state.vector_database_size << " vectors";
		}

		p_app.rag_last_refresh_by_workspace[p_app.rag_scan_workspace_key] = l_running.str();

		const bool l_changedProgress = l_previousState.lifecycle != RagScanLifecycleState::Running || l_previousState.files_processed != p_app.rag_scan_state.files_processed || l_previousState.total_files != p_app.rag_scan_state.total_files || l_previousState.vector_database_size != p_app.rag_scan_state.vector_database_size;

		if (l_changedProgress && (l_now - p_app.rag_scan_status_last_emit_s >= 0.33 || l_previousState.lifecycle != RagScanLifecycleState::Running))
		{
			p_app.status_line = "RAG scan: " + l_running.str();
			AppendRagScanReport(p_app, p_app.status_line);
			p_app.rag_scan_status_last_emit_s = l_now;
		}

		return;
	}

	if (l_previousState.lifecycle == RagScanLifecycleState::Running && p_app.rag_scan_state.lifecycle == RagScanLifecycleState::Stopped)
	{
		if (!p_app.rag_scan_state.error.empty())
		{
			p_app.status_line = "RAG scan failed: " + p_app.rag_scan_state.error;
		}
		else if (l_previousState.total_files == 0)
		{
			p_app.status_line = "RAG scan stopped quickly: no indexable files found "
			                    "(.cpp/.h/.md/.txt/etc) in source directory.";
		}
		else if (l_previousState.vector_database_size == 0)
		{
			p_app.status_line = "RAG scan stopped with 0 vectors. Check embedding model "
			                    "(.gguf) or UAM_EMBEDDING_MODEL_PATH.";
		}
		else
		{
			p_app.status_line = "RAG scan stopped.";
		}

		AppendRagScanReport(p_app, p_app.status_line);
		p_app.rag_scan_status_last_emit_s = l_now;
	}
}

static RagScanState EffectiveRagScanState(const AppState& p_app)
{
	RagScanState l_state = p_app.rag_scan_state;

	if (l_state.lifecycle == RagScanLifecycleState::Stopped && p_app.rag_finished_visible_until_s > ImGui::GetTime())
	{
		l_state.lifecycle = RagScanLifecycleState::Finished;
	}

	return l_state;
}

static std::string BuildRagStatusText(const AppState& p_app)
{
	const RagScanState l_state = EffectiveRagScanState(p_app);

	if (l_state.lifecycle == RagScanLifecycleState::Finished)
	{
		return "RAG: Finished";
	}

	if (l_state.lifecycle == RagScanLifecycleState::Running)
	{
		std::ostringstream l_out;
		l_out << "RAG: Running";

		if (l_state.total_files > 0)
		{
			l_out << " " << l_state.files_processed << "/" << l_state.total_files << " files";
		}
		else
		{
			l_out << " (scanning...)";
		}

		if (l_state.vector_database_size > 0)
		{
			if (l_state.total_files > 0)
			{
				l_out << " | ";
			}
			else
			{
				l_out << " ";
			}

			l_out << l_state.vector_database_size << " vectors";
		}

		return l_out.str();
	}

	return "RAG: Stopped";
}

static void EnsureRagManualQueryWorkspaceState(AppState& p_app, const std::string& p_workspaceKey)
{
	if (p_app.rag_manual_query_workspace_key == p_workspaceKey)
	{
		return;
	}

	p_app.rag_manual_query_workspace_key = p_workspaceKey;
	p_app.rag_manual_query_results.clear();
	p_app.rag_manual_query_error.clear();
	p_app.rag_manual_query_last_query.clear();
}

static void AppendRagScanReport(AppState& p_app, const std::string& p_message)
{
	const std::string l_trimmed = Trim(p_message);

	if (l_trimmed.empty())
	{
		return;
	}

	if (!p_app.rag_scan_reports.empty())
	{
		const std::string& l_last = p_app.rag_scan_reports.back();
		const std::size_t l_separator = l_last.find(" | ");

		if (l_separator != std::string::npos && l_last.substr(l_separator + 3) == l_trimmed)
		{
			return;
		}
	}

	p_app.rag_scan_reports.push_back(TimestampNow() + " | " + l_trimmed);
	constexpr std::size_t l_kMaxRagReports = 320;

	if (p_app.rag_scan_reports.size() > l_kMaxRagReports)
	{
		const std::size_t l_trimCount = p_app.rag_scan_reports.size() - l_kMaxRagReports;
		p_app.rag_scan_reports.erase(p_app.rag_scan_reports.begin(), p_app.rag_scan_reports.begin() + static_cast<std::ptrdiff_t>(l_trimCount));
	}

	p_app.rag_scan_reports_scroll_to_bottom = true;
}

static void RunRagManualTestQuery(AppState& p_app, const std::filesystem::path& p_workspaceRoot)
{
	p_app.rag_manual_query_max = std::clamp(p_app.rag_manual_query_max, 1, 50);
	p_app.rag_manual_query_min = std::clamp(p_app.rag_manual_query_min, 1, p_app.rag_manual_query_max);
	p_app.rag_manual_query_running = true;
	p_app.rag_manual_query_error.clear();
	p_app.rag_manual_query_last_query = p_app.rag_manual_query_input;
	std::string l_queryError;
	p_app.rag_manual_query_results = p_app.rag_index_service.Retrieve(p_workspaceRoot, p_app.rag_manual_query_input, static_cast<std::size_t>(p_app.rag_manual_query_max), static_cast<std::size_t>(p_app.rag_manual_query_min), &l_queryError);
	p_app.rag_manual_query_running = false;
	p_app.rag_manual_query_error = l_queryError;

	if (l_queryError.empty())
	{
		p_app.status_line = "RAG test query completed.";
		AppendRagScanReport(p_app, "Manual query returned " + std::to_string(p_app.rag_manual_query_results.size()) + " snippet(s).");
	}
	else
	{
		p_app.status_line = "RAG test query failed: " + l_queryError;
		AppendRagScanReport(p_app, "Manual query failed: " + l_queryError);
	}
}

static bool RefreshWorkspaceVcsSnapshot(AppState& p_app, const std::filesystem::path& p_workspaceRoot, const bool p_force)
{
	if (p_workspaceRoot.empty())
	{
		return false;
	}

	const std::string l_workspaceKey = p_workspaceRoot.lexically_normal().generic_string();

	if (!p_force && p_app.vcs_snapshot_loaded_workspaces.find(l_workspaceKey) != p_app.vcs_snapshot_loaded_workspaces.end())
	{
		return true;
	}

	VcsSnapshot l_snapshot;
	l_snapshot.working_copy_root = l_workspaceKey;
	const VcsRepoType l_repoType = VcsWorkspaceService::DetectRepo(p_workspaceRoot);

	if (l_repoType == VcsRepoType::None)
	{
		l_snapshot.repo_type = VcsRepoType::None;
		p_app.vcs_snapshot_by_workspace[l_workspaceKey] = std::move(l_snapshot);
		p_app.vcs_snapshot_loaded_workspaces.insert(l_workspaceKey);
		return true;
	}

	VcsCommandResult l_command = VcsWorkspaceService::ReadSnapshot(p_workspaceRoot, l_snapshot);

	if (!l_command.ok)
	{
		l_snapshot.repo_type = VcsRepoType::Svn;
		p_app.vcs_snapshot_by_workspace[l_workspaceKey] = std::move(l_snapshot);
		p_app.vcs_snapshot_loaded_workspaces.insert(l_workspaceKey);

		if (p_force)
		{
			p_app.status_line = l_command.error.empty() ? "SVN snapshot refresh failed." : l_command.error;
		}

		return false;
	}

	p_app.vcs_snapshot_by_workspace[l_workspaceKey] = std::move(l_snapshot);
	p_app.vcs_snapshot_loaded_workspaces.insert(l_workspaceKey);
	return true;
}

static void ShowVcsCommandOutput(AppState& p_app, const std::string& p_title, const VcsCommandResult& p_result)
{
	std::ostringstream l_out;

	if (!p_result.ok)
	{
		l_out << "[Command failed";

		if (p_result.timed_out)
		{
			l_out << " (timed out)";
		}

		if (p_result.exit_code >= 0)
		{
			l_out << ", exit code " << p_result.exit_code;
		}

		l_out << "]\n";

		if (!p_result.error.empty())
		{
			l_out << p_result.error << "\n\n";
		}
	}

	l_out << p_result.output;
	p_app.vcs_output_popup_title = p_title;
	p_app.vcs_output_popup_content = l_out.str();
	p_app.open_vcs_output_popup = true;
}

static bool CreateBranchFromMessage(AppState& p_app, const std::string& p_sourceChatId, const int p_messageIndex)
{
	const int l_sourceIndex = FindChatIndexById(p_app, p_sourceChatId);

	if (l_sourceIndex < 0)
	{
		p_app.status_line = "Branch source chat no longer exists.";
		return false;
	}

	const ChatSession l_source = p_app.chats[l_sourceIndex];

	if (p_messageIndex < 0 || p_messageIndex >= static_cast<int>(l_source.messages.size()))
	{
		p_app.status_line = "Branch source message is no longer valid.";
		return false;
	}

	if (l_source.messages[p_messageIndex].role != MessageRole::User)
	{
		p_app.status_line = "Branching is currently supported for user messages only.";
		return false;
	}

	ChatSession l_branch = CreateNewChat(l_source.folder_id, l_source.provider_id);
	l_branch.uses_native_session = false;
	l_branch.native_session_id.clear();
	l_branch.parent_chat_id = l_source.id;
	l_branch.branch_root_chat_id = l_source.branch_root_chat_id.empty() ? l_source.id : l_source.branch_root_chat_id;
	l_branch.branch_from_message_index = p_messageIndex;
	l_branch.template_override_id = l_source.template_override_id;
	l_branch.gemini_md_bootstrapped = l_source.gemini_md_bootstrapped;
	l_branch.rag_enabled = l_source.rag_enabled;
	l_branch.rag_source_directories = l_source.rag_source_directories;
	l_branch.linked_files = l_source.linked_files;
	l_branch.messages.assign(l_source.messages.begin(), l_source.messages.begin() + p_messageIndex + 1);
	l_branch.updated_at = TimestampNow();
	l_branch.title = "Branch: " + CompactPreview(l_source.messages[p_messageIndex].content, 40);

	if (Trim(l_branch.title).empty())
	{
		l_branch.title = "Branch Chat";
	}

	p_app.chats.push_back(l_branch);
	NormalizeChatBranchMetadata(p_app);
	SortChatsByRecent(p_app.chats);
	SelectChatById(p_app, l_branch.id);
	SaveSettings(p_app);

	if (ChatUsesCliOutput(p_app, p_app.chats[p_app.selected_chat_index]))
	{
		MarkSelectedCliTerminalForLaunch(p_app);
	}

	if (!SaveChat(p_app, l_branch))
	{
		p_app.status_line = "Branch created in memory, but failed to save.";
		return false;
	}

	p_app.status_line = "Branch chat created.";
	return true;
}

static void ConsumePendingBranchRequest(AppState& p_app)
{
	if (p_app.pending_branch_chat_id.empty())
	{
		return;
	}

	const std::string l_chatId = p_app.pending_branch_chat_id;
	const int l_messageIndex = p_app.pending_branch_message_index;
	p_app.pending_branch_chat_id.clear();
	p_app.pending_branch_message_index = -1;
	CreateBranchFromMessage(p_app, l_chatId, l_messageIndex);
}

static std::string BuildShellCommandWithWorkingDirectory(const fs::path& p_workingDirectory, const std::string& p_command)
{
	return PlatformServicesFactory::Instance().process_service.BuildShellCommandWithWorkingDirectory(p_workingDirectory, p_command);
}

static bool EnsureWorkspaceGeminiLayout(const AppState& p_app, const ChatSession& p_chat, std::string* p_errorOut = nullptr)
{
	std::error_code l_ec;
	const fs::path l_workspaceRoot = ResolveWorkspaceRootPath(p_app, p_chat);
	fs::create_directories(l_workspaceRoot, l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create workspace root '" + l_workspaceRoot.string() + "': " + l_ec.message();
		}

		return false;
	}

	const fs::path l_workspaceGemini = WorkspaceGeminiRootPath(p_app, p_chat);
	fs::create_directories(l_workspaceGemini, l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create local .gemini directory: " + l_ec.message();
		}

		return false;
	}

	fs::create_directories(l_workspaceGemini / "Lessons", l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create .gemini/Lessons: " + l_ec.message();
		}

		return false;
	}

	fs::create_directories(l_workspaceGemini / "Failures", l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create .gemini/Failures: " + l_ec.message();
		}

		return false;
	}

	fs::create_directories(l_workspaceGemini / "auto-test", l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create .gemini/auto-test: " + l_ec.message();
		}

		return false;
	}

	return true;
}

static void MarkTemplateCatalogDirty(AppState& p_app)
{
	p_app.template_catalog_dirty = true;
}

static bool RefreshTemplateCatalog(AppState& p_app, const bool p_force)
{
	if (!p_force && !p_app.template_catalog_dirty)
	{
		return true;
	}

	const fs::path l_globalRoot = ResolveGeminiGlobalRootPath(p_app.settings);
	std::string l_error;

	if (!GeminiTemplateCatalog::EnsureCatalogPath(l_globalRoot, &l_error))
	{
		p_app.template_catalog.clear();
		p_app.template_catalog_dirty = false;

		if (!l_error.empty())
		{
			p_app.status_line = l_error;
		}

		return false;
	}

	p_app.template_catalog = GeminiTemplateCatalog::List(l_globalRoot);
	p_app.template_catalog_dirty = false;
	return true;
}

static const TemplateCatalogEntry* FindTemplateEntryById(const AppState& p_app, const std::string& p_templateId)
{
	if (p_templateId.empty())
	{
		return nullptr;
	}

	for (const TemplateCatalogEntry& l_entry : p_app.template_catalog)
	{
		if (l_entry.id == p_templateId)
		{
			return &l_entry;
		}
	}

	return nullptr;
}

static std::string TemplateLabelOrFallback(const AppState& p_app, const std::string& p_templateId)
{
	const TemplateCatalogEntry* lcp_entry = FindTemplateEntryById(p_app, p_templateId);

	if (lcp_entry != nullptr)
	{
		return lcp_entry->display_name;
	}

	return p_templateId.empty() ? "None" : ("Missing: " + p_templateId);
}

static std::string ExecuteCommandCaptureOutput(const std::string& p_command)
{
	const std::string l_fullCommand = p_command + " 2>&1";
	const IPlatformProcessService& process_service = PlatformServicesFactory::Instance().process_service;
	std::string l_output;
	int l_rawStatus = -1;
	std::string l_captureError;

	if (!process_service.CaptureCommandOutput(l_fullCommand, &l_output, &l_rawStatus, &l_captureError))
	{
		std::ostringstream l_message;
		l_message << "Failed to launch Gemini CLI command";

		if (errno != 0)
		{
			l_message << " (" << std::strerror(errno) << ")";
		}

		l_message << ".";
		return l_message.str();
	}

	const int l_exitCode = process_service.NormalizeCapturedCommandExitCode(l_rawStatus);

	if (l_output.empty())
	{
		l_output = "(Gemini CLI returned no output.)";
	}

	if (l_exitCode != 0)
	{
		l_output += "\n\n[Gemini CLI exited with code " + std::to_string(l_exitCode) + "]";
	}

	return l_output;
}

static void StartAsyncCommandTask(AsyncCommandTask& p_task, const std::string& p_command)
{
	p_task.running = true;
	p_task.command_preview = p_command;
	p_task.completed = std::make_shared<std::atomic<bool>>(false);
	p_task.output = std::make_shared<std::string>();
	std::shared_ptr<std::atomic<bool>> l_completed = p_task.completed;
	std::shared_ptr<std::string> l_output = p_task.output;
	auto l_runCommandTask = [p_command, l_completed, l_output]()
	{
		*l_output = ExecuteCommandCaptureOutput(p_command);
		l_completed->store(true, std::memory_order_release);
	};

	std::thread(l_runCommandTask).detach();
}

static bool TryConsumeAsyncCommandTaskOutput(AsyncCommandTask& p_task, std::string& p_outputOut)
{
	if (!p_task.running)
	{
		return false;
	}

	if (p_task.completed == nullptr || p_task.output == nullptr)
	{
		p_task.running = false;
		p_task.command_preview.clear();
		p_task.completed.reset();
		p_task.output.reset();
		p_outputOut.clear();
		return true;
	}

	if (!p_task.completed->load(std::memory_order_acquire))
	{
		return false;
	}

	p_outputOut = *p_task.output;
	p_task.running = false;
	p_task.command_preview.clear();
	p_task.completed.reset();
	p_task.output.reset();
	return true;
}

static std::optional<std::string> ExtractSemverVersion(const std::string& p_text)
{
	static const std::regex l_semverPattern(R"((\d+)\.(\d+)\.(\d+))");
	std::smatch l_match;

	if (std::regex_search(p_text, l_match, l_semverPattern) && !l_match.str(0).empty())
	{
		return l_match.str(0);
	}

	return std::nullopt;
}

static std::string BuildGeminiVersionCheckCommand()
{
	return "gemini --version";
}

static std::string BuildGeminiDowngradeCommand()
{
	return PlatformServicesFactory::Instance().process_service.GeminiDowngradeCommand();
}

static void StartGeminiVersionCheck(AppState& p_app, const bool p_force)
{
	if (p_app.gemini_version_check_task.running)
	{
		return;
	}

	if (!p_force && p_app.gemini_version_checked)
	{
		return;
	}

	StartAsyncCommandTask(p_app.gemini_version_check_task, BuildGeminiVersionCheckCommand());
	p_app.gemini_version_message = "Checking installed Gemini version...";
}

static void StartGeminiDowngradeToSupported(AppState& p_app)
{
	if (p_app.gemini_downgrade_task.running)
	{
		return;
	}

	const std::string l_command = BuildGeminiDowngradeCommand();
	StartAsyncCommandTask(p_app.gemini_downgrade_task, l_command);
	p_app.gemini_downgrade_output.clear();
	p_app.status_line = "Running Gemini downgrade command...";
}

static bool OutputContainsNonZeroExit(const std::string& p_output)
{
	return p_output.find("[Gemini CLI exited with code ") != std::string::npos;
}

static void PollGeminiCompatibilityTasks(AppState& p_app)
{
	std::string l_output;

	if (TryConsumeAsyncCommandTaskOutput(p_app.gemini_version_check_task, l_output))
	{
		p_app.gemini_version_checked = true;
		p_app.gemini_version_raw_output = l_output;
		p_app.gemini_installed_version.clear();
		p_app.gemini_version_supported = false;

		const std::optional<std::string> l_parsed = ExtractSemverVersion(l_output);

		if (l_parsed.has_value())
		{
			p_app.gemini_installed_version = l_parsed.value();
			p_app.gemini_version_supported = (p_app.gemini_installed_version == kSupportedGeminiVersion);

			if (p_app.gemini_version_supported)
			{
				p_app.gemini_version_message = "Gemini version is supported.";
			}
			else
			{
				p_app.gemini_version_message = "Installed Gemini version is unsupported for this app.";
			}
		}
		else
		{
			const std::string l_lowered = Trim(l_output);

			if (l_lowered.find("not found") != std::string::npos || l_lowered.find("not recognized") != std::string::npos)
			{
				p_app.gemini_version_message = "Gemini CLI is not installed or not on PATH.";
			}
			else
			{
				p_app.gemini_version_message = "Could not parse Gemini version output.";
			}
		}
	}

	if (TryConsumeAsyncCommandTaskOutput(p_app.gemini_downgrade_task, l_output))
	{
		p_app.gemini_downgrade_output = l_output;

		if (OutputContainsNonZeroExit(l_output))
		{
			p_app.status_line = "Gemini downgrade command failed. Review output in Settings.";
			p_app.gemini_version_message = "Downgrade command failed.";
		}
		else
		{
			p_app.status_line = "Gemini downgrade completed. Re-checking installed version.";
			StartGeminiVersionCheck(p_app, true);
		}
	}
}

static std::string OpenCodeBridgeRandomHex(const std::size_t p_length)
{
	static thread_local std::mt19937 l_lRng(std::random_device{}());
	std::uniform_int_distribution<int> l_lNibble(0, 15);
	std::string l_lSValue;
	l_lSValue.reserve(p_length);

	for (std::size_t l_i = 0; l_i < p_length; ++l_i)
	{
		const int l_lValue = l_lNibble(l_lRng);
		l_lSValue.push_back(static_cast<char>((l_lValue < 10) ? ('0' + l_lValue) : ('a' + (l_lValue - 10))));
	}

	return l_lSValue;
}

static std::string OpenCodeBridgeTimestampStamp()
{
	const auto l_now = std::chrono::system_clock::now();
	const std::time_t l_tt = std::chrono::system_clock::to_time_t(l_now);
	std::tm l_tmSnapshot{};

	if (!PlatformServicesFactory::Instance().process_service.PopulateLocalTime(l_tt, &l_tmSnapshot))
	{
		return "";
	}

	std::ostringstream l_out;
	l_out << std::put_time(&l_tmSnapshot, "%Y%m%d-%H%M%S");
	return l_out.str();
}

static fs::path ResolveCurrentExecutablePathForBridge()
{
	return PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
}

static fs::path ResolveOpenCodeBridgeExecutablePath()
{
	const std::string l_lSBridgeBinaryName = PlatformServicesFactory::Instance().process_service.OpenCodeBridgeBinaryName();

	std::vector<fs::path> l_lVecPathCandidates;

	if (const fs::path l_lPathExecutable = ResolveCurrentExecutablePathForBridge(); !l_lPathExecutable.empty())
	{
		l_lVecPathCandidates.push_back(l_lPathExecutable.parent_path() / l_lSBridgeBinaryName);
	}

	std::unique_ptr<char, decltype(&SDL_free)> lp_basePath(SDL_GetBasePath(), SDL_free);

	if (lp_basePath != nullptr)
	{
		l_lVecPathCandidates.push_back(fs::path(lp_basePath.get()) / l_lSBridgeBinaryName);
	}

	std::error_code l_lEcCwd;
	const fs::path l_lPathCwd = fs::current_path(l_lEcCwd);

	if (!l_lEcCwd)
	{
		l_lVecPathCandidates.push_back(l_lPathCwd / l_lSBridgeBinaryName);
		l_lVecPathCandidates.push_back(l_lPathCwd / "Builds" / l_lSBridgeBinaryName);
		l_lVecPathCandidates.push_back(l_lPathCwd / "Builds" / "ollama_engine" / l_lSBridgeBinaryName);
		l_lVecPathCandidates.push_back(l_lPathCwd / "Builds" / "Release" / l_lSBridgeBinaryName);
		l_lVecPathCandidates.push_back(l_lPathCwd / "Builds" / "Release" / "ollama_engine" / l_lSBridgeBinaryName);
		// Legacy fallback locations kept for pre-Builds layouts.
		l_lVecPathCandidates.push_back(l_lPathCwd / "build" / l_lSBridgeBinaryName);
		l_lVecPathCandidates.push_back(l_lPathCwd / "build" / "ollama_engine" / l_lSBridgeBinaryName);
		l_lVecPathCandidates.push_back(l_lPathCwd / "build-release" / l_lSBridgeBinaryName);
		l_lVecPathCandidates.push_back(l_lPathCwd / "build-release" / "ollama_engine" / l_lSBridgeBinaryName);
	}

	for (const fs::path& l_lPathCandidate : l_lVecPathCandidates)
	{
		std::error_code l_lEcExists;

		if (!l_lPathCandidate.empty() && fs::exists(l_lPathCandidate, l_lEcExists) && !l_lEcExists)
		{
			return l_lPathCandidate;
		}
	}

	return fs::path(l_lSBridgeBinaryName);
}

static fs::path ResolveOpenCodeConfigPath()
{
	return PlatformServicesFactory::Instance().path_service.ResolveOpenCodeConfigPath();
}

static fs::path BuildOpenCodeBridgeReadyFilePath(const AppState& p_app)
{
	const fs::path l_lPathRuntimeDir = p_app.data_root / "runtime";
	std::error_code l_lEc;
	fs::create_directories(l_lPathRuntimeDir, l_lEc);
	return l_lPathRuntimeDir / ("opencode_bridge_ready_" + OpenCodeBridgeTimestampStamp() + "_" + OpenCodeBridgeRandomHex(8) + ".json");
}

static JsonValue JsonObjectValue()
{
	JsonValue l_value;
	l_value.type = JsonValue::Type::Object;
	return l_value;
}

static JsonValue JsonStringValue(const std::string& p_text)
{
	JsonValue l_value;
	l_value.type = JsonValue::Type::String;
	l_value.string_value = p_text;
	return l_value;
}

static JsonValue* EnsureJsonObjectEntry(JsonValue& p_root, const std::string& p_key, bool* p_changedOut = nullptr)
{
	if (p_root.type != JsonValue::Type::Object)
	{
		p_root = JsonObjectValue();

		if (p_changedOut != nullptr)
		{
			*p_changedOut = true;
		}
	}

	auto l_it = p_root.object_value.find(p_key);

	if (l_it == p_root.object_value.end() || l_it->second.type != JsonValue::Type::Object)
	{
		p_root.object_value[p_key] = JsonObjectValue();

		if (p_changedOut != nullptr)
		{
			*p_changedOut = true;
		}
	}

	return &p_root.object_value[p_key];
}

static bool SetJsonStringEntry(JsonValue& p_root, const std::string& p_key, const std::string& p_value)
{
	auto l_it = p_root.object_value.find(p_key);

	if (l_it != p_root.object_value.end() && l_it->second.type == JsonValue::Type::String && l_it->second.string_value == p_value)
	{
		return false;
	}

	p_root.object_value[p_key] = JsonStringValue(p_value);
	return true;
}

static bool RemoveJsonEntry(JsonValue& p_root, const std::string& p_key)
{
	return p_root.object_value.erase(p_key) > 0;
}

static std::string JsonErrorStringMessage(const JsonValue* p_rootError)
{
	if (p_rootError == nullptr || p_rootError->type != JsonValue::Type::Object)
	{
		return "";
	}

	return JsonStringOrEmpty(p_rootError->Find("error"));
}

struct OpenCodeBridgeReadyInfo
{
	std::string m_endpoint;
	std::string m_apiBase;
	std::string m_model;
	std::string m_error;
	bool m_ok = false;
};

static std::optional<OpenCodeBridgeReadyInfo> ParseOpenCodeBridgeReadyInfo(const std::string& p_fileText)
{
	const std::optional<JsonValue> l_rootOpt = ParseJson(p_fileText);

	if (!l_rootOpt.has_value() || l_rootOpt->type != JsonValue::Type::Object)
	{
		return std::nullopt;
	}

	OpenCodeBridgeReadyInfo l_info;
	const JsonValue& l_root = l_rootOpt.value();

	if (const JsonValue* lcp_okValue = l_root.Find("ok"); lcp_okValue != nullptr && lcp_okValue->type == JsonValue::Type::Bool)
	{
		l_info.m_ok = lcp_okValue->bool_value;
	}

	l_info.m_endpoint = Trim(JsonStringOrEmpty(l_root.Find("endpoint")));
	l_info.m_apiBase = Trim(JsonStringOrEmpty(l_root.Find("api_base")));
	l_info.m_model = Trim(JsonStringOrEmpty(l_root.Find("model")));
	l_info.m_error = Trim(JsonStringOrEmpty(l_root.Find("error")));

	if (!l_info.m_ok && l_info.m_error.empty())
	{
		l_info.m_error = JsonErrorStringMessage(&l_root);
	}

	return l_info;
}

static size_t CurlAppendToStringCallback(void* p_ptr, const size_t p_size, const size_t p_nmemb, void* p_userdata)
{
	if (p_ptr == nullptr || p_userdata == nullptr || p_size == 0 || p_nmemb == 0)
	{
		return 0;
	}

	const size_t l_total = p_size * p_nmemb;
	auto* lp_output = static_cast<std::string*>(p_userdata);
	lp_output->append(static_cast<const char*>(p_ptr), l_total);
	return l_total;
}

static bool CurlHttpGet(const std::string& p_url, const std::string& p_bearerToken, long* p_statusCodeOut, std::string* p_bodyOut, std::string* p_errorOut)
{
	if (p_statusCodeOut != nullptr)
	{
		*p_statusCodeOut = 0;
	}

	if (p_bodyOut != nullptr)
	{
		p_bodyOut->clear();
	}

	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> lp_curl(curl_easy_init(), curl_easy_cleanup);

	if (lp_curl == nullptr)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to initialize libcurl.";
		}

		return false;
	}

	std::string l_body;
	curl_easy_setopt(lp_curl.get(), CURLOPT_URL, p_url.c_str());
	curl_easy_setopt(lp_curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(lp_curl.get(), CURLOPT_TIMEOUT_MS, 1500L);
	curl_easy_setopt(lp_curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
	curl_easy_setopt(lp_curl.get(), CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(lp_curl.get(), CURLOPT_WRITEFUNCTION, CurlAppendToStringCallback);
	curl_easy_setopt(lp_curl.get(), CURLOPT_WRITEDATA, &l_body);

	std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> lp_headers(nullptr, curl_slist_free_all);

	if (!p_bearerToken.empty())
	{
		const std::string l_auth = "Authorization: Bearer " + p_bearerToken;
		curl_slist* lp_headerList = curl_slist_append(nullptr, l_auth.c_str());

		if (lp_headerList == nullptr)
		{
			if (p_errorOut != nullptr)
			{
				*p_errorOut = "Failed to build HTTP authorization header.";
			}

			return false;
		}

		lp_headers.reset(lp_headerList);
		curl_easy_setopt(lp_curl.get(), CURLOPT_HTTPHEADER, lp_headers.get());
	}

	const CURLcode l_code = curl_easy_perform(lp_curl.get());
	long l_statusCode = 0;
	curl_easy_getinfo(lp_curl.get(), CURLINFO_RESPONSE_CODE, &l_statusCode);

	if (p_bodyOut != nullptr)
	{
		*p_bodyOut = l_body;
	}

	if (p_statusCodeOut != nullptr)
	{
		*p_statusCodeOut = l_statusCode;
	}

	if (l_code != CURLE_OK)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = std::string("HTTP probe failed: ") + curl_easy_strerror(l_code);
		}

		return false;
	}

	return true;
}

static bool ProbeOpenCodeBridgeHealth(const AppState& p_app, std::string* p_errorOut = nullptr)
{
	const std::string l_endpoint = Trim(p_app.opencode_bridge.endpoint);

	if (l_endpoint.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "OpenCode bridge endpoint is empty.";
		}

		return false;
	}

	long l_statusCode = 0;
	std::string l_body;
	std::string l_curlError;

	if (!CurlHttpGet(l_endpoint + "/healthz", "", &l_statusCode, &l_body, &l_curlError))
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = l_curlError;
		}

		return false;
	}

	if (l_statusCode != 200)
	{
		if (p_errorOut != nullptr)
		{
			std::ostringstream l_out;
			l_out << "OpenCode bridge health check failed (status " << l_statusCode << ").";
			const std::string l_trimmedBody = Trim(l_body);

			if (!l_trimmedBody.empty())
			{
				l_out << " Body: " << CompactPreview(l_trimmedBody, 180);
			}

			*p_errorOut = l_out.str();
		}

		return false;
	}

	return true;
}

static bool StartOpenCodeBridgeProcess(AppState& p_app, const std::vector<std::string>& p_argv, std::string* p_errorOut = nullptr)
{
	const bool l_started = PlatformServicesFactory::Instance().process_service.StartOpenCodeBridgeProcess(p_argv, p_app.opencode_bridge, p_errorOut);
	p_app.opencode_bridge.running = l_started;
	return l_started;
}

static bool IsOpenCodeBridgeProcessRunning(AppState& p_app)
{
	const bool l_running = PlatformServicesFactory::Instance().process_service.IsOpenCodeBridgeProcessRunning(p_app.opencode_bridge);
	p_app.opencode_bridge.running = l_running;
	return l_running;
}

static void ResetOpenCodeBridgeRuntimeFields(AppState& p_app, const bool p_keepToken = true)
{
	const std::string l_preservedToken = p_keepToken ? p_app.opencode_bridge.token : "";
	p_app.opencode_bridge.running = false;
	p_app.opencode_bridge.healthy = false;
	p_app.opencode_bridge.endpoint.clear();
	p_app.opencode_bridge.api_base.clear();
	p_app.opencode_bridge.selected_model.clear();
	p_app.opencode_bridge.requested_model.clear();
	p_app.opencode_bridge.model_folder.clear();
	p_app.opencode_bridge.ready_file.clear();
	p_app.opencode_bridge.last_error.clear();

	if (p_keepToken)
	{
		p_app.opencode_bridge.token = l_preservedToken;
	}
	else
	{
		p_app.opencode_bridge.token.clear();
	}
}

static bool WaitForOpenCodeBridgeReadyFile(AppState& p_app, const fs::path& p_readyFile, OpenCodeBridgeReadyInfo* p_infoOut, std::string* p_errorOut = nullptr)
{
	const auto l_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);

	while (std::chrono::steady_clock::now() < l_deadline)
	{
		std::error_code l_ec;

		if (fs::exists(p_readyFile, l_ec) && !l_ec)
		{
			const std::string l_text = Trim(ReadTextFile(p_readyFile));

			if (!l_text.empty())
			{
				const std::optional<OpenCodeBridgeReadyInfo> l_info = ParseOpenCodeBridgeReadyInfo(l_text);

				if (l_info.has_value())
				{
					if (!l_info->m_ok && !l_info->m_error.empty())
					{
						if (p_errorOut != nullptr)
						{
							*p_errorOut = l_info->m_error;
						}

						return false;
					}

					if (!l_info->m_endpoint.empty())
					{
						if (p_infoOut != nullptr)
						{
							*p_infoOut = l_info.value();
						}

						return true;
					}
				}
			}
		}

		if (!IsOpenCodeBridgeProcessRunning(p_app))
		{
			if (p_errorOut != nullptr)
			{
				*p_errorOut = "OpenCode bridge process exited before readiness "
				              "handshake.";
			}

			return false;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(40));
	}

	if (p_errorOut != nullptr)
	{
		*p_errorOut = "Timed out waiting for OpenCode bridge ready file.";
	}

	return false;
}

static bool EnsureOpenCodeConfigProvisioned(AppState& p_app, std::string* p_errorOut = nullptr)
{
	const std::string l_apiBase = Trim(p_app.opencode_bridge.api_base);
	std::string l_modelId = Trim(p_app.opencode_bridge.selected_model);

	if (l_modelId.empty())
	{
		l_modelId = Trim(p_app.settings.selected_model_id);
	}

	if (l_apiBase.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "OpenCode bridge API base URL is empty.";
		}

		return false;
	}

	if (l_modelId.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "OpenCode bridge has no selected model.";
		}

		return false;
	}

	const fs::path l_configPath = ResolveOpenCodeConfigPath();
	std::error_code l_ec;
	fs::create_directories(l_configPath.parent_path(), l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create OpenCode config directory: " + l_ec.message();
		}

		return false;
	}

	JsonValue l_root = JsonObjectValue();
	bool l_changed = false;
	bool l_parseFailed = false;

	const std::string l_existingText = ReadTextFile(l_configPath);

	if (!Trim(l_existingText).empty())
	{
		const std::optional<JsonValue> l_parsed = ParseJson(l_existingText);

		if (!l_parsed.has_value() || l_parsed->type != JsonValue::Type::Object)
		{
			l_parseFailed = true;
			const fs::path l_backupPath = l_configPath.parent_path() / (l_configPath.stem().string() + ".backup-" + OpenCodeBridgeTimestampStamp() + l_configPath.extension().string());
			std::error_code l_copyEc;
			fs::copy_file(l_configPath, l_backupPath, fs::copy_options::overwrite_existing, l_copyEc);

			if (l_copyEc)
			{
				p_app.status_line = "OpenCode config parse failed; backup copy also failed: " + l_copyEc.message();
			}
			else
			{
				p_app.status_line = "OpenCode config parse failed; created backup at " + l_backupPath.string() + ".";
			}

			l_changed = true;
		}
		else
		{
			l_root = l_parsed.value();
		}
	}

	JsonValue* lp_provider = EnsureJsonObjectEntry(l_root, "provider", &l_changed);
	JsonValue* lp_uamLocal = EnsureJsonObjectEntry(*lp_provider, "uam_local", &l_changed);
	l_changed = SetJsonStringEntry(*lp_uamLocal, "npm", "@ai-sdk/openai-compatible") || l_changed;
	l_changed = SetJsonStringEntry(*lp_uamLocal, "name", "UAM Local (Ollama Engine)") || l_changed;
	l_changed = SetJsonStringEntry(*lp_uamLocal, "api", l_apiBase) || l_changed;
	JsonValue* lp_options = EnsureJsonObjectEntry(*lp_uamLocal, "options", &l_changed);
	l_changed = SetJsonStringEntry(*lp_options, "baseURL", l_apiBase) || l_changed;

	if (!p_app.opencode_bridge.token.empty())
	{
		l_changed = SetJsonStringEntry(*lp_options, "apiKey", p_app.opencode_bridge.token) || l_changed;
	}
	else
	{
		l_changed = RemoveJsonEntry(*lp_options, "apiKey") || l_changed;
	}

	JsonValue* lp_models = EnsureJsonObjectEntry(*lp_uamLocal, "models", &l_changed);
	JsonValue* lp_modelEntry = EnsureJsonObjectEntry(*lp_models, l_modelId, &l_changed);
	fs::path l_modelPath(l_modelId);
	std::string l_modelDisplayName = l_modelPath.filename().string();

	if (l_modelDisplayName.empty())
	{
		l_modelDisplayName = l_modelId;
	}

	l_changed = SetJsonStringEntry(*lp_modelEntry, "name", l_modelDisplayName) || l_changed;
	l_changed = SetJsonStringEntry(l_root, "model", "uam_local/" + l_modelId) || l_changed;

	if (!l_changed && !l_parseFailed)
	{
		return true;
	}

	if (!WriteTextFile(l_configPath, SerializeJson(l_root)))
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to write OpenCode config file: " + l_configPath.string();
		}

		return false;
	}

	return true;
}

static std::vector<std::string> BuildOpenCodeBridgeArgv(const fs::path& p_bridgeExecutable, const fs::path& p_modelFolder, const std::string& p_requestedModel, const std::string& p_token, const fs::path& p_readyFile)
{
	std::vector<std::string> l_argv;
	l_argv.push_back(p_bridgeExecutable.string());
	l_argv.push_back("--host");
	l_argv.push_back("127.0.0.1");
	l_argv.push_back("--port");
	l_argv.push_back("0");
	l_argv.push_back("--model-folder");
	l_argv.push_back(p_modelFolder.string());

	if (!Trim(p_requestedModel).empty())
	{
		l_argv.push_back("--default-model");
		l_argv.push_back(Trim(p_requestedModel));
	}

	if (!Trim(p_token).empty())
	{
		l_argv.push_back("--token");
		l_argv.push_back(p_token);
	}

	l_argv.push_back("--ready-file");
	l_argv.push_back(p_readyFile.string());
	return l_argv;
}

static bool StartOpenCodeBridge(AppState& p_app, const fs::path& p_modelFolder, const std::string& p_requestedModel, std::string* p_errorOut = nullptr)
{
	fs::path l_normalizedModelFolder = NormalizeAbsolutePath(p_modelFolder);

	if (l_normalizedModelFolder.empty())
	{
		l_normalizedModelFolder = p_modelFolder;
	}

	const fs::path l_bridgeExecutable = ResolveOpenCodeBridgeExecutablePath();

	if (l_bridgeExecutable.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Could not resolve uam_ollama_engine_bridge executable.";
		}

		return false;
	}

	if (p_app.opencode_bridge.token.empty())
	{
		p_app.opencode_bridge.token = OpenCodeBridgeRandomHex(48);
	}

	const fs::path l_readyFile = BuildOpenCodeBridgeReadyFilePath(p_app);
	std::error_code l_rmEc;
	fs::remove(l_readyFile, l_rmEc);

	const std::vector<std::string> l_argv = BuildOpenCodeBridgeArgv(l_bridgeExecutable, l_normalizedModelFolder, p_requestedModel, p_app.opencode_bridge.token, l_readyFile);

	if (!StartOpenCodeBridgeProcess(p_app, l_argv, p_errorOut))
	{
		p_app.opencode_bridge.last_error = (p_errorOut != nullptr) ? *p_errorOut : "OpenCode bridge process launch failed.";
		return false;
	}

	OpenCodeBridgeReadyInfo l_readyInfo;
	std::string l_readyError;

	if (!WaitForOpenCodeBridgeReadyFile(p_app, l_readyFile, &l_readyInfo, &l_readyError))
	{
		StopOpenCodeBridge(p_app);

		if (p_errorOut != nullptr)
		{
			*p_errorOut = l_readyError.empty() ? "OpenCode bridge did not become ready." : l_readyError;
		}

		p_app.opencode_bridge.last_error = (p_errorOut != nullptr) ? *p_errorOut : "OpenCode bridge startup failed.";
		return false;
	}

	p_app.opencode_bridge.endpoint = l_readyInfo.m_endpoint;
	p_app.opencode_bridge.api_base = l_readyInfo.m_apiBase.empty() ? (l_readyInfo.m_endpoint + "/v1") : l_readyInfo.m_apiBase;
	p_app.opencode_bridge.selected_model = l_readyInfo.m_model.empty() ? Trim(p_requestedModel) : l_readyInfo.m_model;
	p_app.opencode_bridge.requested_model = Trim(p_requestedModel);
	p_app.opencode_bridge.model_folder = l_normalizedModelFolder.string();
	p_app.opencode_bridge.ready_file = l_readyFile.string();
	p_app.opencode_bridge.running = true;
	p_app.opencode_bridge.healthy = false;

	std::string l_healthError;

	if (!ProbeOpenCodeBridgeHealth(p_app, &l_healthError))
	{
		StopOpenCodeBridge(p_app);

		if (p_errorOut != nullptr)
		{
			*p_errorOut = l_healthError.empty() ? "OpenCode bridge health check failed." : l_healthError;
		}

		p_app.opencode_bridge.last_error = (p_errorOut != nullptr) ? *p_errorOut : "OpenCode bridge health check failed.";
		return false;
	}

	p_app.opencode_bridge.healthy = true;

	std::string l_configError;

	if (!EnsureOpenCodeConfigProvisioned(p_app, &l_configError))
	{
		StopOpenCodeBridge(p_app);

		if (p_errorOut != nullptr)
		{
			*p_errorOut = l_configError.empty() ? "OpenCode config provisioning failed." : l_configError;
		}

		p_app.opencode_bridge.last_error = (p_errorOut != nullptr) ? *p_errorOut : "OpenCode config provisioning failed.";
		return false;
	}

	p_app.opencode_bridge.last_error.clear();
	return true;
}

static void StopOpenCodeBridge(AppState& p_app)
{
	PlatformServicesFactory::Instance().process_service.StopOpenCodeBridgeProcess(p_app.opencode_bridge);

	const std::string l_preservedToken = p_app.opencode_bridge.token;
	ResetOpenCodeBridgeRuntimeFields(p_app, true);
	p_app.opencode_bridge.token = l_preservedToken;
}

static bool RestartOpenCodeBridgeIfModelChanged(AppState& p_app, std::string* p_errorOut)
{
	const fs::path l_desiredModelFolderPath = ResolveRagModelFolder(p_app);
	fs::path l_desiredModelFolderNorm = NormalizeAbsolutePath(l_desiredModelFolderPath);

	if (l_desiredModelFolderNorm.empty())
	{
		l_desiredModelFolderNorm = l_desiredModelFolderPath;
	}

	const std::string l_desiredModelFolder = l_desiredModelFolderNorm.string();
	const std::string l_desiredRequestedModel = Trim(p_app.settings.selected_model_id);

	const bool l_processRunning = IsOpenCodeBridgeProcessRunning(p_app);
	const bool l_signatureMatches = l_processRunning && p_app.opencode_bridge.model_folder == l_desiredModelFolder && p_app.opencode_bridge.requested_model == l_desiredRequestedModel && !Trim(p_app.opencode_bridge.endpoint).empty() && !Trim(p_app.opencode_bridge.api_base).empty();

	if (l_signatureMatches)
	{
		std::string l_healthError;

		if (!ProbeOpenCodeBridgeHealth(p_app, &l_healthError))
		{
			StopOpenCodeBridge(p_app);

			if (!StartOpenCodeBridge(p_app, l_desiredModelFolderNorm, l_desiredRequestedModel, p_errorOut))
			{
				return false;
			}

			return true;
		}

		p_app.opencode_bridge.healthy = true;
		std::string l_configError;

		if (!EnsureOpenCodeConfigProvisioned(p_app, &l_configError))
		{
			if (p_errorOut != nullptr)
			{
				*p_errorOut = l_configError;
			}

			p_app.opencode_bridge.last_error = l_configError;
			return false;
		}

		return true;
	}

	StopOpenCodeBridge(p_app);
	return StartOpenCodeBridge(p_app, l_desiredModelFolderNorm, l_desiredRequestedModel, p_errorOut);
}

static bool EnsureOpenCodeBridgeRunning(AppState& p_app, std::string* p_errorOut)
{
	const bool l_ok = RestartOpenCodeBridgeIfModelChanged(p_app, p_errorOut);

	if (!l_ok && p_errorOut != nullptr && p_errorOut->empty())
	{
		*p_errorOut = "Failed to ensure OpenCode bridge is running.";
	}

	if (!l_ok)
	{
		p_app.opencode_bridge.healthy = false;
	}

	return l_ok;
}

static fs::path SettingsFilePath(const AppState& p_app)
{
	return AppPaths::SettingsFilePath(p_app.data_root);
}

static fs::path ChatsRootPath(const AppState& p_app)
{
	return AppPaths::ChatsRootPath(p_app.data_root);
}

static fs::path ChatPath(const AppState& p_app, const ChatSession& p_chat)
{
	return AppPaths::ChatPath(p_app.data_root, p_chat.id);
}

static fs::path DefaultDataRootPath()
{
	return AppPaths::DefaultDataRootPath();
}

static fs::path TempFallbackDataRootPath()
{
	std::error_code l_ec;
	const fs::path l_temp = fs::temp_directory_path(l_ec);

	if (!l_ec)
	{
		return l_temp / "universal_agent_manager_data";
	}

	return fs::path("data");
}

static bool EnsureDataRootLayout(const fs::path& p_dataRoot, std::string* p_errorOut)
{
	std::error_code l_ec;
	fs::create_directories(p_dataRoot, l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create data root '" + p_dataRoot.string() + "': " + l_ec.message();
		}

		return false;
	}

	fs::create_directories(p_dataRoot / "chats", l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to create chats dir '" + (p_dataRoot / "chats").string() + "': " + l_ec.message();
		}

		return false;
	}

	return true;
}

static std::optional<fs::path> ResolveGeminiProjectTmpDir(const fs::path& p_projectRoot)
{
	return AppPaths::ResolveGeminiProjectTmpDir(p_projectRoot);
}

static GeminiNativeHistoryStoreOptions NativeGeminiHistoryOptions()
{
	GeminiNativeHistoryStoreOptions options;
	options.max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	options.max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
	return options;
}

static ProviderRuntimeHistoryLoadOptions RuntimeHistoryLoadOptions()
{
	const GeminiNativeHistoryStoreOptions native_options = NativeGeminiHistoryOptions();
	ProviderRuntimeHistoryLoadOptions options;
	options.native_max_file_bytes = native_options.max_file_bytes;
	options.native_max_messages = native_options.max_messages;
	return options;
}

static std::optional<ChatSession> ParseGeminiSessionFile(const fs::path& p_filePath, const ProviderProfile& p_provider)
{
	return GeminiNativeHistoryStore::ParseFile(p_filePath, p_provider, NativeGeminiHistoryOptions());
}

static std::vector<ChatSession> LoadNativeGeminiChats(const fs::path& p_chatsDir, const ProviderProfile& p_provider)
{
	return DeduplicateChatsById(ProviderRuntime::LoadHistory(p_provider, fs::path{}, p_chatsDir, RuntimeHistoryLoadOptions()));
}

static bool StartAsyncNativeChatLoad(AppState& app)
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!ActiveProviderUsesGeminiHistory(app) || app.native_chat_load_task.running)
	{
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

	auto l_loadNativeChatsTask = [chats_dir, provider, completed, chats, error]()
	{
		try
		{
			*chats = LoadNativeGeminiChats(chats_dir, provider);
		}
		catch (const std::exception& ex)
		{
			*error = ex.what();
		}
		catch (...)
		{
			*error = "Unknown native chat load failure.";
		}

		completed->store(true, std::memory_order_release);
	};

	std::thread(l_loadNativeChatsTask).detach();
	return true;
}

static bool TryConsumeAsyncNativeChatLoad(AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out)
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!app.native_chat_load_task.running)
	{
		return false;
	}

	if (app.native_chat_load_task.completed == nullptr || app.native_chat_load_task.chats == nullptr || app.native_chat_load_task.error == nullptr)
	{
		app.native_chat_load_task.running = false;
		app.native_chat_load_task.completed.reset();
		app.native_chat_load_task.chats.reset();
		app.native_chat_load_task.error.reset();
		chats_out.clear();
		error_out.clear();
		return true;
	}

	if (!app.native_chat_load_task.completed->load(std::memory_order_acquire))
	{
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

static std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& p_chats)
{
	std::vector<std::string> l_ids;
	l_ids.reserve(p_chats.size());

	for (const ChatSession& l_chat : p_chats)
	{
		if (l_chat.uses_native_session && !l_chat.native_session_id.empty())
		{
			l_ids.push_back(l_chat.native_session_id);
		}
	}

	return l_ids;
}

static std::string NewFolderId()
{
	const auto l_now = std::chrono::system_clock::now().time_since_epoch();
	const auto l_epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(l_now).count();
	std::ostringstream l_id;
	l_id << "folder-" << l_epochMs;
	return l_id.str();
}

static int FindFolderIndexById(const AppState& p_app, const std::string& p_folderId)
{
	for (int l_i = 0; l_i < static_cast<int>(p_app.folders.size()); ++l_i)
	{
		if (p_app.folders[l_i].id == p_folderId)
		{
			return l_i;
		}
	}

	return -1;
}

static ChatFolder* FindFolderById(AppState& p_app, const std::string& p_folderId)
{
	const int l_idx = FindFolderIndexById(p_app, p_folderId);
	return (l_idx >= 0) ? &p_app.folders[l_idx] : nullptr;
}

static const ChatFolder* FindFolderById(const AppState& p_app, const std::string& p_folderId)
{
	const int l_idx = FindFolderIndexById(p_app, p_folderId);
	return (l_idx >= 0) ? &p_app.folders[l_idx] : nullptr;
}

static int CountChatsInFolder(const AppState& p_app, const std::string& p_folderId);

static void EnsureDefaultFolder(AppState& p_app)
{
	if (FindFolderIndexById(p_app, kDefaultFolderId) >= 0)
	{
		return;
	}

	ChatFolder l_folder;
	l_folder.id = kDefaultFolderId;
	l_folder.title = kDefaultFolderTitle;
	l_folder.directory = fs::current_path().string();
	l_folder.collapsed = false;
	p_app.folders.push_back(std::move(l_folder));
}

static void EnsureNewChatFolderSelection(AppState& p_app)
{
	EnsureDefaultFolder(p_app);

	if (p_app.new_chat_folder_id.empty() || FindFolderById(p_app, p_app.new_chat_folder_id) == nullptr)
	{
		p_app.new_chat_folder_id = kDefaultFolderId;
	}
}

static void NormalizeChatFolderAssignments(AppState& p_app)
{
	EnsureDefaultFolder(p_app);

	for (ChatSession& l_chat : p_app.chats)
	{
		if (l_chat.folder_id.empty() || FindFolderById(p_app, l_chat.folder_id) == nullptr)
		{
			l_chat.folder_id = kDefaultFolderId;
		}
	}

	bool l_anyExpandedWithChats = false;

	for (const ChatFolder& l_folder : p_app.folders)
	{
		if (!l_folder.collapsed && CountChatsInFolder(p_app, l_folder.id) > 0)
		{
			l_anyExpandedWithChats = true;
			break;
		}
	}

	if (!l_anyExpandedWithChats)
	{
		for (ChatFolder& l_folder : p_app.folders)
		{
			if (CountChatsInFolder(p_app, l_folder.id) > 0)
			{
				l_folder.collapsed = false;
			}
		}
	}

	EnsureNewChatFolderSelection(p_app);
}

static void SaveFolders(const AppState& p_app)
{
	ChatFolderStore::Save(p_app.data_root, p_app.folders);
}

static void SaveProviders(const AppState& p_app)
{
	ProviderProfileStore::Save(p_app.data_root, p_app.provider_profiles);
}

static fs::path ProviderProfileFilePath(const AppState& p_app)
{
	return p_app.data_root / "providers.txt";
}

static fs::path FrontendActionFilePath(const AppState& p_app)
{
	return p_app.data_root / "frontend_actions.txt";
}

static bool IsLegacyBuiltInProviderId(const std::string& p_providerId)
{
	const std::string l_lowered = ToLowerAscii(Trim(p_providerId));
	return l_lowered == "gemini" || l_lowered == "codex" || l_lowered == "claude" || l_lowered == "opencode";
}

static bool IsGeminiProviderId(const std::string& p_providerId)
{
	const std::string l_lowered = ToLowerAscii(Trim(p_providerId));
	return l_lowered == "gemini" || l_lowered == "gemini-cli" || l_lowered == "gemini-structured";
}

static std::string MapLegacyProviderId(const std::string& p_providerId, const bool p_preferCliForGemini)
{
	const std::string l_trimmed = Trim(p_providerId);
	const std::string l_lowered = ToLowerAscii(l_trimmed);

	if (l_lowered == "gemini")
	{
		return p_preferCliForGemini ? "gemini-cli" : "gemini-structured";
	}

	if (l_lowered == "codex")
	{
		return "codex-cli";
	}

	if (l_lowered == "claude")
	{
		return "claude-cli";
	}

	if (l_lowered == "opencode")
	{
		return "opencode-cli";
	}

	return l_trimmed;
}

static std::string DefaultGeminiProviderIdForLegacyViewHint(const AppState& p_app)
{
	return (p_app.center_view_mode == CenterViewMode::CliConsole) ? "gemini-cli" : "gemini-structured";
}

static bool ChatHasCliViewHint(const AppState& p_app, const ChatSession& p_chat)
{
	for (const auto& l_terminal : p_app.cli_terminals)
	{
		if (l_terminal == nullptr || l_terminal->attached_chat_id != p_chat.id)
		{
			continue;
		}

		if (l_terminal->running || l_terminal->should_launch || l_terminal->input_ready || l_terminal->generation_in_progress)
		{
			return true;
		}
	}

	return false;
}

static bool ShouldShowProviderProfileInUi(const ProviderProfile& p_profile)
{
	return !IsLegacyBuiltInProviderId(p_profile.id);
}

static bool MigrateProviderProfilesToFixedModeIds(AppState& p_app)
{
	bool l_changed = false;
	std::vector<ProviderProfile> l_migrated;
	l_migrated.reserve(p_app.provider_profiles.size());
	std::unordered_set<std::string> l_seenIds;

	for (ProviderProfile l_profile : p_app.provider_profiles)
	{
		const std::string l_originalId = Trim(l_profile.id);
		const std::string l_mappedId = MapLegacyProviderId(l_originalId, false);

		if (l_mappedId != l_originalId)
		{
			l_changed = true;
		}

		if (l_mappedId.empty())
		{
			l_changed = true;
			continue;
		}

		l_profile.id = l_mappedId;
		const auto l_assignIfChanged = [&](auto& p_field, const auto& p_value)
		{
			if (p_field != p_value)
			{
				p_field = p_value;
				l_changed = true;
			}
		};

		if (l_mappedId == "gemini-structured")
		{
			l_assignIfChanged(l_profile.output_mode, std::string("structured"));
			l_assignIfChanged(l_profile.supports_interactive, false);

			if (Trim(l_profile.command_template).empty() || l_profile.command_template == "gemini {resume} {flags} {prompt}")
			{
				l_assignIfChanged(l_profile.command_template, std::string("gemini {resume} {flags} -p {prompt}"));
			}

			if (Trim(l_profile.history_adapter).empty())
			{
				l_assignIfChanged(l_profile.history_adapter, std::string("gemini-cli-json"));
			}

			if (Trim(l_profile.prompt_bootstrap).empty())
			{
				l_assignIfChanged(l_profile.prompt_bootstrap, std::string("gemini-at-path"));
			}

			if (Trim(l_profile.prompt_bootstrap_path).empty())
			{
				l_assignIfChanged(l_profile.prompt_bootstrap_path, std::string("@.gemini/gemini.md"));
			}
		}
		else if (l_mappedId == "gemini-cli")
		{
			l_assignIfChanged(l_profile.output_mode, std::string("cli"));
			l_assignIfChanged(l_profile.supports_interactive, true);

			if (Trim(l_profile.command_template).empty())
			{
				l_assignIfChanged(l_profile.command_template, std::string("gemini {resume} {flags} {prompt}"));
			}

			if (Trim(l_profile.history_adapter).empty())
			{
				l_assignIfChanged(l_profile.history_adapter, std::string("gemini-cli-json"));
			}

			if (Trim(l_profile.prompt_bootstrap).empty())
			{
				l_assignIfChanged(l_profile.prompt_bootstrap, std::string("gemini-at-path"));
			}

			if (Trim(l_profile.prompt_bootstrap_path).empty())
			{
				l_assignIfChanged(l_profile.prompt_bootstrap_path, std::string("@.gemini/gemini.md"));
			}
		}
		else if (l_mappedId == "codex-cli" || l_mappedId == "claude-cli" || l_mappedId == "opencode-cli" || l_mappedId == "opencode-local")
		{
			l_assignIfChanged(l_profile.output_mode, std::string("cli"));
			l_assignIfChanged(l_profile.supports_interactive, true);
		}
		else if (l_mappedId == "ollama-engine")
		{
			l_assignIfChanged(l_profile.output_mode, std::string("structured"));
			l_assignIfChanged(l_profile.supports_interactive, false);
		}

		const std::string l_dedupeKey = ToLowerAscii(l_profile.id);

		if (!l_seenIds.insert(l_dedupeKey).second)
		{
			l_changed = true;
			continue;
		}

		l_migrated.push_back(std::move(l_profile));
	}

	if (l_migrated.size() != p_app.provider_profiles.size())
	{
		l_changed = true;
	}

	p_app.provider_profiles = std::move(l_migrated);
	ProviderProfileStore::EnsureDefaultProfile(p_app.provider_profiles);

	std::vector<ProviderProfile> l_filtered;
	l_filtered.reserve(p_app.provider_profiles.size());

	for (const ProviderProfile& l_profile : p_app.provider_profiles)
	{
		if (IsLegacyBuiltInProviderId(l_profile.id))
		{
			l_changed = true;
			continue;
		}

		l_filtered.push_back(l_profile);
	}

	p_app.provider_profiles = std::move(l_filtered);
	ProviderProfileStore::EnsureDefaultProfile(p_app.provider_profiles);
	return l_changed;
}

static bool MigrateActiveProviderIdToFixedModes(AppState& p_app)
{
	bool l_changed = false;
	const std::string l_mappedId = MapLegacyProviderId(p_app.settings.active_provider_id, p_app.center_view_mode == CenterViewMode::CliConsole);

	if (l_mappedId != p_app.settings.active_provider_id)
	{
		p_app.settings.active_provider_id = l_mappedId;
		l_changed = true;
	}

	if (Trim(p_app.settings.active_provider_id).empty())
	{
		p_app.settings.active_provider_id = DefaultGeminiProviderIdForLegacyViewHint(p_app);
		l_changed = true;
	}

	if (ProviderProfileStore::FindById(p_app.provider_profiles, p_app.settings.active_provider_id) == nullptr)
	{
		p_app.settings.active_provider_id = DefaultGeminiProviderIdForLegacyViewHint(p_app);

		if (ProviderProfileStore::FindById(p_app.provider_profiles, p_app.settings.active_provider_id) == nullptr && !p_app.provider_profiles.empty())
		{
			p_app.settings.active_provider_id = p_app.provider_profiles.front().id;
		}

		l_changed = true;
	}

	return l_changed;
}

static ProviderProfile* ActiveProvider(AppState& p_app)
{
	ProviderProfile* lp_found = ProviderProfileStore::FindById(p_app.provider_profiles, p_app.settings.active_provider_id);

	if (lp_found != nullptr)
	{
		return lp_found;
	}

	ProviderProfileStore::EnsureDefaultProfile(p_app.provider_profiles);
	p_app.settings.active_provider_id = "gemini-structured";
	return ProviderProfileStore::FindById(p_app.provider_profiles, p_app.settings.active_provider_id);
}

static const ProviderProfile* ActiveProvider(const AppState& p_app)
{
	return ProviderProfileStore::FindById(p_app.provider_profiles, p_app.settings.active_provider_id);
}

static const ProviderProfile& ActiveProviderOrDefault(const AppState& p_app)
{
	const ProviderProfile* lcp_profile = ActiveProvider(p_app);

	if (lcp_profile != nullptr)
	{
		return *lcp_profile;
	}

	static const ProviderProfile l_fallback = ProviderProfileStore::DefaultGeminiProfile();
	return l_fallback;
}

static const ProviderProfile* ProviderForChat(const AppState& p_app, const ChatSession& p_chat)
{
	const std::string l_preferred = Trim(p_chat.provider_id);

	if (!l_preferred.empty())
	{
		if (const ProviderProfile* lcp_profile = ProviderProfileStore::FindById(p_app.provider_profiles, l_preferred); lcp_profile != nullptr)
		{
			return lcp_profile;
		}
	}

	return ActiveProvider(p_app);
}

static const ProviderProfile& ProviderForChatOrDefault(const AppState& p_app, const ChatSession& p_chat)
{
	if (const ProviderProfile* lcp_profile = ProviderForChat(p_app, p_chat); lcp_profile != nullptr)
	{
		return *lcp_profile;
	}

	return ActiveProviderOrDefault(p_app);
}

static bool ActiveProviderUsesGeminiHistory(const AppState& p_app)
{
	const ProviderProfile* lcp_profile = ActiveProvider(p_app);
	return lcp_profile != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*lcp_profile);
}

static bool ActiveProviderUsesInternalEngine(const AppState& p_app)
{
	const ProviderProfile* lcp_profile = ActiveProvider(p_app);
	return lcp_profile != nullptr && ProviderRuntime::UsesInternalEngine(*lcp_profile);
}

static bool ChatUsesGeminiHistory(const AppState& p_app, const ChatSession& p_chat)
{
	const ProviderProfile* lcp_profile = ProviderForChat(p_app, p_chat);
	return lcp_profile != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*lcp_profile);
}

static bool ChatUsesInternalEngine(const AppState& p_app, const ChatSession& p_chat)
{
	const ProviderProfile* lcp_profile = ProviderForChat(p_app, p_chat);
	return lcp_profile != nullptr && ProviderRuntime::UsesInternalEngine(*lcp_profile);
}

static bool ChatUsesCliOutput(const AppState& p_app, const ChatSession& p_chat)
{
	const ProviderProfile* lcp_profile = ProviderForChat(p_app, p_chat);
	return lcp_profile != nullptr && ProviderRuntime::UsesCliOutput(*lcp_profile);
}

static const uam::FrontendAction* FindFrontendAction(const AppState& p_app, const std::string& p_key)
{
	return uam::FindAction(p_app.frontend_actions, p_key);
}

static bool FrontendActionVisible(const AppState& p_app, const std::string& p_key, const bool p_fallbackVisible)
{
	const uam::FrontendAction* lcp_action = FindFrontendAction(p_app, p_key);
	return (lcp_action == nullptr) ? p_fallbackVisible : lcp_action->visible;
}

static std::string FrontendActionLabel(const AppState& p_app, const std::string& p_key, const std::string& p_fallbackLabel)
{
	const uam::FrontendAction* lcp_action = FindFrontendAction(p_app, p_key);

	if (lcp_action == nullptr || Trim(lcp_action->label).empty())
	{
		return p_fallbackLabel;
	}

	return lcp_action->label;
}

static void LoadFrontendActions(AppState& p_app)
{
	std::string l_error;

	if (!uam::LoadFrontendActionMap(FrontendActionFilePath(p_app), p_app.frontend_actions, &l_error))
	{
		p_app.frontend_actions = uam::DefaultFrontendActionMap();

		if (!uam::SaveFrontendActionMap(FrontendActionFilePath(p_app), p_app.frontend_actions, &l_error) && !l_error.empty())
		{
			p_app.status_line = "Frontend action map reset, but saving failed: " + l_error;
		}
		else if (!l_error.empty())
		{
			p_app.status_line = "Frontend action map was invalid and has been reset.";
		}

		return;
	}

	uam::NormalizeFrontendActionMap(p_app.frontend_actions);
}

static std::string FolderForNewChat(const AppState& p_app)
{
	if (!p_app.new_chat_folder_id.empty())
	{
		return p_app.new_chat_folder_id;
	}

	return kDefaultFolderId;
}

static int CountChatsInFolder(const AppState& p_app, const std::string& p_folderId)
{
	int l_count = 0;

	for (const ChatSession& l_chat : p_app.chats)
	{
		if (l_chat.folder_id == p_folderId)
		{
			++l_count;
		}
	}

	return l_count;
}

static std::string FolderTitleOrFallback(const ChatFolder& p_folder)
{
	const std::string l_trimmed = Trim(p_folder.title);
	return l_trimmed.empty() ? "Untitled Folder" : l_trimmed;
}

static int FindChatIndexById(const AppState& p_app, const std::string& p_chatId)
{
	for (int l_i = 0; l_i < static_cast<int>(p_app.chats.size()); ++l_i)
	{
		if (p_app.chats[l_i].id == p_chatId)
		{
			return l_i;
		}
	}

	return -1;
}

static ChatSession* SelectedChat(AppState& p_app)
{
	if (p_app.selected_chat_index < 0 || p_app.selected_chat_index >= static_cast<int>(p_app.chats.size()))
	{
		return nullptr;
	}

	return &p_app.chats[p_app.selected_chat_index];
}

static const ChatSession* SelectedChat(const AppState& p_app)
{
	if (p_app.selected_chat_index < 0 || p_app.selected_chat_index >= static_cast<int>(p_app.chats.size()))
	{
		return nullptr;
	}

	return &p_app.chats[p_app.selected_chat_index];
}

static void SortChatsByRecent(std::vector<ChatSession>& p_chats)
{
	std::sort(p_chats.begin(), p_chats.end(), [](const ChatSession& p_a, const ChatSession& p_b) { return p_a.updated_at > p_b.updated_at; });
}

static bool ShouldReplaceChatForDuplicateId(const ChatSession& p_candidate, const ChatSession& p_existing)
{
	if (p_candidate.uses_native_session != p_existing.uses_native_session)
	{
		return p_candidate.uses_native_session && !p_existing.uses_native_session;
	}

	if (p_candidate.messages.size() != p_existing.messages.size())
	{
		return p_candidate.messages.size() > p_existing.messages.size();
	}

	if (p_candidate.updated_at != p_existing.updated_at)
	{
		return p_candidate.updated_at > p_existing.updated_at;
	}

	if (p_candidate.created_at != p_existing.created_at)
	{
		return p_candidate.created_at > p_existing.created_at;
	}

	if (p_candidate.linked_files.size() != p_existing.linked_files.size())
	{
		return p_candidate.linked_files.size() > p_existing.linked_files.size();
	}

	if (p_candidate.provider_id != p_existing.provider_id)
	{
		return !p_candidate.provider_id.empty();
	}

	if (p_candidate.template_override_id != p_existing.template_override_id)
	{
		return !p_candidate.template_override_id.empty();
	}

	if (p_candidate.parent_chat_id != p_existing.parent_chat_id)
	{
		return !p_candidate.parent_chat_id.empty();
	}

	if (p_candidate.branch_root_chat_id != p_existing.branch_root_chat_id)
	{
		return !p_candidate.branch_root_chat_id.empty();
	}

	if (p_candidate.branch_from_message_index != p_existing.branch_from_message_index)
	{
		return p_candidate.branch_from_message_index > p_existing.branch_from_message_index;
	}

	return false;
}

static std::vector<ChatSession> DeduplicateChatsById(std::vector<ChatSession> p_chats)
{
	std::vector<ChatSession> l_deduped;
	l_deduped.reserve(p_chats.size());
	std::unordered_map<std::string, std::size_t> l_indexById;
	std::unordered_map<std::string, std::size_t> l_indexByNativeSessionId;

	for (ChatSession& l_chat : p_chats)
	{
		l_chat.id = Trim(l_chat.id);

		if (l_chat.id.empty())
		{
			continue;
		}

		const std::string l_nativeSessionId = Trim(l_chat.native_session_id);
		const bool l_hasNativeIdentity = l_chat.uses_native_session && !l_nativeSessionId.empty();
		const std::string l_nativeKey = l_hasNativeIdentity ? ("native:" + l_nativeSessionId) : std::string{};

		if (l_hasNativeIdentity)
		{
			const auto l_nativeIt = l_indexByNativeSessionId.find(l_nativeKey);

			if (l_nativeIt != l_indexByNativeSessionId.end())
			{
				ChatSession& l_existing = l_deduped[l_nativeIt->second];

				if (ShouldReplaceChatForDuplicateId(l_chat, l_existing) || l_existing.id != l_existing.native_session_id)
				{
					l_existing = std::move(l_chat);

					if (l_existing.id != l_existing.native_session_id && !l_existing.native_session_id.empty())
					{
						l_existing.id = l_existing.native_session_id;
					}

					l_indexById[l_existing.id] = l_nativeIt->second;
				}

				continue;
			}
		}

		const auto l_it = l_indexById.find(l_chat.id);

		if (l_it == l_indexById.end())
		{
			if (l_hasNativeIdentity && l_chat.id != l_nativeSessionId)
			{
				l_chat.id = l_nativeSessionId;
			}

			const std::size_t l_nextIndex = l_deduped.size();
			l_indexById[l_chat.id] = l_nextIndex;

			if (l_hasNativeIdentity)
			{
				l_indexByNativeSessionId[l_nativeKey] = l_nextIndex;
			}

			l_deduped.push_back(std::move(l_chat));
			continue;
		}

		ChatSession& l_existing = l_deduped[l_it->second];

		if (ShouldReplaceChatForDuplicateId(l_chat, l_existing))
		{
			l_existing = std::move(l_chat);

			if (l_existing.uses_native_session && !l_existing.native_session_id.empty() && l_existing.id != l_existing.native_session_id)
			{
				l_existing.id = l_existing.native_session_id;
			}
		}

		if (l_existing.uses_native_session && !l_existing.native_session_id.empty())
		{
			l_indexByNativeSessionId["native:" + l_existing.native_session_id] = l_it->second;
		}
	}

	SortChatsByRecent(l_deduped);
	return l_deduped;
}

static void RefreshRememberedSelection(AppState& p_app)
{
	if (!p_app.settings.remember_last_chat)
	{
		p_app.settings.last_selected_chat_id.clear();
		return;
	}

	const ChatSession* lcp_selected = SelectedChat(p_app);
	p_app.settings.last_selected_chat_id = (lcp_selected != nullptr) ? lcp_selected->id : "";
}

static void SaveSettings(AppState& p_app)
{
	p_app.settings.ui_theme = NormalizeThemeChoice(p_app.settings.ui_theme);
	p_app.settings.runtime_backend = ActiveProviderUsesInternalEngine(p_app) ? "ollama-engine" : "provider-cli";
#if UAM_ENABLE_ENGINE_RAG
	p_app.settings.vector_db_backend = (p_app.settings.vector_db_backend == "none") ? "none" : "ollama-engine";
#else
	p_app.settings.vector_db_backend = "none";
	p_app.settings.selected_vector_model_id.clear();
#endif
	p_app.settings.selected_model_id = Trim(p_app.settings.selected_model_id);
	p_app.settings.models_folder_directory = Trim(p_app.settings.models_folder_directory);
	p_app.settings.selected_vector_model_id = Trim(p_app.settings.selected_vector_model_id);
	p_app.settings.vector_database_name_override = NormalizeVectorDatabaseName(p_app.settings.vector_database_name_override);
	p_app.settings.cli_idle_timeout_seconds = std::clamp(p_app.settings.cli_idle_timeout_seconds, 30, 3600);
	p_app.settings.rag_top_k = std::clamp(p_app.settings.rag_top_k, 1, 20);
	p_app.settings.rag_max_snippet_chars = std::clamp(p_app.settings.rag_max_snippet_chars, 120, 4000);
	p_app.settings.rag_max_file_bytes = std::clamp(p_app.settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024);
	p_app.settings.rag_scan_max_tokens = std::clamp(p_app.settings.rag_scan_max_tokens, 0, 32768);
	ClampWindowSettings(p_app.settings);
	SyncRagServiceConfig(p_app);

	if (p_app.opencode_bridge.running)
	{
		fs::path l_desiredModelFolder = NormalizeAbsolutePath(ResolveRagModelFolder(p_app));

		if (l_desiredModelFolder.empty())
		{
			l_desiredModelFolder = ResolveRagModelFolder(p_app);
		}

		const std::string l_desiredModel = Trim(p_app.settings.selected_model_id);

		if (p_app.opencode_bridge.model_folder != l_desiredModelFolder.string() || p_app.opencode_bridge.requested_model != l_desiredModel)
		{
			std::string l_bridgeError;

			if (!RestartOpenCodeBridgeIfModelChanged(p_app, &l_bridgeError) && !l_bridgeError.empty())
			{
				p_app.status_line = l_bridgeError;
			}
		}
	}

	RefreshRememberedSelection(p_app);
	SettingsStore::Save(SettingsFilePath(p_app), p_app.settings, p_app.center_view_mode);
}

static void LoadSettings(AppState& p_app)
{
	SettingsStore::Load(SettingsFilePath(p_app), p_app.settings, p_app.center_view_mode);
#if UAM_ENABLE_ENGINE_RAG
	p_app.settings.vector_db_backend = (p_app.settings.vector_db_backend == "none") ? "none" : "ollama-engine";
#else
	p_app.settings.vector_db_backend = "none";
	p_app.settings.selected_vector_model_id.clear();
#endif
	p_app.settings.selected_model_id = Trim(p_app.settings.selected_model_id);
	p_app.settings.models_folder_directory = Trim(p_app.settings.models_folder_directory);
	p_app.settings.selected_vector_model_id = Trim(p_app.settings.selected_vector_model_id);
	p_app.settings.vector_database_name_override = NormalizeVectorDatabaseName(p_app.settings.vector_database_name_override);
	p_app.settings.cli_idle_timeout_seconds = std::clamp(p_app.settings.cli_idle_timeout_seconds, 30, 3600);

	if (Trim(p_app.settings.prompt_profile_root_path).empty())
	{
		p_app.settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
	}

	SyncRagServiceConfig(p_app);
	p_app.rag_manual_query_max = std::clamp(p_app.settings.rag_top_k, 1, 20);
	p_app.rag_manual_query_min = 1;
}

static bool SaveChat(const AppState& p_app, const ChatSession& p_chat)
{
	const ProviderProfile& l_provider = ProviderForChatOrDefault(p_app, p_chat);
	return ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
}

static std::vector<ChatSession> LoadChats(const AppState& p_app)
{
	return LocalChatStore::Load(p_app.data_root);
}

static bool MigrateChatProviderBindingsToFixedModes(AppState& p_app)
{
	bool l_changed = false;
	const std::string l_fallbackProviderId = Trim(p_app.settings.active_provider_id).empty() ? DefaultGeminiProviderIdForLegacyViewHint(p_app) : p_app.settings.active_provider_id;

	for (ChatSession& l_chat : p_app.chats)
	{
		const std::string l_originalProviderId = l_chat.provider_id;
		const bool l_preferCliForLegacyGemini = ChatHasCliViewHint(p_app, l_chat) || p_app.center_view_mode == CenterViewMode::CliConsole;
		std::string l_mappedProviderId = MapLegacyProviderId(l_originalProviderId, l_preferCliForLegacyGemini);

		if (l_mappedProviderId.empty())
		{
			l_mappedProviderId = l_fallbackProviderId;
		}

		if (ProviderProfileStore::FindById(p_app.provider_profiles, l_mappedProviderId) == nullptr)
		{
			l_mappedProviderId = l_fallbackProviderId;
		}

		if (l_mappedProviderId.empty())
		{
			continue;
		}

		if (l_mappedProviderId != l_originalProviderId)
		{
			l_chat.provider_id = l_mappedProviderId;

			if (l_chat.updated_at.empty())
			{
				l_chat.updated_at = TimestampNow();
			}

			if (!SaveChat(p_app, l_chat))
			{
				p_app.status_line = "Failed to persist migrated provider id for chat " + CompactPreview(l_chat.id, 24) + ".";
			}

			l_changed = true;
		}
	}

	return l_changed;
}

static ChatSession CreateNewChat(const std::string& p_folderId, const std::string& p_providerId)
{
	ChatSession l_chat;
	l_chat.id = NewSessionId();
	l_chat.provider_id = p_providerId;
	l_chat.parent_chat_id.clear();
	l_chat.branch_root_chat_id = l_chat.id;
	l_chat.branch_from_message_index = -1;
	l_chat.folder_id = p_folderId;
	l_chat.created_at = TimestampNow();
	l_chat.updated_at = l_chat.created_at;
	l_chat.title = "Chat " + l_chat.created_at;
	return l_chat;
}

static bool IsLocalDraftChatId(const std::string& p_chatId)
{
	return p_chatId.rfind("chat-", 0) == 0;
}

static bool MessagesEquivalentForNativeLinking(const Message& p_localMessage, const Message& p_nativeMessage)
{
	return p_localMessage.role == p_nativeMessage.role && Trim(p_localMessage.content) == Trim(p_nativeMessage.content);
}

static bool IsMessagePrefixForNativeLinking(const std::vector<Message>& p_localMessages, const std::vector<Message>& p_nativeMessages)
{
	if (p_localMessages.empty() || p_localMessages.size() > p_nativeMessages.size())
	{
		return false;
	}

	for (std::size_t l_i = 0; l_i < p_localMessages.size(); ++l_i)
	{
		if (!MessagesEquivalentForNativeLinking(p_localMessages[l_i], p_nativeMessages[l_i]))
		{
			return false;
		}
	}

	return true;
}

static std::optional<std::string> SingleSessionIdFromSet(const std::unordered_set<std::string>& p_sessionIds)
{
	if (p_sessionIds.size() != 1)
	{
		return std::nullopt;
	}

	return *p_sessionIds.begin();
}

static std::optional<std::string> InferNativeSessionIdForLocalDraft(const ChatSession& p_localChat, const std::vector<ChatSession>& p_nativeChats)
{
	if (!IsLocalDraftChatId(p_localChat.id) || p_localChat.messages.size() < 2)
	{
		return std::nullopt;
	}

	std::unordered_set<std::string> l_exactMatchIds;
	std::unordered_set<std::string> l_prefixMatchIds;

	for (const ChatSession& l_nativeChat : p_nativeChats)
	{
		if (!l_nativeChat.uses_native_session || l_nativeChat.native_session_id.empty())
		{
			continue;
		}

		if (!IsMessagePrefixForNativeLinking(p_localChat.messages, l_nativeChat.messages))
		{
			continue;
		}

		l_prefixMatchIds.insert(l_nativeChat.native_session_id);

		if (p_localChat.messages.size() == l_nativeChat.messages.size())
		{
			l_exactMatchIds.insert(l_nativeChat.native_session_id);
		}
	}

	if (const auto l_exactMatch = SingleSessionIdFromSet(l_exactMatchIds); l_exactMatch.has_value())
	{
		return l_exactMatch;
	}

	if (p_localChat.messages.size() >= 3)
	{
		return SingleSessionIdFromSet(l_prefixMatchIds);
	}

	return std::nullopt;
}

static bool PersistLocalDraftNativeSessionLink(const AppState& p_app, ChatSession& p_localChat, const std::string& p_nativeSessionId)
{
	const std::string l_sessionId = Trim(p_nativeSessionId);

	if (l_sessionId.empty() || !IsLocalDraftChatId(p_localChat.id))
	{
		return false;
	}

	bool l_changed = false;

	if (!p_localChat.uses_native_session)
	{
		p_localChat.uses_native_session = true;
		l_changed = true;
	}

	if (p_localChat.native_session_id != l_sessionId)
	{
		p_localChat.native_session_id = l_sessionId;
		l_changed = true;
	}

	if (!l_changed)
	{
		return true;
	}

	if (p_localChat.updated_at.empty())
	{
		p_localChat.updated_at = TimestampNow();
	}

	return SaveChat(p_app, p_localChat);
}

static std::string ResolveResumeSessionIdForChat(const AppState& p_app, const ChatSession& p_chat)
{
	if (!ChatUsesGeminiHistory(p_app, p_chat))
	{
		return "";
	}

	const auto l_sessionExists = [&](const std::string& p_sessionId)
	{
		if (p_sessionId.empty() || p_app.gemini_chats_dir.empty())
		{
			return false;
		}

		std::error_code l_ec;
		return fs::exists(p_app.gemini_chats_dir / (p_sessionId + ".json"), l_ec) && !l_ec;
	};

	if (p_chat.uses_native_session)
	{
		if (!p_chat.native_session_id.empty())
		{
			return l_sessionExists(p_chat.native_session_id) ? p_chat.native_session_id : "";
		}

		if (!p_chat.id.empty())
		{
			return l_sessionExists(p_chat.id) ? p_chat.id : "";
		}

		return "";
	}

	// Legacy compatibility: older local snapshots of native chats may not
	// persist native flags.
	if (!p_chat.messages.empty() && !p_chat.id.empty() && !IsLocalDraftChatId(p_chat.id))
	{
		return l_sessionExists(p_chat.id) ? p_chat.id : "";
	}

	return "";
}

static void SelectChatById(AppState& p_app, const std::string& p_chatId)
{
	const ChatSession* lcp_previouslySelected = SelectedChat(p_app);
	const std::string l_previousId = (lcp_previouslySelected != nullptr) ? lcp_previouslySelected->id : "";
	p_app.selected_chat_index = FindChatIndexById(p_app, p_chatId);

	if (p_app.selected_chat_index >= 0)
	{
		p_app.chats_with_unseen_updates.erase(p_app.chats[p_app.selected_chat_index].id);
	}

	if (l_previousId != p_chatId)
	{
		p_app.composer_text.clear();
	}

	RefreshRememberedSelection(p_app);
}

static void AddMessage(ChatSession& p_chat, const MessageRole p_role, const std::string& p_text)
{
	Message l_message;
	l_message.role = p_role;
	l_message.content = p_text;
	l_message.created_at = TimestampNow();
	p_chat.messages.push_back(std::move(l_message));
	p_chat.updated_at = TimestampNow();

	if (p_chat.messages.size() == 1 && p_role == MessageRole::User)
	{
		std::string l_maybeTitle = Trim(p_text);

		if (l_maybeTitle.size() > 48)
		{
			l_maybeTitle = l_maybeTitle.substr(0, 45) + "...";
		}

		if (!l_maybeTitle.empty())
		{
			p_chat.title = l_maybeTitle;
		}
	}
}

static std::string BuildProviderPrompt(const ProviderProfile& p_provider, const std::string& p_userPrompt, const std::vector<std::string>& p_files)
{
	return ProviderRuntime::BuildPrompt(p_provider, p_userPrompt, p_files);
}

static std::string BuildProviderCommand(const ProviderProfile& p_provider, const AppSettings& p_settings, const std::string& p_prompt, const std::vector<std::string>& p_files, const std::string& p_resumeSessionId)
{
	return ProviderRuntime::BuildCommand(p_provider, p_settings, p_prompt, p_files, p_resumeSessionId);
}

static OllamaEngineClient& SharedOllamaEngineClient()
{
	return OllamaEngineService::Instance().Client();
}

static bool RuntimeUsesLocalEngine(const AppState& p_app)
{
	return ActiveProviderUsesInternalEngine(p_app);
}

static bool EnsureLocalRuntimeModelLoaded(AppState& p_app, std::string* p_errorOut = nullptr)
{
	const fs::path l_modelFolder = ResolveRagModelFolder(p_app);
	OllamaEngineClient& l_engine = SharedOllamaEngineClient();
	l_engine.SetModelFolder(l_modelFolder);
	l_engine.SetEmbeddingDimensions(256);

	if (Trim(p_app.settings.selected_model_id).empty())
	{
		return true;
	}

	if (p_app.loaded_runtime_model_id == p_app.settings.selected_model_id)
	{
		return true;
	}

	if (!l_engine.Load(p_app.settings.selected_model_id, p_errorOut))
	{
		return false;
	}

	p_app.loaded_runtime_model_id = p_app.settings.selected_model_id;
	return true;
}

static bool IsRuntimeEnabledForProvider(const ProviderProfile& p_provider, std::string* p_reasonOut)
{
	const bool l_enabled = ProviderRuntime::IsRuntimeEnabled(p_provider);

	if (p_reasonOut != nullptr)
	{
		p_reasonOut->clear();

		if (!l_enabled)
		{
			*p_reasonOut = ProviderRuntime::DisabledReason(p_provider);

			if (p_reasonOut->empty())
			{
				*p_reasonOut = "Runtime '" + p_provider.id + "' is disabled in this build.";
			}
		}
	}

	return l_enabled;
}

static bool ProviderUsesOpenCodeLocalBridge(const ProviderProfile& p_provider)
{
	return ToLowerAscii(Trim(p_provider.id)) == "opencode-local";
}

static bool EnsureSelectedLocalRuntimeModelForProvider(AppState& p_app)
{
	const fs::path l_modelFolder = ResolveRagModelFolder(p_app);
	OllamaEngineClient& l_engine = SharedOllamaEngineClient();
	l_engine.SetModelFolder(l_modelFolder);
	const std::vector<std::string> l_runtimeModels = l_engine.ListModels();
	const std::string l_selectedModel = Trim(p_app.settings.selected_model_id);
	const bool l_selectedModelValid = !l_selectedModel.empty() && std::find(l_runtimeModels.begin(), l_runtimeModels.end(), l_selectedModel) != l_runtimeModels.end();

	if (l_selectedModelValid)
	{
		return true;
	}

	if (l_runtimeModels.empty())
	{
		p_app.runtime_model_selection_id.clear();
		p_app.status_line = "No local runtime models found. Add one, then retry.";
	}
	else
	{
		if (Trim(p_app.runtime_model_selection_id).empty() || std::find(l_runtimeModels.begin(), l_runtimeModels.end(), p_app.runtime_model_selection_id) == l_runtimeModels.end())
		{
			p_app.runtime_model_selection_id = l_runtimeModels.front();
		}

		if (l_selectedModel.empty())
		{
			p_app.status_line = "Select a local runtime model to continue.";
		}
		else
		{
			p_app.status_line = "Selected model is unavailable. Choose a local "
			                    "runtime model to continue.";
		}
	}

	p_app.open_runtime_model_selection_popup = true;
	return false;
}

static TemplatePreflightOutcome PreflightWorkspaceTemplateForChat(AppState& p_app, const ProviderProfile& p_provider, const ChatSession& p_chat, std::string* p_bootstrapPromptOut = nullptr, std::string* p_statusOut = nullptr)
{
	RefreshTemplateCatalog(p_app);

	std::string l_effectiveTemplateId = p_chat.template_override_id;

	if (l_effectiveTemplateId.empty())
	{
		l_effectiveTemplateId = p_app.settings.default_prompt_profile_id;
	}

	if (l_effectiveTemplateId.empty())
	{
		l_effectiveTemplateId = p_app.settings.default_gemini_template_id;
	}

	if (l_effectiveTemplateId.empty())
	{
		if (p_statusOut != nullptr)
		{
			*p_statusOut = "No prompt profile selected. Set a default in Templates.";
		}

		return TemplatePreflightOutcome::ReadyWithoutTemplate;
	}

	const TemplateCatalogEntry* lcp_entry = FindTemplateEntryById(p_app, l_effectiveTemplateId);

	if (lcp_entry == nullptr)
	{
		if (p_statusOut != nullptr)
		{
			*p_statusOut = "Selected prompt profile is missing: " + l_effectiveTemplateId + ". Choose one in Templates.";
		}

		p_app.open_template_manager_popup = true;
		return TemplatePreflightOutcome::BlockingError;
	}

	if (ProviderRuntime::UsesGeminiPathBootstrap(p_provider))
	{
		if (!EnsureWorkspaceGeminiLayout(p_app, p_chat, p_statusOut))
		{
			return TemplatePreflightOutcome::BlockingError;
		}

		std::error_code l_ec;
		fs::copy_file(lcp_entry->absolute_path, WorkspaceGeminiTemplatePath(p_app, p_chat), fs::copy_options::overwrite_existing, l_ec);

		if (l_ec)
		{
			if (p_statusOut != nullptr)
			{
				*p_statusOut = "Failed to materialize .gemini/gemini.md: " + l_ec.message();
			}

			return TemplatePreflightOutcome::BlockingError;
		}

		if (p_bootstrapPromptOut != nullptr)
		{
			*p_bootstrapPromptOut = p_provider.prompt_bootstrap_path.empty() ? "@.gemini/gemini.md" : p_provider.prompt_bootstrap_path;
		}

		return TemplatePreflightOutcome::ReadyWithTemplate;
	}

	if (p_bootstrapPromptOut != nullptr)
	{
		*p_bootstrapPromptOut = ReadTextFile(lcp_entry->absolute_path);

		if (p_bootstrapPromptOut->empty())
		{
			if (p_statusOut != nullptr)
			{
				*p_statusOut = "Selected prompt profile is empty.";
			}

			return TemplatePreflightOutcome::ReadyWithoutTemplate;
		}
	}

	return TemplatePreflightOutcome::ReadyWithTemplate;
}

static bool QueueGeminiPromptForChat(AppState& p_app, ChatSession& p_chat, const std::string& p_prompt, const bool p_templateControlMessage = false)
{
	if (HasPendingCallForChat(p_app, p_chat.id))
	{
		p_app.status_line = "Provider command already running for this chat.";
		return false;
	}

	const std::string l_promptText = Trim(p_prompt);

	if (l_promptText.empty())
	{
		p_app.status_line = "Prompt is empty.";
		return false;
	}

	const ProviderProfile& l_provider = ProviderForChatOrDefault(p_app, p_chat);
	std::string l_runtimeDisabledReason;

	if (!IsRuntimeEnabledForProvider(l_provider, &l_runtimeDisabledReason))
	{
		p_app.status_line = l_runtimeDisabledReason;
		return false;
	}

	const bool l_useLocalRuntime = ChatUsesInternalEngine(p_app, p_chat);
	const bool l_useOpencodeLocalBridge = ProviderUsesOpenCodeLocalBridge(l_provider);
	std::string l_templateStatus;
	std::string l_bootstrapPrompt;
	TemplatePreflightOutcome l_templateOutcome = TemplatePreflightOutcome::ReadyWithoutTemplate;

	if (!p_templateControlMessage || !p_chat.gemini_md_bootstrapped)
	{
		l_templateOutcome = PreflightWorkspaceTemplateForChat(p_app, l_provider, p_chat, &l_bootstrapPrompt, &l_templateStatus);

		if (l_templateOutcome == TemplatePreflightOutcome::BlockingError)
		{
			p_app.status_line = l_templateStatus.empty() ? "Prompt profile preflight failed." : l_templateStatus;
			return false;
		}
	}

	const bool l_shouldBootstrapTemplate = !p_templateControlMessage && !p_chat.gemini_md_bootstrapped && p_chat.messages.empty() && l_templateOutcome == TemplatePreflightOutcome::ReadyWithTemplate;
	std::string l_runtimePrompt = l_promptText;

	if (!p_templateControlMessage)
	{
		l_runtimePrompt = BuildRagEnhancedPrompt(p_app, p_chat, l_promptText);
	}

	if (l_shouldBootstrapTemplate && !l_bootstrapPrompt.empty())
	{
		l_runtimePrompt = l_bootstrapPrompt + "\n\n" + l_runtimePrompt;
	}

	if ((l_useLocalRuntime || l_useOpencodeLocalBridge) && !EnsureSelectedLocalRuntimeModelForProvider(p_app))
	{
		return false;
	}

	AddMessage(p_chat, MessageRole::User, l_promptText);
	SaveAndUpdateStatus(p_app, p_chat, "Prompt queued for provider runtime.", "Saved message locally, but failed to persist chat data.");

	const bool l_useSharedCliSession = !l_useLocalRuntime && ProviderRuntime::UsesCliOutput(l_provider) && l_provider.supports_interactive;

	if (!l_useLocalRuntime && ProviderRuntime::UsesCliOutput(l_provider) && !l_provider.supports_interactive)
	{
		AddMessage(p_chat, MessageRole::System, "Provider is configured for CLI output but has no interactive runtime command.");
		SaveChat(p_app, p_chat);
		p_app.status_line = "Provider runtime configuration error.";
		return false;
	}

	if (l_useSharedCliSession)
	{
		std::string l_terminalError;

		if (!SendPromptToCliRuntime(p_app, p_chat, l_runtimePrompt, &l_terminalError))
		{
			AddMessage(p_chat, MessageRole::System, "Provider terminal send failed: " + (l_terminalError.empty() ? std::string("unknown error") : l_terminalError));
			SaveChat(p_app, p_chat);
			p_app.status_line = "Provider terminal send failed.";
			return false;
		}

		if (l_shouldBootstrapTemplate)
		{
			p_chat.gemini_md_bootstrapped = true;
			SaveChat(p_app, p_chat);
		}

		if (p_templateControlMessage)
		{
			p_app.status_line = "Prompt profile updated in live provider terminal session.";
		}
		else
		{
			p_app.status_line = "Prompt sent to live provider terminal session.";
		}

		p_app.scroll_to_bottom = true;
		return true;
	}

	if (l_useLocalRuntime)
	{
		std::string l_loadError;

		if (!EnsureLocalRuntimeModelLoaded(p_app, &l_loadError))
		{
			AddMessage(p_chat, MessageRole::System, "Local runtime model load failed: " + (l_loadError.empty() ? std::string("unknown error") : l_loadError));
			SaveChat(p_app, p_chat);
			p_app.status_line = "Local runtime model load failed.";
			return false;
		}

		const ollama_engine::SendMessageResponse l_response = SharedOllamaEngineClient().SendMessage(l_runtimePrompt);

		if (l_response.pbOk)
		{
			AddMessage(p_chat, MessageRole::Assistant, l_response.pSText);
			SaveAndUpdateStatus(p_app, p_chat, "Local response generated.", "Local response generated, but chat save failed.");
			p_app.scroll_to_bottom = true;
			return true;
		}

		AddMessage(p_chat, MessageRole::System, "Local runtime error: " + l_response.pSError);
		SaveChat(p_app, p_chat);
		p_app.status_line = "Local runtime command failed.";
		p_app.scroll_to_bottom = true;
		return false;
	}

	std::vector<ChatSession> l_nativeBefore;

	if (ChatUsesGeminiHistory(p_app, p_chat))
	{
		RefreshGeminiChatsDir(p_app);
		l_nativeBefore = LoadNativeGeminiChats(p_app.gemini_chats_dir, l_provider);
	}

	const std::string l_resumeSessionId = ResolveResumeSessionIdForChat(p_app, p_chat);
	const std::string l_providerPrompt = BuildProviderPrompt(l_provider, l_runtimePrompt, p_chat.linked_files);
	const std::string l_providerCommand = BuildProviderCommand(l_provider, p_app.settings, l_providerPrompt, p_chat.linked_files, l_resumeSessionId);
	const std::string l_command = BuildShellCommandWithWorkingDirectory(ResolveWorkspaceRootPath(p_app, p_chat), l_providerCommand);
	const std::string l_chatId = p_chat.id;

	PendingGeminiCall l_pending;
	l_pending.chat_id = l_chatId;
	l_pending.resume_session_id = l_resumeSessionId;
	l_pending.session_ids_before = SessionIdsFromChats(l_nativeBefore);
	l_pending.command_preview = l_command;
	l_pending.completed = std::make_shared<std::atomic<bool>>(false);
	l_pending.output = std::make_shared<std::string>();
	{
		std::shared_ptr<std::atomic<bool>> l_completed = l_pending.completed;
		std::shared_ptr<std::string> l_output = l_pending.output;
		auto l_runPendingCallTask = [l_command, l_completed, l_output]()
		{
			*l_output = ExecuteCommandCaptureOutput(l_command);
			l_completed->store(true, std::memory_order_release);
		};

		std::thread(l_runPendingCallTask).detach();
	}

	p_app.pending_calls.push_back(std::move(l_pending));

	if (l_shouldBootstrapTemplate)
	{
		p_chat.gemini_md_bootstrapped = true;
		SaveChat(p_app, p_chat);
	}

	if (p_templateControlMessage)
	{
		p_app.status_line = "Prompt profile updated and synced to provider bootstrap flow.";
	}
	else if (l_templateOutcome == TemplatePreflightOutcome::ReadyWithoutTemplate && !l_templateStatus.empty())
	{
		p_app.status_line = l_templateStatus;
	}

	p_app.scroll_to_bottom = true;
	return true;
}

static void SaveAndUpdateStatus(AppState& p_app, const ChatSession& p_chat, const std::string& p_success, const std::string& p_failure)
{
	if (SaveChat(p_app, p_chat))
	{
		p_app.status_line = p_success;
	}
	else
	{
		p_app.status_line = p_failure;
	}
}

static void ApplyLocalOverrides(AppState& p_app, std::vector<ChatSession>& p_nativeChats)
{
	const std::string l_selectedChatId = (SelectedChat(p_app) != nullptr) ? SelectedChat(p_app)->id : "";
	p_nativeChats = DeduplicateChatsById(std::move(p_nativeChats));
	std::vector<ChatSession> l_localChats = LoadChats(p_app);

	for (ChatSession& l_local : l_localChats)
	{
		if (l_local.uses_native_session || !l_local.native_session_id.empty() || !IsLocalDraftChatId(l_local.id))
		{
			continue;
		}

		const std::string l_normalizedProviderId = MapLegacyProviderId(l_local.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesGeminiHistory = Trim(l_local.provider_id).empty() || (lcp_localProvider != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider));

		if (!l_localChatUsesGeminiHistory)
		{
			continue;
		}

		const auto l_inferredSessionId = InferNativeSessionIdForLocalDraft(l_local, p_nativeChats);

		if (l_inferredSessionId.has_value())
		{
			PersistLocalDraftNativeSessionLink(p_app, l_local, l_inferredSessionId.value());
		}
	}

	l_localChats = DeduplicateChatsById(std::move(l_localChats));
	std::unordered_map<std::string, const ChatSession*> lcp_localMap;

	for (const ChatSession& l_local : l_localChats)
	{
		lcp_localMap[l_local.id] = &l_local;
	}

	std::unordered_set<std::string> l_nativeIds;

	for (ChatSession& l_native : p_nativeChats)
	{
		l_nativeIds.insert(l_native.id);
		const auto l_it = lcp_localMap.find(l_native.id);

		if (l_it == lcp_localMap.end())
		{
			continue;
		}

		const ChatSession& l_local = *l_it->second;

		if (!Trim(l_local.title).empty())
		{
			l_native.title = l_local.title;
		}

		if (!l_local.linked_files.empty())
		{
			l_native.linked_files = l_local.linked_files;
		}

		if (!l_local.provider_id.empty())
		{
			l_native.provider_id = l_local.provider_id;
		}

		if (!l_local.template_override_id.empty())
		{
			l_native.template_override_id = l_local.template_override_id;
		}

		l_native.rag_enabled = l_local.rag_enabled;
		l_native.rag_source_directories = l_local.rag_source_directories;

		if (!l_local.parent_chat_id.empty())
		{
			l_native.parent_chat_id = l_local.parent_chat_id;
		}

		if (!l_local.branch_root_chat_id.empty())
		{
			l_native.branch_root_chat_id = l_local.branch_root_chat_id;
		}

		if (l_local.branch_from_message_index >= 0)
		{
			l_native.branch_from_message_index = l_local.branch_from_message_index;
		}

		if (!l_local.native_session_id.empty())
		{
			l_native.native_session_id = l_local.native_session_id;
			l_native.uses_native_session = true;
		}
		else if (l_local.uses_native_session && l_native.native_session_id.empty())
		{
			l_native.native_session_id = l_native.id;
			l_native.uses_native_session = true;
		}

		if (l_local.gemini_md_bootstrapped)
		{
			l_native.gemini_md_bootstrapped = true;
		}

		if (!l_local.folder_id.empty())
		{
			l_native.folder_id = l_local.folder_id;
		}

		if (l_native.created_at.empty() && !l_local.created_at.empty())
		{
			l_native.created_at = l_local.created_at;
		}

		if (l_native.updated_at.empty() && !l_local.updated_at.empty())
		{
			l_native.updated_at = l_local.updated_at;
		}

		const bool l_localMessagesAreNewer = !l_local.messages.empty() && (l_local.messages.size() > l_native.messages.size() || (l_local.messages.size() == l_native.messages.size() && l_local.updated_at > l_native.updated_at));

		if (l_localMessagesAreNewer)
		{
			// Keep optimistic local history visible until native provider
			// history catches up.
			l_native.messages = l_local.messages;

			if (!l_local.updated_at.empty())
			{
				l_native.updated_at = l_local.updated_at;
			}

			if (l_native.created_at.empty() && !l_local.created_at.empty())
			{
				l_native.created_at = l_local.created_at;
			}
		}
	}

	std::vector<ChatSession> l_merged = p_nativeChats;

	for (const ChatSession& l_chat : l_localChats)
	{
		if (l_nativeIds.find(l_chat.id) != l_nativeIds.end())
		{
			continue;
		}

		if (l_chat.uses_native_session || !l_chat.native_session_id.empty())
		{
			continue;
		}

		const std::string l_normalizedProviderId = MapLegacyProviderId(l_chat.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesGeminiHistory = (lcp_localProvider == nullptr) ? true : ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider);
		// In Gemini-history mode, only explicit in-app drafts (chat-*) should
		// appear as local-only chats.
		if (l_localChatUsesGeminiHistory && !IsLocalDraftChatId(l_chat.id) && !Trim(l_chat.provider_id).empty())
		{
			continue;
		}

		bool l_hasRunningTerminal = false;

		for (const auto& l_terminal : p_app.cli_terminals)
		{
			if (l_terminal != nullptr && l_terminal->attached_chat_id == l_chat.id && l_terminal->running)
			{
				l_hasRunningTerminal = true;
				break;
			}
		}

		if (l_chat.messages.empty() && !HasPendingCallForChat(p_app, l_chat.id) && !l_hasRunningTerminal && l_chat.id != l_selectedChatId)
		{
			continue;
		}

		l_merged.push_back(l_chat);
	}

	p_app.chats = DeduplicateChatsById(std::move(l_merged));
	NormalizeChatBranchMetadata(p_app);
	NormalizeChatFolderAssignments(p_app);
}

static void RefreshGeminiChatsDir(AppState& p_app)
{
	const auto l_tmpDir = ResolveGeminiProjectTmpDir(fs::current_path());

	if (l_tmpDir.has_value())
	{
		p_app.gemini_chats_dir = l_tmpDir.value() / "chats";
		std::error_code l_ec;
		fs::create_directories(p_app.gemini_chats_dir, l_ec);
	}
	else
	{
		p_app.gemini_chats_dir.clear();
	}
}

static std::vector<std::string> CollectNewSessionIds(const std::vector<ChatSession>& p_loadedChats, const std::vector<std::string>& p_existingIds)
{
	std::unordered_set<std::string> l_seen(p_existingIds.begin(), p_existingIds.end());
	std::vector<std::string> l_discovered;

	for (const ChatSession& l_chat : p_loadedChats)
	{
		if (!l_chat.native_session_id.empty() && l_seen.find(l_chat.native_session_id) == l_seen.end())
		{
			l_discovered.push_back(l_chat.native_session_id);
		}
	}

	return l_discovered;
}

static std::string PickFirstUnblockedSessionId(const std::vector<std::string>& p_candidateIds, const std::unordered_set<std::string>& p_blockedIds)
{
	for (const std::string& l_candidate : p_candidateIds)
	{
		if (!l_candidate.empty() && p_blockedIds.find(l_candidate) == p_blockedIds.end())
		{
			return l_candidate;
		}
	}

	return "";
}

static bool SessionIdExistsInLoadedChats(const std::vector<ChatSession>& p_loadedChats, const std::string& p_sessionId)
{
	if (p_sessionId.empty())
	{
		return false;
	}

	for (const ChatSession& l_chat : p_loadedChats)
	{
		if (l_chat.uses_native_session && l_chat.native_session_id == p_sessionId)
		{
			return true;
		}
	}

	return false;
}

static std::optional<fs::path> FindNativeSessionFilePathInDirectory(const fs::path& p_chatsDir, const std::string& p_sessionId)
{
	if (p_sessionId.empty() || p_chatsDir.empty() || !fs::exists(p_chatsDir))
	{
		return std::nullopt;
	}

	std::error_code l_ec;

	for (const auto& l_item : fs::directory_iterator(p_chatsDir, l_ec))
	{
		if (l_ec || !l_item.is_regular_file() || l_item.path().extension() != ".json")
		{
			continue;
		}

		const std::string l_fileText = ReadTextFile(l_item.path());

		if (l_fileText.empty())
		{
			continue;
		}

		const std::optional<JsonValue> l_rootOpt = ParseJson(l_fileText);

		if (!l_rootOpt.has_value() || l_rootOpt->type != JsonValue::Type::Object)
		{
			continue;
		}

		if (JsonStringOrEmpty(l_rootOpt->Find("sessionId")) == p_sessionId)
		{
			return l_item.path();
		}
	}

	return std::nullopt;
}

static std::optional<fs::path> FindNativeSessionFilePath(const AppState& p_app, const std::string& p_sessionId)
{
	return FindNativeSessionFilePathInDirectory(p_app.gemini_chats_dir, p_sessionId);
}

static MessageRole NativeMessageRoleFromType(const ProviderProfile& p_provider, const std::string& p_type)
{
	return ProviderRuntime::RoleFromNativeType(p_provider, p_type);
}

static bool TruncateNativeSessionFromDisplayedMessage(const AppState& p_app, const ChatSession& p_chat, const int p_displayedMessageIndex, std::string* p_errorOut)
{
	if (!p_chat.uses_native_session || p_chat.native_session_id.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Chat is not linked to a native Gemini session.";
		}

		return false;
	}

	const auto l_sessionFile = FindNativeSessionFilePath(p_app, p_chat.native_session_id);

	if (!l_sessionFile.has_value())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Could not locate native Gemini session file.";
		}

		return false;
	}

	const std::string l_fileText = ReadTextFile(l_sessionFile.value());

	if (l_fileText.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native Gemini session file is empty.";
		}

		return false;
	}

	const std::optional<JsonValue> l_rootOpt = ParseJson(l_fileText);

	if (!l_rootOpt.has_value() || l_rootOpt->type != JsonValue::Type::Object)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to parse native Gemini session JSON.";
		}

		return false;
	}

	JsonValue l_root = l_rootOpt.value();

	auto l_messagesIt = l_root.object_value.find("messages");

	if (l_messagesIt == l_root.object_value.end() || l_messagesIt->second.type != JsonValue::Type::Array)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native Gemini session has no messages array.";
		}

		return false;
	}

	JsonValue& l_messagesArray = l_messagesIt->second;
	const ProviderProfile& l_provider = ActiveProviderOrDefault(p_app);

	std::vector<int> l_visibleRawIndices;
	std::vector<MessageRole> l_visibleRoles;
	l_visibleRawIndices.reserve(l_messagesArray.array_value.size());
	l_visibleRoles.reserve(l_messagesArray.array_value.size());

	for (int l_i = 0; l_i < static_cast<int>(l_messagesArray.array_value.size()); ++l_i)
	{
		const JsonValue& l_rawMessage = l_messagesArray.array_value[l_i];

		if (l_rawMessage.type != JsonValue::Type::Object)
		{
			continue;
		}

		const std::string l_content = Trim(ExtractGeminiContentText(l_rawMessage.Find("content")));

		if (l_content.empty())
		{
			continue;
		}

		l_visibleRawIndices.push_back(l_i);
		l_visibleRoles.push_back(NativeMessageRoleFromType(l_provider, JsonStringOrEmpty(l_rawMessage.Find("type"))));
	}

	if (p_displayedMessageIndex < 0 || p_displayedMessageIndex >= static_cast<int>(l_visibleRawIndices.size()))
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Selected message no longer matches native session timeline.";
		}

		return false;
	}

	if (l_visibleRoles[p_displayedMessageIndex] != MessageRole::User)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Only user messages can be edited.";
		}

		return false;
	}

	const int l_rawCutIndex = l_visibleRawIndices[p_displayedMessageIndex];
	l_messagesArray.array_value.erase(l_messagesArray.array_value.begin() + l_rawCutIndex, l_messagesArray.array_value.end());

	if (!WriteTextFile(l_sessionFile.value(), SerializeJson(l_root)))
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to write updated native Gemini session.";
		}

		return false;
	}

	return true;
}

static fs::path ResolveWindowIconPath()
{
	std::vector<fs::path> l_candidates;
	std::unique_ptr<char, decltype(&SDL_free)> lp_basePath(SDL_GetBasePath(), SDL_free);

	if (lp_basePath != nullptr)
	{
		l_candidates.emplace_back(fs::path(lp_basePath.get()) / "app_icon.bmp");
	}

	l_candidates.emplace_back("app_icon.bmp");
	l_candidates.emplace_back(fs::path("assets") / "app_icon.bmp");

	for (const fs::path& l_candidate : l_candidates)
	{
		std::error_code l_ec;

		if (fs::exists(l_candidate, l_ec) && !l_ec)
		{
			return l_candidate;
		}
	}

	return {};
}

static void ApplyWindowIcon(SDL_Window* p_window)
{
	if (p_window == nullptr)
	{
		return;
	}

	const fs::path l_iconPath = ResolveWindowIconPath();

	if (l_iconPath.empty())
	{
		return;
	}

	const std::string l_iconUtf8 = l_iconPath.string();
	std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> lp_iconSurface(SDL_LoadBMP(l_iconUtf8.c_str()), SDL_FreeSurface);

	if (lp_iconSurface == nullptr)
	{
		std::fprintf(stderr, "Warning: could not load window icon '%s': %s\n", l_iconUtf8.c_str(), SDL_GetError());
		return;
	}

	SDL_SetWindowIcon(p_window, lp_iconSurface.get());
}

static int RunLegacyApplicationMain()
{
	AppState l_app;
	PlatformServices& l_platformServices = PlatformServicesFactory::Instance();
	const CURLcode l_curlInitCode = curl_global_init(CURL_GLOBAL_DEFAULT);

	if (l_curlInitCode != CURLE_OK)
	{
		std::fprintf(stderr, "Failed to initialize libcurl: %s\n", curl_easy_strerror(l_curlInitCode));
		return 1;
	}

	struct CurlGlobalCleanupGuard
	{
		~CurlGlobalCleanupGuard()
		{
			curl_global_cleanup();
		}

	} l_curlCleanupGuard;
	std::vector<fs::path> l_dataRootCandidates;

	if (const char* lcp_dataDirEnv = std::getenv("UAM_DATA_DIR"))
	{
		const std::string l_envRoot = Trim(lcp_dataDirEnv);

		if (!l_envRoot.empty())
		{
			l_dataRootCandidates.push_back(fs::path(l_envRoot));
		}
	}

	if (l_dataRootCandidates.empty())
	{
		std::error_code l_cwdEc;
		const fs::path l_cwd = fs::current_path(l_cwdEc);

		if (!l_cwdEc)
		{
			l_dataRootCandidates.push_back(l_cwd / "data");
		}

		l_dataRootCandidates.push_back(l_platformServices.path_service.DefaultDataRootPath());
	}

	l_dataRootCandidates.push_back(TempFallbackDataRootPath());

	std::unordered_set<std::string> l_triedRoots;
	std::string l_lastDataRootError = "Unknown data directory initialization failure.";
	bool l_initializedDataRoot = false;

	for (const fs::path& l_candidateRoot : l_dataRootCandidates)
	{
		if (l_candidateRoot.empty())
		{
			continue;
		}

		const std::string l_key = l_candidateRoot.lexically_normal().string();

		if (l_triedRoots.find(l_key) != l_triedRoots.end())
		{
			continue;
		}

		l_triedRoots.insert(l_key);

		std::string l_error;

		if (EnsureDataRootLayout(l_candidateRoot, &l_error))
		{
			l_app.data_root = l_candidateRoot;
			l_initializedDataRoot = true;
			break;
		}

		l_lastDataRootError = std::move(l_error);
	}

	if (!l_initializedDataRoot)
	{
		std::fprintf(stderr, "Failed to initialize application data directories: %s\n", l_lastDataRootError.c_str());
		return 1;
	}

	LoadSettings(l_app);
	bool l_settingsDirty = false;
	const bool l_hadProviderFile = fs::exists(ProviderProfileFilePath(l_app));
	l_app.provider_profiles = ProviderProfileStore::Load(l_app.data_root);
	bool l_providersDirty = MigrateProviderProfilesToFixedModeIds(l_app);

	if (MigrateActiveProviderIdToFixedModes(l_app))
	{
		l_settingsDirty = true;
	}

	if (ActiveProvider(l_app) == nullptr && !l_app.provider_profiles.empty())
	{
		l_app.settings.active_provider_id = l_app.provider_profiles.front().id;
		l_settingsDirty = true;
	}

	if (ProviderProfile* lp_activeProfile = ActiveProvider(l_app); lp_activeProfile != nullptr)
	{
		if (!l_hadProviderFile && !l_app.settings.provider_command_template.empty() && IsGeminiProviderId(lp_activeProfile->id) && ProviderRuntime::UsesStructuredOutput(*lp_activeProfile))
		{
			lp_activeProfile->command_template = l_app.settings.provider_command_template;
			l_providersDirty = true;
		}

		l_app.settings.provider_command_template = lp_activeProfile->command_template;
		l_app.settings.gemini_command_template = l_app.settings.provider_command_template;
		l_app.settings.runtime_backend = ProviderRuntime::UsesInternalEngine(*lp_activeProfile) ? "ollama-engine" : "provider-cli";

		if (!ProviderRuntime::IsRuntimeEnabled(*lp_activeProfile))
		{
			const std::string l_disabledReason = ProviderRuntime::DisabledReason(*lp_activeProfile);
			l_app.status_line = l_disabledReason.empty() ? "Active provider runtime is disabled in this build." : l_disabledReason;
		}
	}

	if (!l_hadProviderFile || l_providersDirty)
	{
		SaveProviders(l_app);
	}

	LoadFrontendActions(l_app);
	RefreshTemplateCatalog(l_app, true);
	l_app.folders = ChatFolderStore::Load(l_app.data_root);
	EnsureDefaultFolder(l_app);
	SaveFolders(l_app);
	const ProviderProfile& l_activeProvider = ActiveProviderOrDefault(l_app);
	const ProviderRuntimeHistoryLoadOptions l_historyOptions = RuntimeHistoryLoadOptions();

	if (ActiveProviderUsesGeminiHistory(l_app))
	{
		RefreshGeminiChatsDir(l_app);
		l_app.chats = DeduplicateChatsById(ProviderRuntime::LoadHistory(l_activeProvider, l_app.data_root, l_app.gemini_chats_dir, l_historyOptions));
		ApplyLocalOverrides(l_app, l_app.chats);
		NormalizeChatBranchMetadata(l_app);
		NormalizeChatFolderAssignments(l_app);

		if (l_app.gemini_chats_dir.empty())
		{
			l_app.status_line = "Gemini native session directory not found "
			                    "yet. Run Gemini CLI in this project once.";
		}
	}
	else
	{
		l_app.chats = DeduplicateChatsById(ProviderRuntime::LoadHistory(l_activeProvider, l_app.data_root, l_app.gemini_chats_dir, l_historyOptions));
		NormalizeChatBranchMetadata(l_app);
		NormalizeChatFolderAssignments(l_app);
	}

	if (MigrateChatProviderBindingsToFixedModes(l_app))
	{
		l_settingsDirty = true;
	}

	if (!l_app.chats.empty())
	{
		if (l_app.settings.remember_last_chat && !l_app.settings.last_selected_chat_id.empty())
		{
			l_app.selected_chat_index = FindChatIndexById(l_app, l_app.settings.last_selected_chat_id);
		}

		if (l_app.selected_chat_index < 0 || l_app.selected_chat_index >= static_cast<int>(l_app.chats.size()))
		{
			l_app.selected_chat_index = 0;
		}

		RefreshRememberedSelection(l_app);
	}

	if (const ChatSession* lcp_selectedChat = SelectedChat(l_app); lcp_selectedChat != nullptr && ChatUsesCliOutput(l_app, *lcp_selectedChat))
	{
		MarkSelectedCliTerminalForLaunch(l_app);
	}

	if (l_settingsDirty)
	{
		SaveSettings(l_app);
	}

	l_platformServices.ui_traits.ApplyProcessDpiAwareness();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	l_platformServices.ui_traits.ConfigureOpenGlAttributes();
	const char* lcp_glslVersion = l_platformServices.ui_traits.OpenGlGlslVersion();

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> lp_window(SDL_CreateWindow("Universal Agent Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, l_app.settings.window_width, l_app.settings.window_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI), SDL_DestroyWindow);

	if (lp_window == nullptr)
	{
		std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	ApplyWindowIcon(lp_window.get());

	SDL_GLContext l_glContext = SDL_GL_CreateContext(lp_window.get());

	if (l_glContext == nullptr)
	{
		std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	SDL_GL_MakeCurrent(lp_window.get(), l_glContext);
	SDL_GL_SetSwapInterval(1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& l_io = ImGui::GetIO();
	l_io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	const float l_platformUiScale = DetectUiScale(lp_window.get());
	g_platform_layout_scale = std::clamp(l_platformUiScale, 1.0f, 2.25f);
	ConfigureFonts(l_io, l_platformUiScale);
	ApplyThemeFromSettings(l_app);

	if (l_platformUiScale > 1.01f)
	{
		ImGui::GetStyle().ScaleAllSizes(l_platformUiScale);
	}

	CaptureUiScaleBaseStyle();
	ApplyUserUiScale(l_io, l_app.settings.ui_scale_multiplier);

	ImGui_ImplSDL2_InitForOpenGL(lp_window.get(), l_glContext);
	ImGui_ImplOpenGL3_Init(lcp_glslVersion);

	if (l_app.settings.window_maximized)
	{
		SDL_MaximizeWindow(lp_window.get());
	}

	bool l_done = false;
	bool l_terminalsStoppedForShutdown = false;

	while (!l_done)
	{
		SDL_Event l_event;

		while (SDL_PollEvent(&l_event))
		{
			if (ForwardEscapeToSelectedCliTerminal(l_app, l_event))
			{
				continue;
			}

			ImGui_ImplSDL2_ProcessEvent(&l_event);

			if (l_event.type == SDL_QUIT)
			{
				l_done = true;
			}

			if (l_event.type == SDL_WINDOWEVENT && l_event.window.event == SDL_WINDOWEVENT_CLOSE && l_event.window.windowID == SDL_GetWindowID(lp_window.get()))
			{
				l_done = true;
			}

			if (l_event.type == SDL_WINDOWEVENT && l_event.window.windowID == SDL_GetWindowID(lp_window.get()))
			{
				const Uint8 l_windowEvent = l_event.window.event;

				if (l_windowEvent == SDL_WINDOWEVENT_SIZE_CHANGED || l_windowEvent == SDL_WINDOWEVENT_RESIZED || l_windowEvent == SDL_WINDOWEVENT_MAXIMIZED || l_windowEvent == SDL_WINDOWEVENT_RESTORED)
				{
					CaptureWindowState(l_app, lp_window.get());
					SaveSettings(l_app);
				}
			}
		}

		if (l_done)
		{
			// Exit guard: stop all CLI terminals using a non-blocking fast path
			// so window close cannot hang behind child process teardown.
			FastStopCliTerminalsForExit(l_app);
			l_terminalsStoppedForShutdown = true;
			break;
		}

		PollPendingGeminiCall(l_app);
		PollAllCliTerminals(l_app);
		PollGeminiCompatibilityTasks(l_app);

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();
		ApplyUserUiScale(l_io, l_app.settings.ui_scale_multiplier);
		PollRagScanState(l_app);

		HandleGlobalShortcuts(l_app);
		DrawDesktopMenuBar(l_app, l_done);

		const ImGuiViewport* lcp_viewport = ImGui::GetMainViewport();
		DrawAmbientBackdrop(lcp_viewport->Pos, lcp_viewport->Size, static_cast<float>(ImGui::GetTime()));

		ImGui::SetNextWindowPos(ImVec2(lcp_viewport->WorkPos.x, lcp_viewport->WorkPos.y));
		ImGui::SetNextWindowSize(ImVec2(lcp_viewport->WorkSize.x, lcp_viewport->WorkSize.y));

		ImGuiWindowFlags l_windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

		ImGui::Begin("Universal Agent Manager", nullptr, l_windowFlags);

		const float l_layoutW = ImGui::GetContentRegionAvail().x;
		float l_sidebarW = std::clamp(l_layoutW * 0.25f, 250.0f, 360.0f);

		if (l_layoutW < 1020.0f)
		{
			l_sidebarW = std::clamp(l_layoutW * 0.30f, 230.0f, 320.0f);
		}

		l_sidebarW = l_platformServices.ui_traits.AdjustSidebarWidth(l_layoutW, l_sidebarW, EffectiveUiScale());

		if (ImGui::BeginTable("layout_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBody))
		{
			ImGui::TableSetupColumn("Chats", ImGuiTableColumnFlags_WidthFixed, l_sidebarW);
			ImGui::TableSetupColumn("Conversation", ImGuiTableColumnFlags_WidthStretch, 0.72f);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			DrawLeftPane(l_app);

			ChatSession* lp_selected = SelectedChat(l_app);
			ImGui::TableSetColumnIndex(1);

			if (lp_selected == nullptr)
			{
				BeginPanel("empty_main", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, 0, ImVec2(ui::kSpace24, ui::kSpace24));
				ImGui::TextWrapped("No chat selected. Create one from the left panel.");
				EndPanel();
			}
			else
			{
				DrawChatDetailPane(l_app, *lp_selected);
			}

			ImGui::EndTable();
		}

		ImGui::End();
		DrawAboutModal(l_app);
		DrawDeleteChatConfirmationModal(l_app);
		DrawDeleteFolderConfirmationModal(l_app);
		DrawFolderSettingsModal(l_app);
		DrawTemplateChangeWarningModal(l_app);
		DrawTemplateManagerModal(l_app);
		DrawVcsOutputModal(l_app);
		DrawRuntimeModelSelectionModal(l_app);
		DrawRagConsoleModal(l_app);
		DrawAppSettingsModal(l_app, l_platformUiScale);
		ConsumePendingBranchRequest(l_app);

		ImGui::Render();
		int l_displayW = 0;
		int l_displayH = 0;
		SDL_GL_GetDrawableSize(lp_window.get(), &l_displayW, &l_displayH);
		glViewport(0, 0, l_displayW, l_displayH);
		glClearColor(ui::kMainBackground.x, ui::kMainBackground.y, ui::kMainBackground.z, ui::kMainBackground.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SDL_GL_SwapWindow(lp_window.get());
	}

	CaptureWindowState(l_app, lp_window.get());
	SaveSettings(l_app);

	l_app.pending_calls.clear();
	l_app.resolved_native_sessions_by_chat_id.clear();

	if (!l_terminalsStoppedForShutdown)
	{
		StopAllCliTerminals(l_app, true);
	}

	StopOpenCodeBridge(l_app);

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	SDL_GL_DeleteContext(l_glContext);
	SDL_Quit();

	return 0;
}

class Application
{
  public:
	int Initialize()
	{
		return 0;
	}

	int Run()
	{
		return RunLegacyApplicationMain();
	}

	bool RunFrame()
	{
		// Frame ownership remains in the legacy main loop during migration.
		return false;
	}

	void Shutdown()
	{
	}
};

int main(int, char**)
{
	Application application;
	const int init_rc = application.Initialize();

	if (init_rc != 0)
	{
		return init_rc;
	}

	const int run_rc = application.Run();
	application.Shutdown();
	return run_rc;
}
