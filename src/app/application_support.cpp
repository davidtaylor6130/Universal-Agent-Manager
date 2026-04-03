#include "application.h"
#include "application_core_helpers.h"
#include "chat_domain_service.h"
#include "native_session_link_service.h"
#include "persistence_coordinator.h"
#include "provider_resolution_service.h"
#include "provider_profile_migration_service.h"
#include "runtime_orchestration_services.h"
#include "runtime_local_service.h"
#include "template_runtime_service.h"

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
#include "common/markdown_template_catalog.h"
#include "common/provider_profile.h"
#include "common/provider_runtime.h"
#include "common/rag_index_service.h"
#include "common/runtime/local_engine_runtime_service.h"
#include "common/runtime/provider_cli_compatibility_service.h"
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
using uam::CliTerminalState;
using uam::kTerminalScrollbackMaxLines;
using uam::TerminalScrollbackLine;

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

namespace
{
	constexpr const char* kRuntimeBackendProviderCli = "provider-cli";
	constexpr const char* kRuntimeIdLocalEngine = "ollama-engine";
	constexpr const char* kPromptBootstrapPath = "@.gemini/gemini.md";
} // namespace

static void ClampWindowSettings(AppSettings& p_settings);
static std::string NormalizeThemeChoice(std::string p_value);
static void DrawSessionSidePane(AppState& p_app, ChatSession& p_chat);
static void SaveAndUpdateStatus(AppState& p_app, const ChatSession& p_chat, const std::string& p_success, const std::string& p_failure);
static std::string CompactPreview(const std::string& p_text, std::size_t p_maxLen);
static void MarkSelectedCliTerminalForLaunch(AppState& p_app);
static void SaveSettings(AppState& p_app);
static bool IsRuntimeEnabledForProvider(const ProviderProfile& p_provider, std::string* p_reasonOut = nullptr);
static bool SendPromptToCliRuntime(AppState& p_app, ChatSession& p_chat, const std::string& p_prompt, std::string* p_errorOut);
static bool HasPendingCallForChat(const AppState& p_app, const std::string& p_chatId);
static bool HasAnyPendingCall(const AppState& p_app);
static const PendingRuntimeCall* FirstPendingCallForChat(const AppState& p_app, const std::string& p_chatId);
static bool CreateBranchFromMessage(AppState& p_app, const std::string& p_sourceChatId, int p_messageIndex);
static void ConsumePendingBranchRequest(AppState& p_app);
static bool RefreshWorkspaceVcsSnapshot(AppState& p_app, const std::filesystem::path& p_workspaceRoot, bool p_force);
static void ShowVcsCommandOutput(AppState& p_app, const std::string& p_title, const VcsCommandResult& p_result);

static float ScaleUiLength(float p_value);
static ImVec2 ScaleUiSize(const ImVec2& p_value);
static void CaptureUiScaleBaseStyle();
static void ApplyUserUiScale(ImGuiIO& p_io, float p_userScaleMultiplier);

#include "common/runtime/json_runtime.h"

static ProviderRuntimeHistoryLoadOptions RuntimeHistoryLoadOptions();
static bool FrontendActionVisible(const AppState& p_app, const std::string& p_key, const bool p_fallbackVisible = true);
static std::string FrontendActionLabel(const AppState& p_app, const std::string& p_key, const std::string& p_fallbackLabel);
static void LoadSettings(AppState& p_app);
static std::optional<fs::path> FindNativeSessionFilePathInDirectory(const fs::path& p_chatsDir, const std::string& p_sessionId);
static fs::path ResolveWindowIconPath();
static void ApplyWindowIcon(SDL_Window* p_window);

#include "common/runtime/terminal_runtime.h"
#include "common/ui/ui_sections.h"

void ProviderRequestService::StartSelectedChatRequest(uam::AppState& p_app) const
{
	ChatSession* lp_chat = ChatDomainService().SelectedChat(p_app);

	if (lp_chat == nullptr)
	{
		p_app.status_line = "Select or create a chat first.";
		return;
	}

	const std::string l_promptText = Trim(p_app.composer_text);

	if (QueuePromptForChat(p_app, *lp_chat, l_promptText, false))
	{
		p_app.composer_text.clear();
	}
}

void ChatHistorySyncService::RefreshChatHistory(uam::AppState& p_app) const
{
	const ChatSession* lcp_selected = ChatDomainService().SelectedChat(p_app);
	const std::string l_selectedId = (lcp_selected != nullptr) ? lcp_selected->id : "";
	SyncChatsFromNative(p_app, l_selectedId, true);
	p_app.status_line = "Chat history refreshed.";
}

