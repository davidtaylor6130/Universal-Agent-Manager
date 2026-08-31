#include "application.h"

#include "agent_run_ledger.h"
#include "agent_run_scheduler.h"
#include "chat_domain_service.h"
#include "chat_lifecycle_service.h"
#include "computer_use_service.h"
#include "goal_service.h"
#include "markdown_store_service.h"
#include "persistence_coordinator.h"
#include "provider_model_catalog_service.h"
#include "provider_resolution_service.h"
#include "resource_collection_service.h"
#include "runtime_activity.h"
#include "runtime_orchestration_services.h"
#include "shell_action_service.h"
#include "theme_service.h"
#include "uam_control_service.h"
#include "memory_service.h"

#include "common/constants/app_constants.h"
#include "common/models/app_models.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/config/frontend_actions.h"
#include "common/config/settings_normalization.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal_polling.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/config/settings_store.h"
#include "common/platform/platform_services.h"
#include "common/utils/env_utils.h"
#include "common/utils/string_utils.h"

#include "cef/cef_push.h"
#include "cef/cef_includes.h"
#include "cef/uam_cef_security.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_app.h"
#include "cef/uam_cef_client.h"
#include "include/cef_path_util.h"

#include "include/cef_app.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// uam_cef_globals — shared with uam_cef_app.cpp
// ---------------------------------------------------------------------------

