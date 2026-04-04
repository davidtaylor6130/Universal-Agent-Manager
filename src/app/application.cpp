#include "application.h"

#include "application_core_helpers.h"
#include "chat_domain_service.h"
#include "persistence_coordinator.h"
#include "provider_resolution_service.h"
#include "provider_profile_migration_service.h"
#include "runtime_orchestration_services.h"
#include "template_runtime_service.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <vterm.h>
#include <curl/curl.h>

#include "runtime_local_service.h"
#include "common/constants/app_constants.h"
#include "common/models/app_models.h"
#include "common/paths/app_paths.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/config/frontend_actions.h"
#include "common/provider/markdown_template_catalog.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/rag/rag_index_service.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/local_engine_runtime_service.h"
#include "common/runtime/terminal_polling.h"
#include "common/ui/chat_actions/chat_action_pending_calls.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/config/settings_store.h"
#include "common/vcs/vcs_workspace_service.h"
#include "common/platform/platform_services.h"
#include "common/ui/modals/modal_window_state.h"
#include "common/ui/theme/theme_apply.h"
#include "common/ui/theme/theme_fonts.h"
#include "common/ui/theme/theme_scaling.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#include "common/platform/sdl_includes.h"
#include "common/platform/gl_includes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#define UAM_TARGET_FPS 30

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kRuntimeBackendProviderCli = "provider-cli";
	constexpr const char* kRuntimeIdLocalEngine = "ollama-engine";

	void ResetAsyncCommandTask(uam::AsyncCommandTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}

		task.running = false;
		task.command_preview.clear();
		task.state.reset();
	}

	void ResetPendingRuntimeCall(PendingRuntimeCall& call)
	{
		if (call.worker != nullptr)
		{
			call.worker->request_stop();
			call.worker.reset();
		}

		call.state.reset();
	}

	void ResetNativeChatLoadTask(uam::platform::AsyncNativeChatLoadTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}

		task.running = false;
		task.provider_id_snapshot.clear();
		task.chats_dir_snapshot.clear();
		task.state.reset();
	}
} // namespace

Application::Application()
{
	// Constructor owns startup for the app lifetime.
	if (!OnLoad())
	{
		m_done = true;
		m_exitCode = (m_exitCode != 0) ? m_exitCode : 1;
	}
}

Application::~Application()
{
	// Destructor owns final app shutdown and low-level cleanup.
	Shutdown();

	if (m_imguiInitialized)
	{
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL2_Shutdown();
		ImGui::DestroyContext();
		m_imguiInitialized = false;
	}

	if (m_glContext != nullptr)
	{
		SDL_GL_DeleteContext(m_glContext);
		m_glContext = nullptr;
	}

	if (m_window != nullptr)
	{
		SDL_DestroyWindow(m_window);
		m_window = nullptr;
	}

	if (m_sdlInitialized)
	{
		SDL_Quit();
		m_sdlInitialized = false;
	}

	if (m_curlInitialized)
	{
		curl_global_cleanup();
		m_curlInitialized = false;
	}

	m_platformServices = nullptr;
	m_glslVersion = nullptr;
	m_platformUiScale = 1.0f;
	m_done = true;
}