void ChatDetailView::Draw(uam::AppState& p_app, ChatSession* p_selectedChat) const
{
	if (p_selectedChat == nullptr)
	{
		BeginPanel("empty_main", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, 0, ImVec2(ui::kSpace24, ui::kSpace24));
		ImGui::TextWrapped("No chat selected. Create one from the left panel.");
		EndPanel();
		return;
	}

	DrawChatDetailPane(p_app, *p_selectedChat);
}

void ModalHostView::Draw(uam::AppState& p_app, const float p_platformUiScale) const
{
	DrawAboutModal(p_app);
	DrawDeleteChatConfirmationModal(p_app);
	DrawDeleteFolderConfirmationModal(p_app);
	DrawFolderSettingsModal(p_app);
	DrawTemplateChangeWarningModal(p_app);
	DrawMarkdownTemplateManagerModal(p_app);
	DrawVcsOutputModal(p_app);
	DrawRuntimeModelSelectionModal(p_app);
	DrawRagConsoleModal(p_app);
	DrawAppSettingsModal(p_app, p_platformUiScale);
	ConsumePendingBranchRequest(p_app);
}

void UiController::DrawFrame(uam::AppState& p_app,
                             bool& p_done,
                             const float p_platformUiScale,
                             const IPlatformUiTraits& p_uiTraits,
                             const ChatDetailView& p_chatDetail,
                             const ModalHostView& p_modalHost) const
{
	ImGuiIO& l_io = ImGui::GetIO();
	ApplyUserUiScale(l_io, p_app.settings.ui_scale_multiplier);
	PollRagScanState(p_app);

	HandleGlobalShortcuts(p_app);
	DrawDesktopMenuBar(p_app, p_done);

	const ImGuiViewport* lcp_viewport = ImGui::GetMainViewport();
	DrawAmbientBackdrop(lcp_viewport->Pos, lcp_viewport->Size, static_cast<float>(ImGui::GetTime()));

	ImGui::SetNextWindowPos(ImVec2(lcp_viewport->WorkPos.x, lcp_viewport->WorkPos.y));
	ImGui::SetNextWindowSize(ImVec2(lcp_viewport->WorkSize.x, lcp_viewport->WorkSize.y));

	const ImGuiWindowFlags l_windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
	ImGui::Begin("Universal Agent Manager", nullptr, l_windowFlags);

	const float l_layoutWidth = ImGui::GetContentRegionAvail().x;
	float l_sidebarWidth = std::clamp(l_layoutWidth * 0.25f, 250.0f, 360.0f);

	if (l_layoutWidth < 1020.0f)
	{
		l_sidebarWidth = std::clamp(l_layoutWidth * 0.30f, 230.0f, 320.0f);
	}

	l_sidebarWidth = p_uiTraits.AdjustSidebarWidth(l_layoutWidth, l_sidebarWidth, EffectiveUiScale());

	if (ImGui::BeginTable("layout_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBody))
	{
		ImGui::TableSetupColumn("Chats", ImGuiTableColumnFlags_WidthFixed, l_sidebarWidth);
		ImGui::TableSetupColumn("Conversation", ImGuiTableColumnFlags_WidthStretch, 0.72f);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		DrawLeftPane(p_app);

		ChatSession* lp_selectedChat = ChatDomainService().SelectedChat(p_app);
		ImGui::TableSetColumnIndex(1);
		p_chatDetail.Draw(p_app, lp_selectedChat);
		ImGui::EndTable();
	}

	ImGui::End();
	p_modalHost.Draw(p_app, p_platformUiScale);
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
	const int l_sourceIndex = ChatDomainService().FindChatIndexById(p_app, p_sourceChatId);

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

	ChatSession l_branch = ChatDomainService().CreateNewChat(l_source.folder_id, l_source.provider_id);
	l_branch.uses_native_session = false;
	l_branch.native_session_id.clear();
	l_branch.parent_chat_id = l_source.id;
	l_branch.branch_root_chat_id = l_source.branch_root_chat_id.empty() ? l_source.id : l_source.branch_root_chat_id;
	l_branch.branch_from_message_index = p_messageIndex;
	l_branch.template_override_id = l_source.template_override_id;
	l_branch.prompt_profile_bootstrapped = l_source.prompt_profile_bootstrapped;
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
	ChatBranching::Normalize(p_app.chats);
	ChatDomainService().SortChatsByRecent(p_app.chats);
	ChatDomainService().SelectChatById(p_app, l_branch.id);
	SaveSettings(p_app);

	if (ProviderResolutionService().ChatUsesCliOutput(p_app, p_app.chats[p_app.selected_chat_index]))
	{
		MarkSelectedCliTerminalForLaunch(p_app);
	}

	const ProviderProfile& l_branchProvider = ProviderResolutionService().ProviderForChatOrDefault(p_app, l_branch);

	if (!ProviderRuntime::SaveHistory(l_branchProvider, p_app.data_root, l_branch))
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

static ProviderRuntimeHistoryLoadOptions RuntimeHistoryLoadOptions()
{
	ProviderRuntimeHistoryLoadOptions options;
	options.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	options.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
	return options;
}

std::vector<ChatSession> ChatHistorySyncService::LoadNativeSessionChats(const fs::path& p_chatsDir, const ProviderProfile& p_provider) const
{
	return ChatDomainService().DeduplicateChatsById(ProviderRuntime::LoadHistory(p_provider, fs::path{}, p_chatsDir, RuntimeHistoryLoadOptions()));
}

bool ChatHistorySyncService::StartAsyncNativeChatLoad(AppState& app) const
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!ProviderResolutionService().ActiveProviderUsesNativeOverlayHistory(app) || app.native_chat_load_task.running)
	{
		return false;
	}

	RefreshNativeSessionDirectory(app);
	const fs::path chats_dir = app.native_history_chats_dir;
	const ProviderProfile provider = ProviderResolutionService().ActiveProviderOrDefault(app);

	app.native_chat_load_task.running = true;
	app.native_chat_load_task.completed = std::make_shared<std::atomic<bool>>(false);
	app.native_chat_load_task.chats = std::make_shared<std::vector<ChatSession>>();
	app.native_chat_load_task.error = std::make_shared<std::string>();
	std::shared_ptr<std::atomic<bool>> completed = app.native_chat_load_task.completed;
	std::shared_ptr<std::vector<ChatSession>> chats = app.native_chat_load_task.chats;
	std::shared_ptr<std::string> error = app.native_chat_load_task.error;

	auto l_loadNativeChatsTask = [this, chats_dir, provider, completed, chats, error]()
	{
		try
		{
			*chats = LoadNativeSessionChats(chats_dir, provider);
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

bool ChatHistorySyncService::TryConsumeAsyncNativeChatLoad(AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out) const
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

std::vector<std::string> ChatHistorySyncService::SessionIdsFromChats(const std::vector<ChatSession>& p_chats) const
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

static bool FrontendActionVisible(const AppState& p_app, const std::string& p_key, const bool p_fallbackVisible)
{
	const uam::FrontendAction* lcp_action = uam::FindAction(p_app.frontend_actions, p_key);
	return (lcp_action == nullptr) ? p_fallbackVisible : lcp_action->visible;
}

static std::string FrontendActionLabel(const AppState& p_app, const std::string& p_key, const std::string& p_fallbackLabel)
{
	const uam::FrontendAction* lcp_action = uam::FindAction(p_app.frontend_actions, p_key);

	if (lcp_action == nullptr || Trim(lcp_action->label).empty())
	{
		return p_fallbackLabel;
	}

	return lcp_action->label;
}

static void SaveSettings(AppState& p_app)
{
	p_app.settings.ui_theme = NormalizeThemeChoice(p_app.settings.ui_theme);
	p_app.settings.runtime_backend = ProviderResolutionService().ActiveProviderUsesInternalEngine(p_app) ? kRuntimeIdLocalEngine : kRuntimeBackendProviderCli;
#if UAM_ENABLE_ENGINE_RAG
	p_app.settings.vector_db_backend = (p_app.settings.vector_db_backend == "none") ? "none" : kRuntimeIdLocalEngine;
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

			if (!RuntimeLocalService().RestartLocalBridgeIfModelChanged(p_app, &l_bridgeError) && !l_bridgeError.empty())
			{
				p_app.status_line = l_bridgeError;
			}
		}
	}

	ChatDomainService().RefreshRememberedSelection(p_app);
	SettingsStore::Save(AppPaths::SettingsFilePath(p_app.data_root), p_app.settings, p_app.center_view_mode);
}

static void LoadSettings(AppState& p_app)
{
	SettingsStore::Load(AppPaths::SettingsFilePath(p_app.data_root), p_app.settings, p_app.center_view_mode);
#if UAM_ENABLE_ENGINE_RAG
	p_app.settings.vector_db_backend = (p_app.settings.vector_db_backend == "none") ? "none" : kRuntimeIdLocalEngine;
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

bool ChatHistorySyncService::PersistLocalDraftNativeSessionLink(const AppState& p_app, ChatSession& p_localChat, const std::string& p_nativeSessionId) const
{
	const std::string l_sessionId = Trim(p_nativeSessionId);

	if (l_sessionId.empty() || !NativeSessionLinkService().IsLocalDraftChatId(p_localChat.id))
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

	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_localChat);
	return ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_localChat);
}

std::string ChatHistorySyncService::ResolveResumeSessionIdForChat(const AppState& p_app, const ChatSession& p_chat) const
{
	if (!ProviderResolutionService().ChatUsesNativeOverlayHistory(p_app, p_chat))
	{
		return "";
	}

	const auto l_sessionExists = [&](const std::string& p_sessionId)
	{
		if (p_sessionId.empty() || p_app.native_history_chats_dir.empty())
		{
			return false;
		}

		std::error_code l_ec;
		return fs::exists(p_app.native_history_chats_dir / (p_sessionId + ".json"), l_ec) && !l_ec;
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
	if (!p_chat.messages.empty() && !p_chat.id.empty() && !NativeSessionLinkService().IsLocalDraftChatId(p_chat.id))
	{
		return l_sessionExists(p_chat.id) ? p_chat.id : "";
	}

	return "";
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


TemplatePreflightOutcome ProviderRequestService::PreflightWorkspaceTemplateForChat(AppState& p_app, const ProviderProfile& p_provider, const ChatSession& p_chat, std::string* p_bootstrapPromptOut, std::string* p_statusOut) const
{
	TemplateRuntimeService().RefreshTemplateCatalog(p_app);

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

	const TemplateCatalogEntry* lcp_entry = TemplateRuntimeService().FindTemplateEntryById(p_app, l_effectiveTemplateId);

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
		if (!TemplateRuntimeService().EnsureWorkspaceProviderLayout(p_app, p_chat, p_statusOut))
		{
			return TemplatePreflightOutcome::BlockingError;
		}

		std::error_code l_ec;
		fs::copy_file(lcp_entry->absolute_path, WorkspacePromptProfileTemplatePath(p_app, p_chat), fs::copy_options::overwrite_existing, l_ec);

		if (l_ec)
		{
			if (p_statusOut != nullptr)
			{
				*p_statusOut = "Failed to materialize workspace prompt profile file: " + l_ec.message();
			}

			return TemplatePreflightOutcome::BlockingError;
		}

		if (p_bootstrapPromptOut != nullptr)
		{
			*p_bootstrapPromptOut = p_provider.prompt_bootstrap_path.empty() ? kPromptBootstrapPath : p_provider.prompt_bootstrap_path;
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

bool ProviderRequestService::QueuePromptForChat(AppState& p_app, ChatSession& p_chat, const std::string& p_prompt, const bool p_templateControlMessage) const
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

	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);
	std::string l_runtimeDisabledReason;

	if (!IsRuntimeEnabledForProvider(l_provider, &l_runtimeDisabledReason))
	{
		p_app.status_line = l_runtimeDisabledReason;
		return false;
	}

	const bool l_useLocalRuntime = ProviderResolutionService().ChatUsesInternalEngine(p_app, p_chat);
	const bool l_useLocalBridgeRuntime = RuntimeLocalService().ProviderUsesLocalBridgeRuntime(l_provider);
	std::string l_templateStatus;
	std::string l_bootstrapPrompt;
	TemplatePreflightOutcome l_templateOutcome = TemplatePreflightOutcome::ReadyWithoutTemplate;

	if (!p_templateControlMessage || !p_chat.prompt_profile_bootstrapped)
	{
		l_templateOutcome = PreflightWorkspaceTemplateForChat(p_app, l_provider, p_chat, &l_bootstrapPrompt, &l_templateStatus);

		if (l_templateOutcome == TemplatePreflightOutcome::BlockingError)
		{
			p_app.status_line = l_templateStatus.empty() ? "Prompt profile preflight failed." : l_templateStatus;
			return false;
		}
	}

	const bool l_shouldBootstrapTemplate = !p_templateControlMessage && !p_chat.prompt_profile_bootstrapped && p_chat.messages.empty() && l_templateOutcome == TemplatePreflightOutcome::ReadyWithTemplate;
	std::string l_runtimePrompt = l_promptText;

	if (!p_templateControlMessage)
	{
		l_runtimePrompt = BuildRagEnhancedPrompt(p_app, p_chat, l_promptText);
	}

	if (l_shouldBootstrapTemplate && !l_bootstrapPrompt.empty())
	{
		l_runtimePrompt = l_bootstrapPrompt + "\n\n" + l_runtimePrompt;
	}

	if ((l_useLocalRuntime || l_useLocalBridgeRuntime) && !RuntimeLocalService().EnsureSelectedLocalRuntimeModelForProvider(p_app))
	{
		return false;
	}

	ChatDomainService().AddMessage(p_chat, MessageRole::User, l_promptText);
	SaveAndUpdateStatus(p_app, p_chat, "Prompt queued for provider runtime.", "Saved message locally, but failed to persist chat data.");

	const bool l_useSharedCliSession = !l_useLocalRuntime && ProviderRuntime::UsesCliOutput(l_provider) && l_provider.supports_interactive;

	if (!l_useLocalRuntime && ProviderRuntime::UsesCliOutput(l_provider) && !l_provider.supports_interactive)
	{
		ChatDomainService().AddMessage(p_chat, MessageRole::System, "Provider is configured for CLI output but has no interactive runtime command.");
		ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
		p_app.status_line = "Provider runtime configuration error.";
		return false;
	}

	if (l_useSharedCliSession)
	{
		std::string l_terminalError;

		if (!SendPromptToCliRuntime(p_app, p_chat, l_runtimePrompt, &l_terminalError))
		{
			ChatDomainService().AddMessage(p_chat, MessageRole::System, "Provider terminal send failed: " + (l_terminalError.empty() ? std::string("unknown error") : l_terminalError));
			ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
			p_app.status_line = "Provider terminal send failed.";
			return false;
		}

		if (l_shouldBootstrapTemplate)
		{
			p_chat.prompt_profile_bootstrapped = true;
			ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
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

		if (!RuntimeLocalService().EnsureLocalRuntimeModelLoaded(p_app, &l_loadError))
		{
			ChatDomainService().AddMessage(p_chat, MessageRole::System, "Local runtime model load failed: " + (l_loadError.empty() ? std::string("unknown error") : l_loadError));
			ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
			p_app.status_line = "Local runtime model load failed.";
			return false;
		}

		const LocalEngineResponse l_response = p_app.runtime_model_service.SendPrompt(ResolveRagModelFolder(p_app), l_runtimePrompt);

		if (l_response.ok)
		{
			ChatDomainService().AddMessage(p_chat, MessageRole::Assistant, l_response.text);
			SaveAndUpdateStatus(p_app, p_chat, "Local response generated.", "Local response generated, but chat save failed.");
			p_app.scroll_to_bottom = true;
			return true;
		}

		ChatDomainService().AddMessage(p_chat, MessageRole::System, "Local runtime error: " + l_response.error);
		ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
		p_app.status_line = "Local runtime command failed.";
		p_app.scroll_to_bottom = true;
		return false;
	}

	std::vector<ChatSession> l_nativeBefore;

	if (ProviderResolutionService().ChatUsesNativeOverlayHistory(p_app, p_chat))
	{
		ChatHistorySyncService().RefreshNativeSessionDirectory(p_app);
		l_nativeBefore = ChatHistorySyncService().LoadNativeSessionChats(p_app.native_history_chats_dir, l_provider);
	}

	const std::string l_resumeSessionId = ChatHistorySyncService().ResolveResumeSessionIdForChat(p_app, p_chat);
	const std::string l_providerPrompt = ProviderRuntime::BuildPrompt(l_provider, l_runtimePrompt, p_chat.linked_files);
	const std::string l_providerCommand = ProviderRuntime::BuildCommand(l_provider, p_app.settings, l_providerPrompt, p_chat.linked_files, l_resumeSessionId);
	const std::string l_command = PlatformServicesFactory::Instance().process_service.BuildShellCommandWithWorkingDirectory(ResolveWorkspaceRootPath(p_app, p_chat), l_providerCommand);
	const std::string l_chatId = p_chat.id;

	PendingRuntimeCall l_pending;
	l_pending.chat_id = l_chatId;
	l_pending.resume_session_id = l_resumeSessionId;
	l_pending.session_ids_before = ChatHistorySyncService().SessionIdsFromChats(l_nativeBefore);
	l_pending.command_preview = l_command;
	l_pending.completed = std::make_shared<std::atomic<bool>>(false);
	l_pending.output = std::make_shared<std::string>();
	{
		std::shared_ptr<std::atomic<bool>> l_completed = l_pending.completed;
		std::shared_ptr<std::string> l_output = l_pending.output;
		auto l_runPendingCallTask = [l_command, l_completed, l_output]()
		{
			*l_output = PersistenceCoordinator().ExecuteCommandCaptureOutput(l_command);
			l_completed->store(true, std::memory_order_release);
		};

		std::thread(l_runPendingCallTask).detach();
	}

	p_app.pending_calls.push_back(std::move(l_pending));

	if (l_shouldBootstrapTemplate)
	{
		p_chat.prompt_profile_bootstrapped = true;
		ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat);
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
	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);

	if (ProviderRuntime::SaveHistory(l_provider, p_app.data_root, p_chat))
	{
		p_app.status_line = p_success;
	}
	else
	{
		p_app.status_line = p_failure;
	}
}

void ChatHistorySyncService::ApplyLocalOverrides(AppState& p_app, std::vector<ChatSession>& p_nativeChats) const
{
	const std::string l_selectedChatId = (ChatDomainService().SelectedChat(p_app) != nullptr) ? ChatDomainService().SelectedChat(p_app)->id : "";
	p_nativeChats = ChatDomainService().DeduplicateChatsById(std::move(p_nativeChats));
	std::vector<ChatSession> l_localChats = ChatRepository::LoadLocalChats(p_app.data_root);

	for (ChatSession& l_local : l_localChats)
	{
		if (l_local.uses_native_session || !l_local.native_session_id.empty() || !NativeSessionLinkService().IsLocalDraftChatId(l_local.id))
		{
			continue;
		}

		const std::string l_normalizedProviderId = ProviderProfileMigrationService().MapLegacyRuntimeId(l_local.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesNativeOverlayHistory = Trim(l_local.provider_id).empty() || (lcp_localProvider != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider));

		if (!l_localChatUsesNativeOverlayHistory)
		{
			continue;
		}

		const auto l_inferredSessionId = NativeSessionLinkService().InferNativeSessionIdForLocalDraft(l_local, p_nativeChats);

		if (l_inferredSessionId.has_value())
		{
			PersistLocalDraftNativeSessionLink(p_app, l_local, l_inferredSessionId.value());
		}
	}

	l_localChats = ChatDomainService().DeduplicateChatsById(std::move(l_localChats));
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

		if (l_local.prompt_profile_bootstrapped)
		{
			l_native.prompt_profile_bootstrapped = true;
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

		const std::string l_normalizedProviderId = ProviderProfileMigrationService().MapLegacyRuntimeId(l_chat.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesNativeOverlayHistory = (lcp_localProvider == nullptr) ? true : ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider);
		// In native-overlay history mode, only explicit in-app drafts (chat-*) should
		// appear as local-only chats.
		if (l_localChatUsesNativeOverlayHistory && !NativeSessionLinkService().IsLocalDraftChatId(l_chat.id) && !Trim(l_chat.provider_id).empty())
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

	p_app.chats = ChatDomainService().DeduplicateChatsById(std::move(l_merged));
	ChatBranching::Normalize(p_app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(p_app);
}

void ChatHistorySyncService::RefreshNativeSessionDirectory(AppState& p_app) const
{
	const auto l_tmpDir = AppPaths::ResolveGeminiProjectTmpDir(fs::current_path());

	if (l_tmpDir.has_value())
	{
		p_app.native_history_chats_dir = l_tmpDir.value() / "chats";
		std::error_code l_ec;
		fs::create_directories(p_app.native_history_chats_dir, l_ec);
	}
	else
	{
		p_app.native_history_chats_dir.clear();
	}
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

bool ChatHistorySyncService::TruncateNativeSessionFromDisplayedMessage(const AppState& p_app, const ChatSession& p_chat, const int p_displayedMessageIndex, std::string* p_errorOut) const
{
	if (!p_chat.uses_native_session || p_chat.native_session_id.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Chat is not linked to a native runtime session.";
		}

		return false;
	}

	const auto l_sessionFile = FindNativeSessionFilePathInDirectory(p_app.native_history_chats_dir, p_chat.native_session_id);

	if (!l_sessionFile.has_value())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Could not locate native runtime session file.";
		}

		return false;
	}

	const std::string l_fileText = ReadTextFile(l_sessionFile.value());

	if (l_fileText.empty())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native runtime session file is empty.";
		}

		return false;
	}

	const std::optional<JsonValue> l_rootOpt = ParseJson(l_fileText);

	if (!l_rootOpt.has_value() || l_rootOpt->type != JsonValue::Type::Object)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to parse native runtime session JSON.";
		}

		return false;
	}

	JsonValue l_root = l_rootOpt.value();

	auto l_messagesIt = l_root.object_value.find("messages");

	if (l_messagesIt == l_root.object_value.end() || l_messagesIt->second.type != JsonValue::Type::Array)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native runtime session has no messages array.";
		}

		return false;
	}

	JsonValue& l_messagesArray = l_messagesIt->second;
	const ProviderProfile& l_provider = ProviderResolutionService().ActiveProviderOrDefault(p_app);

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
		l_visibleRoles.push_back(ProviderRuntime::RoleFromNativeType(l_provider, JsonStringOrEmpty(l_rawMessage.Find("type"))));
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
			*p_errorOut = "Failed to write updated native runtime session.";
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