namespace uam_cef_globals
{
	uam::AppState* g_app_state = nullptr;
	CefRefPtr<UamCefClient> g_client;
} // namespace uam_cef_globals

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{
	std::string WorkspaceFolderAvailabilityFingerprint(const std::vector<ChatFolder>& folders)
	{
		std::string result;
		for (const ChatFolder& folder : folders)
		{
			result += folder.id;
			result.push_back('\0');
			if (!uam::paths::IsControllerLocalWorkspace(folder))
			{
				result.push_back('1');
				continue;
			}
			const fs::path directory = uam::paths::PathFromUtf8(folder.directory);
			const bool can_probe = PlatformServicesFactory::Instance().path_service.CanProbeDirectoryWithoutPrompt(directory);
			result.push_back(folder.directory.empty() || !can_probe || uam::paths::IsDirectoryNoThrow(directory) ? '1' : '0');
		}
		return result;
	}

	void ResetRuntimeCliVersionState(uam::AppState& app)
	{
		uam::ResetAsyncCommandTask(app.runtime_cli_version_check_task);
		uam::ResetAsyncCommandTask(app.runtime_cli_pin_task);
		app.runtime_cli_version_provider_id.clear();
		app.runtime_cli_version_check_queue.clear();
		app.runtime_cli_pin_provider_id.clear();
		app.runtime_cli_versions_by_provider_id.clear();
	}

	std::string CalculateCliVersionStateSignature(const std::unordered_map<std::string, uam::CliProviderVersionState>& states)
	{
		std::vector<const std::pair<const std::string, uam::CliProviderVersionState>*> ordered_states;
		ordered_states.reserve(states.size());

		for (const auto& entry : states)
		{
			ordered_states.push_back(&entry);
		}

		std::ranges::sort(ordered_states, [](const auto* lhs, const auto* rhs) {
			return lhs->first < rhs->first;
		});

		std::string signature;
		for (const auto* entry : ordered_states)
		{
			const std::string& provider_id = entry->first;
			const uam::CliProviderVersionState& state = entry->second;
			signature += provider_id;
			signature.push_back('\0');
			signature += state.checked ? "1" : "0";
			signature.push_back('\0');
			signature += state.supported ? "1" : "0";
			signature.push_back('\0');
			signature += state.installed_version;
			signature.push_back('\0');
			signature += state.selected_version;
			signature.push_back('\0');
			signature += state.raw_output;
			signature.push_back('\0');
			signature += state.message;
			signature.push_back('\0');
			signature += state.install_output;
			signature.push_back('\0');
		}

		return signature;
	}

	struct RuntimeCliCompatibilitySnapshot
	{
		std::string runtime_cli_version_provider_id;
		std::string runtime_cli_pin_provider_id;
		std::string provider_state_signature;
		std::string status_line;
	};

	RuntimeCliCompatibilitySnapshot CreateCliCompatibilitySnapshot(const uam::AppState& app)
	{
		RuntimeCliCompatibilitySnapshot snapshot;
		snapshot.runtime_cli_version_provider_id = app.runtime_cli_version_provider_id;
		snapshot.runtime_cli_pin_provider_id = app.runtime_cli_pin_provider_id;
		snapshot.provider_state_signature = CalculateCliVersionStateSignature(app.runtime_cli_versions_by_provider_id);
		snapshot.status_line = app.status_line;
		return snapshot;
	}

	bool IsCliCompatibilitySnapshotChanged(const RuntimeCliCompatibilitySnapshot& before, const RuntimeCliCompatibilitySnapshot& after)
	{
		if (before.runtime_cli_version_provider_id != after.runtime_cli_version_provider_id)
		{
			return true;
		}
		if (before.runtime_cli_pin_provider_id != after.runtime_cli_pin_provider_id)
		{
			return true;
		}
		if (before.provider_state_signature != after.provider_state_signature)
		{
			return true;
		}
		return before.status_line != after.status_line;
	}

	bool IsSelectedChatRunning(const uam::AppState& app)
	{
		const ChatSession* selected_chat = ChatDomainService().SelectedChat(app);
		if (selected_chat == nullptr)
		{
			return false;
		}

		if (const uam::CliTerminalState* terminal = uam::FindCliTerminalForChat(app, *selected_chat);
		    terminal != nullptr && terminal->running && uam::CliTerminalLifecycleIsProcessing(*terminal))
		{
			return true;
		}

		const uam::AcpSessionState* acp = FindAcpSessionForChat(app, selected_chat->id);
		return acp != nullptr && acp->running && uam::AcpSessionHasBlockingRuntimeWork(*acp);
	}

	bool IsAnyRuntimeActive(const uam::AppState& app)
	{
		if (uam::HasAnyActiveCliTerminal(app))
		{
			return true;
		}

		for (const auto& session : app.acp_sessions)
		{
			if (session != nullptr && session->running)
			{
				return true;
			}
		}

		return !app.pending_calls.empty() || !app.memory_extraction_tasks.empty() || !app.memory_extraction_queue.empty();
	}

	int GetNextPollDelayMs(const uam::AppState& app, bool dictation_running)
	{
		if (dictation_running)
		{
			return 50;
		}
		if (IsSelectedChatRunning(app))
		{
			return 16;
		}
		if (IsAnyRuntimeActive(app))
		{
			return 250;
		}
		return 1000;
	}

	// ---- Periodic poll task ---------------------------------------------------

	/// CefTask that calls Application::PollTick() on the CEF UI thread.
	class AppPollTask : public CefTask
	{
	  public:
		explicit AppPollTask(Application* app) : m_app(app)
		{
		}
		void Execute() override
		{
			if (m_app)
			{
				m_app->PollTick();
			}
		}

	  private:
		Application* m_app;
		IMPLEMENT_REFCOUNTING(AppPollTask);
	};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

Application::Application()
{
	// State initialization is deferred to Run() so that CEF is already up.
}

Application::~Application()
{
	Shutdown();

	m_platformServices = nullptr;
}

int Application::Run(CefMainArgs main_args, std::vector<std::string> launch_arguments)
{
	m_platformServices = &PlatformServicesFactory::Instance();
	m_launchArguments = std::move(launch_arguments);

	if (!InitializeState())
	{
		if (m_shellActionInvocation && m_exitCode == 0)
		{
			return 0;
		}
		return m_exitCode != 0 ? m_exitCode : 1;
	}

	if (!InitializeCef(main_args))
	{
		return m_exitCode != 0 ? m_exitCode : 1;
	}

	// CefRunMessageLoop() blocks until OnBeforeClose calls CefQuitMessageLoop().
	CefRunMessageLoop();
	return m_exitCode;
}

// ---------------------------------------------------------------------------
// Periodic poll
// ---------------------------------------------------------------------------

void Application::PollTick()
{
	CEF_REQUIRE_UI_THREAD();
	const auto poll_started = std::chrono::steady_clock::now();

	if (m_done)
	{
		return;
	}

	const RuntimeCliCompatibilitySnapshot provider_snapshot_before = CreateCliCompatibilitySnapshot(m_app);
	const bool pending_calls_changed = PollPendingRuntimeCall(m_app);
	const bool acp_sessions_changed = uam::PollAllAcpSessions(m_app, m_browser);
	const bool uam_control_changed = uam::UamControlService::ProcessPendingRequests(m_app);
	const bool agent_runs_changed = uam::AgentRunScheduler::Poll(m_app);
	uam::FlushPendingChatSaves(m_app);
	const bool cli_terminals_changed = uam::PollAllCliTerminals(m_browser, m_app);
	const bool memory_changed = MemoryService::ProcessDueMemoryWork(m_app);
	const bool computer_use_changed = uam::ComputerUseService::Poll(m_app);
	const bool shell_actions_changed = ShellActionService::ProcessPendingRequests(m_app);
	const std::string folder_availability = WorkspaceFolderAvailabilityFingerprint(m_app.folders);
	const bool folder_availability_changed = folder_availability != m_workspaceFolderAvailabilityFingerprint;
	m_workspaceFolderAvailabilityFingerprint = folder_availability;
	if (shell_actions_changed && m_browser && m_browser->GetHost())
	{
		m_browser->GetHost()->SetFocus(true);
#if defined(_WIN32)
		const HWND window = m_browser->GetHost()->GetWindowHandle();
		if (window != nullptr)
		{
			ShowWindow(window, SW_RESTORE);
			SetForegroundWindow(window);
		}
#endif
	}
	ProviderCliCompatibilityService().Poll(m_app);
	const bool model_discovery_retry_changed = uam::RetryCompatibilityBlockedAcpModelDiscoveries(m_app);

	// Poll the provider model catalog service for async model refresh completion.
	if (m_app.provider_model_catalog != nullptr)
	{
		m_app.provider_model_catalog->Poll();
		m_app.provider_model_catalog->MaybeStartRefresh();
	}
	const bool provider_compatibility_changed = IsCliCompatibilitySnapshotChanged(provider_snapshot_before, CreateCliCompatibilitySnapshot(m_app));
	const bool runtime_state_changed = pending_calls_changed || acp_sessions_changed || uam_control_changed || agent_runs_changed || cli_terminals_changed || memory_changed || computer_use_changed || shell_actions_changed || folder_availability_changed || model_discovery_retry_changed;
	const bool ui_relevant_state_changed = runtime_state_changed || provider_compatibility_changed || uam::HasDeferredStatePush();
	for (const DictationEvent& event : m_platformServices->dictation_service.PollEvents())
	{
		uam::PushDictationEvent(m_browser, event);
	}

	// Push only when the serialized app state actually changed.
	if (m_browser && ui_relevant_state_changed)
	{
		uam::PushStateUpdateIfChanged(m_browser, m_app);
	}

	// Keep the machine awake while an agent turn or goal loop is active so
	// display sleep / App Nap cannot pause this polling loop mid-goal.
	m_platformServices->process_service.SetKeepSystemAwake(uam::RuntimeShouldKeepSystemAwake(m_app));

	const auto poll_finished = std::chrono::steady_clock::now();
	const auto poll_duration = std::chrono::duration_cast<std::chrono::milliseconds>(poll_finished - poll_started);
	static auto last_slow_poll_report = std::chrono::steady_clock::time_point::min(); // ponytail: UI-thread rate limit; move to metrics state if telemetry needs aggregation.
	if (poll_duration >= std::chrono::milliseconds(50) &&
	    (last_slow_poll_report == std::chrono::steady_clock::time_point::min() || poll_finished - last_slow_poll_report >= std::chrono::seconds(5)))
	{
		std::cerr << "[performance] PollTick took " << poll_duration.count() << " ms.\n";
		last_slow_poll_report = poll_finished;
	}

	ScheduleNextUpdate(GetNextPollDelayMs(m_app, m_platformServices->dictation_service.IsRunning()));
}

void Application::ScheduleNextUpdate(int delay_ms)
{
	if (!m_done)
	{
		CefPostDelayedTask(TID_UI, new AppPollTask(this), delay_ms);
	}
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

void Application::OnBrowserReady(CefRefPtr<CefBrowser> browser)
{
	m_browser = browser;
	// Start the polling loop as soon as the browser window exists.
	ScheduleNextUpdate(50);
}

bool Application::InitializeState()
{
	const fs::path executable_path = m_platformServices->process_service.ResolveCurrentExecutablePath();
	const std::optional<fs::path> data_root_env = uam::env::GetTrimmedPath("UAM_DATA_DIR");
	m_app.data_root = data_root_env.value_or(m_platformServices->path_service.DefaultDataRootPath());
	std::string data_root_error;
	if (m_app.data_root.empty() || !PersistenceCoordinator().EnsureDataRootLayout(m_app.data_root, &data_root_error))
	{
		std::fprintf(stderr,
		             "Failed to initialize the requested application data root '%s'. UAM will not open a fallback database: %s\n",
		             uam::paths::Utf8PathString(m_app.data_root).c_str(),
		             data_root_error.empty() ? "invalid or empty path" : data_root_error.c_str());
		m_exitCode = 1;
		return false;
	}
	std::fprintf(stderr, "[storage] data_root=%s\n", uam::paths::Utf8PathString(m_app.data_root).c_str());
	(void)uam::env::SetString("UAM_DATA_DIR", uam::paths::Utf8PathString(m_app.data_root));

	std::string shell_action_error;
	if (!ShellActionService::QueueLaunchRequest(m_app.data_root, m_launchArguments, &m_shellActionInvocation, &shell_action_error))
	{
		std::fprintf(stderr, "%s\n", shell_action_error.c_str());
		m_exitCode = 1;
		return false;
	}

	std::string data_root_lock_error;
	m_dataRootLock = m_platformServices->process_service.TryAcquireDataRootLock(m_app.data_root, &data_root_lock_error);
	if (m_dataRootLock == nullptr)
	{
		if (m_shellActionInvocation)
		{
			m_exitCode = 0;
			return false;
		}
		if (data_root_lock_error.empty())
		{
			data_root_lock_error = "Another Universal Agent Manager instance is already using this data root.";
		}
		std::fprintf(stderr, "%s\n", data_root_lock_error.c_str());
		m_exitCode = 1;
		return false;
	}
	std::string uam_control_error;
	if (!uam::UamControlService::Initialize(m_app, &uam_control_error))
	{
		std::fprintf(stderr, "%s\n", uam_control_error.c_str());
		m_exitCode = 1;
		return false;
	}
	uam::AgentRunLoadResult agent_runs = uam::AgentRunLedger::LoadAll(m_app.data_root);
	(void)uam::AgentRunLedger::MarkNonterminalInterrupted(
	    m_app.data_root, &agent_runs.runs, "manager_restarted", &agent_runs.errors);
	m_app.agent_runs = std::move(agent_runs.runs);
	for (const std::string& error : agent_runs.errors)
	{
		std::fprintf(stderr, "[agent-runs] %s\n", error.c_str());
	}

	if (!PersistenceCoordinator().LoadSettings(m_app))
	{
		std::fprintf(stderr, "%s\n", m_app.status_line.c_str());
		m_exitCode = 1;
		return false;
	}
	m_settingsLoaded = true;
	bool settings_dirty = false;
	const fs::path bundled_skills_root = MarkdownStoreService::BundledRootForExecutable(executable_path);
	if (m_app.settings.markdown_store_directory.empty())
	{
		m_app.settings.markdown_store_directory = uam::paths::Utf8PathString(m_app.data_root / "markdown-store");
		settings_dirty = true;
	}
	const fs::path configured_skills_root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	bool bundled_skills_ready = false;
	if (!bundled_skills_root.empty())
	{
		std::string bundled_skills_error;
		bundled_skills_ready = MarkdownStoreService::SeedBundledEntries(bundled_skills_root, configured_skills_root, &bundled_skills_error);
		if (!bundled_skills_ready && !bundled_skills_error.empty())
			m_app.status_line = "Could not import bundled skills: " + bundled_skills_error;
	}
	if (ThemeService::IsCustomThemeId(m_app.settings.ui_theme) && !ThemeService::Exists(m_app.data_root, m_app.settings.ui_theme))
	{
		m_app.settings.ui_theme = uam::settings::kFocusThemeId;
		settings_dirty = true;
	}
	// PR-7: provider profiles are build-defined, not user data. They are reset to the
	// built-in set on every startup and never persisted, so any per-profile "store" plumbing
	// (e.g. EnsureDefaultProfile) is belt-and-suspenders unless profiles later become editable.
	m_app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	if (!uam::RecoverPendingDeletionTransaction(m_app))
	{
		std::fprintf(stderr, "%s\n", m_app.status_line.c_str());
		m_exitCode = 1;
		return false;
	}
	ProviderCliCompatibilityService().StartVersionCheck(m_app, false);
	if (ProviderResolutionService().ActiveProvider(m_app) == nullptr && !m_app.provider_profiles.empty())
	{
		m_app.settings.active_provider_id = m_app.provider_profiles.front().id;
		settings_dirty = true;
	}

	if (ProviderProfile* active_profile = ProviderResolutionService().ActiveProvider(m_app); active_profile != nullptr)
	{

		if (!ProviderRuntime::IsRuntimeEnabled(*active_profile))
		{
			const std::string disabled_reason = ProviderRuntime::DisabledReason(*active_profile);
			m_app.status_line = uam::strings::NonEmptyOrFallback(disabled_reason, "Active provider runtime is disabled in this build.");
		}
	}

	m_app.folders = ChatFolderStore::Load(m_app.data_root);
	m_app.shell_actions = ShellActionService::Load(m_app.data_root, bundled_skills_ready ? configured_skills_root : fs::path{});

	// Initialize the provider model catalog service (async refresh for OpenCode Zen models).
	m_app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	m_app.provider_model_catalog->Initialize(m_app.data_root, m_app.provider_profiles, m_app.settings.provider_extra_flags);

	ChatHistorySyncService().LoadSidebarChats(m_app);
	uam::MigrateWorkspaceFolderOwnership(m_app);
	m_workspaceFolderAvailabilityFingerprint = WorkspaceFolderAvailabilityFingerprint(m_app.folders);
	if (const std::size_t reconnecting = uam::RestoreRemoteAcpSessionsAfterRestart(m_app);
	    reconnecting > 0)
	{
		m_app.status_line = "Reconnecting " + std::to_string(reconnecting) +
		                    " remote turn" + (reconnecting == 1 ? "" : "s") + ".";
	}
	if (const std::size_t paused_goals = uam::GoalService::PauseActiveGoalsAfterRestart(m_app);
	    paused_goals > 0)
	{
		m_app.status_line = "Paused " + std::to_string(paused_goals) +
		                    " active goal" + (paused_goals == 1 ? "" : "s") +
		                    " after the manager restarted.";
	}
	m_app.resource_collections = uam::ResourceCollectionService::Load(m_app.data_root);
	MemoryService::RefreshMemoryActivity(m_app);

	if (!m_app.chats.empty())
	{
		ChatDomainService().SelectRememberedOrFirstChat(m_app);

		std::string hydrate_warning;
		ChatSession* selected_chat = ChatDomainService().SelectedChat(m_app);
		if (selected_chat != nullptr)
		{
			ChatRepository::HydrateChatMessages(m_app.data_root, *selected_chat, &hydrate_warning);
		}
		if (!hydrate_warning.empty())
			m_app.status_line = hydrate_warning;
	}
	if (!uam::ComputerUseService::ResetControlsForStartup(m_app))
	{
		m_app.status_line =
		    "A previous computer-use control file could not be stopped. End any orphaned provider process before using computer use.";
	}

	if (settings_dirty)
		PersistenceCoordinator().SaveSettings(m_app);

	// Make AppState accessible to CEF app/client via global pointer.
	uam_cef_globals::g_app_state = &m_app;

	return true;
}

bool Application::InitializeCef(CefMainArgs main_args)
{
	CefSettings settings;
	settings.no_sandbox = true;
	const fs::path cef_cache_path = m_app.data_root / "cef";
	CefString(&settings.root_cache_path) = uam::paths::NormalizedNativePathString(cef_cache_path);
	CefString(&settings.cache_path) = uam::paths::NormalizedNativePathString(cef_cache_path);
	CefString(&settings.log_file) = uam::paths::NormalizedNativePathString(m_app.data_root / "cef.log");

	// Resolve CEF resource paths relative to the executable.
	CefString exe_dir_str;
	if (CefGetPath(PK_DIR_EXE, exe_dir_str))
	{
		const fs::path exe_dir = uam::paths::PathFromUtf8(exe_dir_str.ToString());

#if defined(__APPLE__)
		// On macOS, the helper app sits in Contents/Frameworks/ alongside the
		// CEF framework.  Tell CEF exactly where to find it so it does not have
		// to guess; this prevents the EXC_BREAKPOINT / SIGTRAP crash that occurs
		// when Chromium cannot locate its renderer / GPU subprocesses.
		const fs::path helper_path = exe_dir / ".." / "Frameworks" / "universal_agent_manager Helper.app" / "Contents" / "MacOS" / "universal_agent_manager Helper";
		if (uam::paths::PathExistsNoThrow(helper_path))
		{
			CefString(&settings.browser_subprocess_path) = uam::paths::NormalizedNativePathString(helper_path);
		}
		// On macOS the CEF framework is self-contained; resource paths are
		// resolved automatically from the framework bundle.  No need to set
		// resources_dir_path / locales_dir_path explicitly.
#else
		CefString(&settings.resources_dir_path) = uam::paths::NormalizedNativePathString(exe_dir);
		CefString(&settings.locales_dir_path) = uam::paths::NormalizedNativePathString(exe_dir / "locales");
#endif
	}

	auto cef_app = CefRefPtr<UamCefApp>(new UamCefApp([this](const std::string& error)
	{
		std::fprintf(stderr, "[CEF] %s\n", error.c_str());
		m_exitCode = 1;
		CefQuitMessageLoop();
	}));

	// OnBrowserReady is called from UamCefClient::OnAfterCreated() via the
	// callback we pass here, giving us the browser reference for PushCliOutput etc.
	auto on_browser_ready = [this](CefRefPtr<CefBrowser> browser) { OnBrowserReady(browser); };

	// Store the ready callback so uam_cef_app.cpp can forward it to UamCefClient.
	// We piggyback on the existing g_client mechanism — when UamCefApp creates the
	// client in OnContextInitialized() it uses a default callback; we need to
	// supply our own.  Override g_client's callback after creation by constructing
	// a new client here and passing it through the globals.
	//
	// Simplest approach: create UamCefClient now and stash it so OnContextInitialized
	// can skip creating a new one.
	const fs::path exe_dir = m_platformServices->process_service.ResolveCurrentExecutablePath().parent_path();
	auto client = CefRefPtr<UamCefClient>(new UamCefClient(m_app, uam::cef::ResolveTrustedUiIndexUrl(exe_dir), on_browser_ready));
	uam_cef_globals::g_client = client;

	if (!CefInitialize(main_args, settings, cef_app.get(), nullptr))
	{
		std::fprintf(stderr, "CefInitialize failed.\n");
		m_exitCode = 1;
		return false;
	}
	m_cefInitialized = true;

	return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void Application::Shutdown()
{
	if (m_shutdownComplete)
	{
		return;
	}
	m_shutdownComplete = true;
	m_done = true;
	if (m_platformServices != nullptr)
	{
		m_platformServices->dictation_service.Stop();
	}
	if (m_settingsLoaded && m_dataRootLock != nullptr)
	{
		PersistenceCoordinator().SaveSettings(m_app);
	}

	for (PendingRuntimeCall& call : m_app.pending_calls)
	{
		ResetPendingRuntimeCall(call);
	}

	m_app.pending_calls.clear();
	m_app.resolved_native_sessions_by_chat_id.clear();
	ResetRuntimeCliVersionState(m_app);
	MemoryService::StopMemoryTasks(m_app);
	for (uam::AsyncPermissionReviewTask& task : m_app.permission_review_tasks)
	{
		uam::StopAsyncPermissionReviewTask(task);
	}
	m_app.permission_review_tasks.clear();
	uam::platform::ResetAsyncNativeChatLoadTask(m_app.native_chat_load_task);
	uam::platform::ResetAsyncNativeChatLoadTasks(m_app.native_chat_load_tasks);
	(void)uam::AgentRunScheduler::InterruptForShutdown(m_app);
	uam::FastStopAcpSessionsForExit(m_app);
	uam::UamControlService::Shutdown(m_app);
	uam::FastStopCliTerminalsForExit(m_app);

	m_browser = nullptr;
	uam_cef_globals::g_client = nullptr;
	uam_cef_globals::g_app_state = nullptr;

	if (m_cefInitialized)
	{
		CefShutdown();
		m_cefInitialized = false;
	}
	m_dataRootLock.reset();
}
