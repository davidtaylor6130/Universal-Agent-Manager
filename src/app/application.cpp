#include "application.h"

#include "application_core_helpers.h"
#include "chat_domain_service.h"
#include "persistence_coordinator.h"
#include "provider_resolution_service.h"
#include "runtime_orchestration_services.h"
#include "memory_service.h"

#include "common/constants/app_constants.h"
#include "common/models/app_models.h"
#include "common/paths/app_paths.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/config/frontend_actions.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal_polling.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/config/settings_store.h"
#include "common/platform/platform_services.h"

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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

	bool IsMacAppBundleExecutable(const fs::path& exe_path)
	{
#if defined(__APPLE__)
		const fs::path normalized = exe_path.lexically_normal();
		const fs::path macos_dir = normalized.parent_path();
		const fs::path contents_dir = macos_dir.parent_path();
		const fs::path app_dir = contents_dir.parent_path();
		return !app_dir.empty() && macos_dir.filename() == "MacOS" && contents_dir.filename() == "Contents" && app_dir.extension() == ".app";
#else
		(void)exe_path;
		return false;
#endif
	}

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

	void ResetNativeChatLoadTasks(std::unordered_map<std::string, uam::platform::AsyncNativeChatLoadTask>& tasks)
	{
		for (auto& entry : tasks)
		{
			ResetNativeChatLoadTask(entry.second);
		}

		tasks.clear();
	}

	struct RuntimeCliCompatibilitySnapshot
	{
		bool runtime_cli_version_checked = false;
		bool runtime_cli_version_supported = false;
		std::string runtime_cli_installed_version;
		std::string runtime_cli_version_raw_output;
		std::string runtime_cli_version_message;
		std::string runtime_cli_pin_output;
		std::string status_line;
	};

	RuntimeCliCompatibilitySnapshot CaptureRuntimeCliCompatibilitySnapshot(const uam::AppState& app)
	{
		RuntimeCliCompatibilitySnapshot snapshot;
		snapshot.runtime_cli_version_checked = app.runtime_cli_version_checked;
		snapshot.runtime_cli_version_supported = app.runtime_cli_version_supported;
		snapshot.runtime_cli_installed_version = app.runtime_cli_installed_version;
		snapshot.runtime_cli_version_raw_output = app.runtime_cli_version_raw_output;
		snapshot.runtime_cli_version_message = app.runtime_cli_version_message;
		snapshot.runtime_cli_pin_output = app.runtime_cli_pin_output;
		snapshot.status_line = app.status_line;
		return snapshot;
	}

	bool RuntimeCliCompatibilitySnapshotChanged(const RuntimeCliCompatibilitySnapshot& before, const RuntimeCliCompatibilitySnapshot& after)
	{
		return before.runtime_cli_version_checked != after.runtime_cli_version_checked || before.runtime_cli_version_supported != after.runtime_cli_version_supported || before.runtime_cli_installed_version != after.runtime_cli_installed_version || before.runtime_cli_version_raw_output != after.runtime_cli_version_raw_output || before.runtime_cli_version_message != after.runtime_cli_version_message || before.runtime_cli_pin_output != after.runtime_cli_pin_output || before.status_line != after.status_line;
	}

	bool HasSelectedActiveRuntime(const uam::AppState& app)
	{
		const ChatSession* selected_chat = ChatDomainService().SelectedChat(app);
		if (selected_chat == nullptr)
		{
			return false;
		}

		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal != nullptr && terminal->running && CliTerminalMatchesChatId(*terminal, selected_chat->id))
			{
				return true;
			}
		}

		const uam::AcpSessionState* acp = FindAcpSessionForChat(app, selected_chat->id);
		return acp != nullptr && acp->running;
	}

	bool HasAnyActiveRuntime(const uam::AppState& app)
	{
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal != nullptr && terminal->running)
			{
				return true;
			}
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

	int NextPollDelayMs(const uam::AppState& app)
	{
		if (HasSelectedActiveRuntime(app))
		{
			return 50;
		}
		if (HasAnyActiveRuntime(app))
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
				m_app->PollTick();
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

int Application::Run(CefMainArgs main_args)
{
	m_platformServices = &PlatformServicesFactory::Instance();

	if (!InitializeState())
	{
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

	if (m_done)
		return;

	const RuntimeCliCompatibilitySnapshot provider_snapshot_before = CaptureRuntimeCliCompatibilitySnapshot(m_app);
	const bool pending_calls_changed = PollPendingRuntimeCall(m_app);
	const bool acp_sessions_changed = uam::PollAllAcpSessions(m_app);
	const bool cli_terminals_changed = PollAllCliTerminals(m_browser, m_app);
	const bool memory_changed = MemoryService::ProcessDueMemoryWork(m_app);
	ProviderCliCompatibilityService().Poll(m_app);
	const bool provider_compatibility_changed = RuntimeCliCompatibilitySnapshotChanged(provider_snapshot_before, CaptureRuntimeCliCompatibilitySnapshot(m_app));
	const bool ui_relevant_state_changed = pending_calls_changed || acp_sessions_changed || cli_terminals_changed || memory_changed || provider_compatibility_changed;

	// Push only when the serialized app state actually changed.
	if (m_browser && ui_relevant_state_changed)
		uam::PushStateUpdateIfChanged(m_browser, m_app);

	ScheduleNextUpdate(NextPollDelayMs(m_app));
}

void Application::ScheduleNextUpdate(const int delay_ms)
{
	if (!m_done)
		CefPostDelayedTask(TID_UI, new AppPollTask(this), delay_ms);
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
	std::vector<fs::path> l_dataRootCandidates;
	std::error_code l_exeEc;
	const fs::path l_exePath = m_platformServices->process_service.ResolveCurrentExecutablePath();
	const bool l_runningFromMacAppBundle = !l_exeEc && !l_exePath.empty() && IsMacAppBundleExecutable(l_exePath);

	if (const char* lcp_dataDirEnv = std::getenv("UAM_DATA_DIR"))
	{
		const std::string l_envRoot = Trim(lcp_dataDirEnv);
		if (!l_envRoot.empty())
			l_dataRootCandidates.push_back(fs::path(l_envRoot));
	}

	if (!l_exePath.empty() && !l_runningFromMacAppBundle)
		l_dataRootCandidates.push_back(l_exePath.parent_path() / "data");

	if (!l_runningFromMacAppBundle)
	{
		std::error_code l_cwdEc;
		const fs::path l_cwd = fs::current_path(l_cwdEc);
		if (!l_cwdEc)
			l_dataRootCandidates.push_back(l_cwd / "data");
	}

	l_dataRootCandidates.push_back(m_platformServices->path_service.DefaultDataRootPath());
	l_dataRootCandidates.push_back(PersistenceCoordinator().TempFallbackDataRootPath());

	std::unordered_set<std::string> l_triedRoots;
	std::string l_lastDataRootError = "Unknown data directory initialization failure.";
	bool l_initializedDataRoot = false;

	for (const fs::path& l_candidateRoot : l_dataRootCandidates)
	{
		if (l_candidateRoot.empty())
			continue;

		const std::string l_key = l_candidateRoot.lexically_normal().string();
		if (l_triedRoots.find(l_key) != l_triedRoots.end())
			continue;

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

	std::string data_root_lock_error;
	m_dataRootLock = m_platformServices->process_service.TryAcquireDataRootLock(m_app.data_root, &data_root_lock_error);
	if (m_dataRootLock == nullptr)
	{
		if (data_root_lock_error.empty())
		{
			data_root_lock_error = "Another Universal Agent Manager instance is already using this data root.";
		}
		std::fprintf(stderr, "%s\n", data_root_lock_error.c_str());
		m_exitCode = 1;
		return false;
	}

	PersistenceCoordinator().LoadSettings(m_app);
	bool l_settingsDirty = false;
	m_app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	if (ProviderResolutionService().ActiveProvider(m_app) == nullptr && !m_app.provider_profiles.empty())
	{
		m_app.settings.active_provider_id = m_app.provider_profiles.front().id;
		l_settingsDirty = true;
	}

	if (ProviderProfile* lp_activeProfile = ProviderResolutionService().ActiveProvider(m_app); lp_activeProfile != nullptr)
	{
		m_app.settings.provider_command_template = lp_activeProfile->command_template;
		m_app.settings.gemini_command_template = m_app.settings.provider_command_template;
		m_app.settings.runtime_backend = "provider-cli";

		if (!ProviderRuntime::IsRuntimeEnabled(*lp_activeProfile))
		{
			const std::string l_disabledReason = ProviderRuntime::DisabledReason(*lp_activeProfile);
			m_app.status_line = l_disabledReason.empty() ? "Active provider runtime is disabled in this build." : l_disabledReason;
		}
	}

	m_app.folders = ChatFolderStore::Load(m_app.data_root);
	ChatHistorySyncService().LoadSidebarChatsByDiscovery(m_app);
	MemoryService::RefreshMemoryActivity(m_app);

	if (!m_app.chats.empty())
	{
		if (m_app.settings.remember_last_chat && !m_app.settings.last_selected_chat_id.empty())
			m_app.selected_chat_index = ChatDomainService().FindChatIndexById(m_app, m_app.settings.last_selected_chat_id);

		if (m_app.selected_chat_index < 0 || m_app.selected_chat_index >= static_cast<int>(m_app.chats.size()))
			m_app.selected_chat_index = 0;

		ChatDomainService().RefreshRememberedSelection(m_app);
	}

	if (l_settingsDirty)
		PersistenceCoordinator().SaveSettings(m_app);

	// Make AppState accessible to CEF app/client via global pointer.
	uam_cef_globals::g_app_state = &m_app;

	return true;
}

bool Application::InitializeCef(CefMainArgs main_args)
{
	CefSettings settings;
	settings.no_sandbox = true;

	// Resolve CEF resource paths relative to the executable.
	CefString exe_dir_str;
	if (CefGetPath(PK_DIR_EXE, exe_dir_str))
	{
		const fs::path exe_dir(exe_dir_str.ToString());

#if defined(__APPLE__)
		// On macOS, the helper app sits in Contents/Frameworks/ alongside the
		// CEF framework.  Tell CEF exactly where to find it so it does not have
		// to guess; this prevents the EXC_BREAKPOINT / SIGTRAP crash that occurs
		// when Chromium cannot locate its renderer / GPU subprocesses.
		const fs::path helper_path = exe_dir / ".." / "Frameworks" / "universal_agent_manager Helper.app" / "Contents" / "MacOS" / "universal_agent_manager Helper";
		if (fs::exists(helper_path))
		{
			CefString(&settings.browser_subprocess_path) = helper_path.lexically_normal().string();
		}
		// On macOS the CEF framework is self-contained; resource paths are
		// resolved automatically from the framework bundle.  No need to set
		// resources_dir_path / locales_dir_path explicitly.
#else
		CefString(&settings.resources_dir_path) = exe_dir.string();
		CefString(&settings.locales_dir_path) = (exe_dir / "locales").string();
#endif
	}

	auto cef_app = CefRefPtr<UamCefApp>(new UamCefApp());

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

	return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void Application::Shutdown()
{
	m_done = true;
	PersistenceCoordinator().SaveSettings(m_app);

	for (PendingRuntimeCall& call : m_app.pending_calls)
		ResetPendingRuntimeCall(call);
	m_app.pending_calls.clear();
	m_app.resolved_native_sessions_by_chat_id.clear();
	ResetAsyncCommandTask(m_app.runtime_cli_version_check_task);
	ResetAsyncCommandTask(m_app.runtime_cli_pin_task);
	MemoryService::StopMemoryTasks(m_app);
	ResetNativeChatLoadTask(m_app.native_chat_load_task);
	ResetNativeChatLoadTasks(m_app.native_chat_load_tasks);
	uam::FastStopAcpSessionsForExit(m_app);
	FastStopCliTerminalsForExit(m_app);

	uam_cef_globals::g_app_state = nullptr;
	uam_cef_globals::g_client = nullptr;
	m_browser = nullptr;

	CefShutdown();
	m_dataRootLock.reset();
}