bool Application::InitializeState()
{
	const CURLcode l_curlInitCode = curl_global_init(CURL_GLOBAL_DEFAULT);

	if (l_curlInitCode != CURLE_OK)
	{
		std::fprintf(stderr, "Failed to initialize libcurl: %s\n", curl_easy_strerror(l_curlInitCode));
		m_exitCode = 1;
		return false;
	}

	m_curlInitialized = true;
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

		l_dataRootCandidates.push_back(m_platformServices->path_service.DefaultDataRootPath());
	}

	l_dataRootCandidates.push_back(PersistenceCoordinator().TempFallbackDataRootPath());

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

		if (PersistenceCoordinator().EnsureDataRootLayout(l_candidateRoot, &l_error))
		{
			m_app.data_root = l_candidateRoot;
			l_initializedDataRoot = true;
			break;
		}

		l_lastDataRootError = std::move(l_error);
	}

	if (!l_initializedDataRoot)
	{
		std::fprintf(stderr, "Failed to initialize application data directories: %s\n", l_lastDataRootError.c_str());
		m_exitCode = 1;
		return false;
	}

	LoadSettings(m_app);
	bool l_settingsDirty = false;
	const bool l_hadProviderFile = fs::exists(m_app.data_root / "providers.txt");
	m_app.provider_profiles = ProviderProfileStore::Load(m_app.data_root);
	bool l_providersDirty = ProviderProfileMigrationService().MigrateProviderProfilesToFixedModeIds(m_app);

	if (ProviderProfileMigrationService().MigrateActiveProviderIdToFixedModes(m_app))
	{
		l_settingsDirty = true;
	}

	if (ProviderResolutionService().ActiveProvider(m_app) == nullptr && !m_app.provider_profiles.empty())
	{
		m_app.settings.active_provider_id = m_app.provider_profiles.front().id;
		l_settingsDirty = true;
	}

	if (ProviderProfile* lp_activeProfile = ProviderResolutionService().ActiveProvider(m_app); lp_activeProfile != nullptr)
	{
		if (!l_hadProviderFile && !m_app.settings.provider_command_template.empty() && ProviderProfileMigrationService().IsNativeHistoryProviderId(lp_activeProfile->id) && ProviderRuntime::UsesStructuredOutput(*lp_activeProfile))
		{
			lp_activeProfile->command_template = m_app.settings.provider_command_template;
			l_providersDirty = true;
		}

		m_app.settings.provider_command_template = lp_activeProfile->command_template;
		m_app.settings.gemini_command_template = m_app.settings.provider_command_template;
		m_app.settings.runtime_backend = ProviderRuntime::UsesInternalEngine(*lp_activeProfile) ? kRuntimeIdLocalEngine : kRuntimeBackendProviderCli;

		if (!ProviderRuntime::IsRuntimeEnabled(*lp_activeProfile))
		{
			const std::string l_disabledReason = ProviderRuntime::DisabledReason(*lp_activeProfile);
			m_app.status_line = l_disabledReason.empty() ? "Active provider runtime is disabled in this build." : l_disabledReason;
		}
	}

	if (!l_hadProviderFile || l_providersDirty)
	{
		ProviderProfileStore::Save(m_app.data_root, m_app.provider_profiles);
	}

	PersistenceCoordinator().LoadFrontendActions(m_app);
	TemplateRuntimeService().RefreshTemplateCatalog(m_app, true);
	m_app.folders = ChatFolderStore::Load(m_app.data_root);
	ChatDomainService().EnsureDefaultFolder(m_app);
	ChatFolderStore::Save(m_app.data_root, m_app.folders);
	const ProviderProfile& l_activeProvider = ProviderResolutionService().ActiveProviderOrDefault(m_app);
	const ProviderRuntimeHistoryLoadOptions l_historyOptions = RuntimeHistoryLoadOptions();

	if (ProviderResolutionService().ActiveProviderUsesNativeOverlayHistory(m_app))
	{
		ChatHistorySyncService().RefreshNativeSessionDirectory(m_app);
		m_app.chats = ChatDomainService().DeduplicateChatsById(ProviderRuntime::LoadHistory(l_activeProvider, m_app.data_root, m_app.native_history_chats_dir, l_historyOptions));
		ChatHistorySyncService().ApplyLocalOverrides(m_app, m_app.chats);
		ChatBranching::Normalize(m_app.chats);
		ChatDomainService().NormalizeChatFolderAssignments(m_app);

		if (m_app.native_history_chats_dir.empty())
		{
			m_app.status_line = "Native session directory not found yet. Run the provider CLI in this project once.";
		}
	}
	else
	{
		m_app.chats = ChatDomainService().DeduplicateChatsById(ProviderRuntime::LoadHistory(l_activeProvider, m_app.data_root, m_app.native_history_chats_dir, l_historyOptions));
		ChatBranching::Normalize(m_app.chats);
		ChatDomainService().NormalizeChatFolderAssignments(m_app);
	}

	if (ProviderProfileMigrationService().MigrateChatProviderBindingsToFixedModes(m_app))
	{
		l_settingsDirty = true;
	}

	if (!m_app.chats.empty())
	{
		if (m_app.settings.remember_last_chat && !m_app.settings.last_selected_chat_id.empty())
		{
			m_app.selected_chat_index = ChatDomainService().FindChatIndexById(m_app, m_app.settings.last_selected_chat_id);
		}

		if (m_app.selected_chat_index < 0 || m_app.selected_chat_index >= static_cast<int>(m_app.chats.size()))
		{
			m_app.selected_chat_index = 0;
		}

		ChatDomainService().RefreshRememberedSelection(m_app);
	}

	if (const ChatSession* lcp_selectedChat = ChatDomainService().SelectedChat(m_app); lcp_selectedChat != nullptr && ProviderResolutionService().ChatUsesCliOutput(m_app, *lcp_selectedChat))
	{
		MarkSelectedCliTerminalForLaunch(m_app);
	}

	if (l_settingsDirty)
	{
		SaveSettings(m_app);
	}

	return true;
}