int Application::Run()
{
	//Figure the time for the fps wanted defined by UAM_TARGET_FPS
	constexpr Uint64 kFrameDurationMs = 1000 / UAM_TARGET_FPS;

	// Run owns the SDL event pump and frame loop.
	while (!m_done && m_window != nullptr)
	{
		//GetFrame time start
		const Uint64 l_frameStartMs = SDL_GetTicks64();

		// The outer loop drives frames until shutdown is requested or the window is gone.
		SDL_Event l_event;

		// SDL may queue multiple input/window events between frames, so drain them before Update().
		while (SDL_PollEvent(&l_event))
		{
			// Let an embedded terminal consume Escape first so app-level handlers do not steal it.
			if (ForwardEscapeToSelectedCliTerminal(m_app, l_event))
			{
				continue;
			}

			ImGui_ImplSDL2_ProcessEvent(&l_event);

			if (l_event.type == SDL_QUIT)
			{
				m_done = true;
			}

			if (l_event.type == SDL_WINDOWEVENT && l_event.window.event == SDL_WINDOWEVENT_CLOSE && l_event.window.windowID == SDL_GetWindowID(m_window))
			{
				m_done = true;
			}

			if (l_event.type == SDL_WINDOWEVENT && l_event.window.windowID == SDL_GetWindowID(m_window))
			{
				const Uint8 l_windowEvent = l_event.window.event;

				if (l_windowEvent == SDL_WINDOWEVENT_SIZE_CHANGED || l_windowEvent == SDL_WINDOWEVENT_RESIZED || l_windowEvent == SDL_WINDOWEVENT_MAXIMIZED || l_windowEvent == SDL_WINDOWEVENT_RESTORED)
				{
					// Persist size/state changes as they happen.
					PersistWindowStateAndSettings();
				}
			}
		}

		//Call the actual apps logic and update loop.
		Update();

		//Enforce a strick fps set at the start of run.
		const Uint64 l_frameElapsedMs = SDL_GetTicks64() - l_frameStartMs;
		if (l_frameElapsedMs < kFrameDurationMs)
		{
			SDL_Delay(static_cast<Uint32>(kFrameDurationMs - l_frameElapsedMs));
		}
	}

	return m_exitCode;
}

bool Application::OnLoad()
{
	// Reset application-owned state before runtime/services initialize.
	m_app = uam::AppState();
	m_platformServices = &PlatformServicesFactory::Instance();
	m_window = nullptr;
	m_glContext = nullptr;
	m_glslVersion = nullptr;
	m_platformUiScale = 1.0f;
	m_done = false;
	m_terminalsStoppedForShutdown = false;
	m_sdlInitialized = false;
	m_imguiInitialized = false;
	m_curlInitialized = false;
	m_exitCode = 0;

	if (!InitializeState())
	{
		return false;
	}

	return InitializeWindowAndUi();
}

bool Application::Update()
{
	if (m_done)
	{
		// Stop terminal work immediately once shutdown has been requested.
		FastStopCliTerminalsForExit(m_app);
		m_terminalsStoppedForShutdown = true;
		return false;
	}

	// Poll runtime work before drawing the next frame.
	PollPendingRuntimeCall(m_app);
	PollAllCliTerminals(m_app);
	ProviderCliCompatibilityService().Poll(m_app);

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	m_uiController.DrawFrame(m_app,
	                         m_done,
	                         m_platformUiScale,
	                         m_platformServices->ui_traits,
	                         m_chatDetailView,
	                         m_modalHostView);

	ImGui::Render();
	PresentFrame();
	return !m_done;
}

void Application::Shutdown()
{
	// Shutdown only app-owned runtime state; low-level teardown stays in the destructor.
	PersistWindowStateAndSettings();
	for (PendingRuntimeCall& call : m_app.pending_calls)
	{
		ResetPendingRuntimeCall(call);
	}
	m_app.pending_calls.clear();
	m_app.resolved_native_sessions_by_chat_id.clear();
	ResetAsyncCommandTask(m_app.runtime_cli_version_check_task);
	ResetAsyncCommandTask(m_app.runtime_cli_pin_task);
	ResetNativeChatLoadTask(m_app.native_chat_load_task);

	if (!m_terminalsStoppedForShutdown)
	{
		StopAllCliTerminals(m_app, true);
		m_terminalsStoppedForShutdown = true;
	}

	RuntimeLocalService().StopLocalBridge(m_app);
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
	ChatHistorySyncService().LoadSidebarChats(m_app);

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

	ChatHistorySyncService().RefreshNativeSessionDirectory(m_app);

	if (ProviderResolutionService().ActiveProviderUsesNativeOverlayHistory(m_app) && m_app.native_history_chats_dir.empty())
	{
		m_app.status_line = "Native session directory not found yet. Run the provider CLI in this project once.";
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
