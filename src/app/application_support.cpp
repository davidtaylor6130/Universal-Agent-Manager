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
#include "common/runtime/terminal_common.h"
#include "common/runtime/local_engine_runtime_service.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/settings_store.h"
#include "common/vcs_workspace_service.h"
#include "common/platform/platform_services.h"
#include "common/ui/modals/modal_window_state.h"
#include "common/ui/ui_sections.h"
#include "common/ui/theme/theme_apply.h"
#include "common/ui/theme/theme_fonts.h"
#include "common/ui/theme/theme_scaling.h"

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

using uam::constants::kAppCopyright;
using uam::constants::kAppDisplayName;
using uam::constants::kAppVersion;
using uam::constants::kDefaultFolderId;
using uam::constants::kDefaultFolderTitle;

namespace
{
	constexpr const char* kRuntimeBackendProviderCli = "provider-cli";
	constexpr const char* kRuntimeIdLocalEngine = "ollama-engine";
} // namespace

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
	ChatDomainService().ConsumePendingBranchRequest(p_app);
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
	DrawAmbientBackdrop(lcp_viewport->Pos, lcp_viewport->Size, float(ImGui::GetTime()));

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

	PersistenceCoordinator().LoadSettings(m_app);
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
	ProviderRuntimeHistoryLoadOptions l_historyOptions;
	l_historyOptions.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	l_historyOptions.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();

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

		if (m_app.selected_chat_index < 0 || m_app.selected_chat_index >= int(m_app.chats.size()))
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
		PersistenceCoordinator().SaveSettings(m_app);
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

	ApplyWindowIcon();
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
	PersistenceCoordinator().SaveSettings(m_app);
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

fs::path Application::ResolveWindowIconPath() const
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

void Application::ApplyWindowIcon() const
{
	if (m_window == nullptr)
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

	SDL_SetWindowIcon(m_window, lp_iconSurface.get());
}