bool Application::InitializeWindowAndUi()
{
	m_platformServices->ui_traits.ApplyProcessDpiAwareness();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		m_exitCode = 1;
		return false;
	}

	m_sdlInitialized = true;
	m_platformServices->ui_traits.ConfigureOpenGlAttributes();
	m_glslVersion = m_platformServices->ui_traits.OpenGlGlslVersion();

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	m_window = SDL_CreateWindow("Universal Agent Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, m_app.settings.window_width, m_app.settings.window_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

	if (m_window == nullptr)
	{
		std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		m_exitCode = 1;
		return false;
	}

	ApplyWindowIcon(m_window);
	m_glContext = SDL_GL_CreateContext(m_window);

	if (m_glContext == nullptr)
	{
		std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		m_exitCode = 1;
		return false;
	}

	SDL_GL_MakeCurrent(m_window, m_glContext);
	SDL_GL_SetSwapInterval(1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	m_imguiInitialized = true;
	ImGuiIO& l_io = ImGui::GetIO();
	l_io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	m_platformUiScale = DetectUiScale(m_window);
	g_platform_layout_scale = std::clamp(m_platformUiScale, 1.0f, 2.25f);
	ConfigureFonts(l_io, m_platformUiScale);
	ApplyThemeFromSettings(m_app);

	if (m_platformUiScale > 1.01f)
	{
		ImGui::GetStyle().ScaleAllSizes(m_platformUiScale);
	}

	CaptureUiScaleBaseStyle();
	ApplyUserUiScale(l_io, m_app.settings.ui_scale_multiplier);

	ImGui_ImplSDL2_InitForOpenGL(m_window, m_glContext);
	ImGui_ImplOpenGL3_Init(m_glslVersion);

	if (m_app.settings.window_maximized)
	{
		SDL_MaximizeWindow(m_window);
	}

	return true;
}

void Application::PersistWindowStateAndSettings()
{
	if (m_window == nullptr || m_app.data_root.empty())
	{
		return;
	}

	CaptureWindowState(m_app, m_window);
	SaveSettings(m_app);
}

void Application::PresentFrame()
{
	if (m_window == nullptr)
	{
		return;
	}

	int l_displayWidth = 0;
	int l_displayHeight = 0;
	SDL_GL_GetDrawableSize(m_window, &l_displayWidth, &l_displayHeight);
	glViewport(0, 0, l_displayWidth, l_displayHeight);
	glClearColor(ui::kMainBackground.x, ui::kMainBackground.y, ui::kMainBackground.z, ui::kMainBackground.w);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	SDL_GL_SwapWindow(m_window);
}
