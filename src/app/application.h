#include "common/discovery/management_discovery.h"
#pragma once

#include "cef/cef_includes.h"
#include "include/cef_thread.h"
#include "common/platform/platform_services.h"
#include "common/state/app_state.h"
#include "app/runtime_orchestration_services.h"
#include "app/startup_native_import_service.h"
#include "app/model_discovery_service.h"
#include "app/opencode_compaction_sync_service.h"
#include "app/opencode_native_activity_service.h"
#include "app/computer_use_service.h"
#include "app/worktree_retention_service.h"

#include <future>
#include <chrono>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class UamQueryHandler;

/// <summary>
/// Top-level application lifetime manager for the CEF build.
/// Owns AppState and all service orchestration for React/CEF and xterm.js.
/// </summary>
class Application
{
  public:
	Application();
	~Application();
	Application(const Application&) = delete;
	Application& operator=(const Application&) = delete;
	Application(Application&&) = delete;
	Application& operator=(Application&&) = delete;

	/// <summary>
	/// Called from main() once CEF has been initialized.
	/// Runs CefRunMessageLoop() until the window closes or management mode receives a stop signal.
	/// </summary>
	int Run(CefMainArgs main_args, std::vector<std::string> launch_arguments = {});

	/// <summary>
	/// Called periodically from a CefTask on the UI thread.
	/// Polls runtime state and pushes updates to the React frontend.
	/// </summary>
	void PollTick();
	void ApplyBundledMarkdownStoreSeed(bool seeded, std::string error, std::vector<ShellAction> shell_actions, const std::string& destination_directory);

  private:
	std::unique_ptr<UamQueryHandler> m_managementHandler;
	std::unique_ptr<uam::discovery::ManagementAnnouncement> m_managementAnnouncement;
	bool m_managementNode = false;
	uam::AppState m_app;
	uam::StartupModelDiscoveryService m_startupModelDiscovery;
	std::shared_ptr<uam::OpenCodeCompactionSyncService> m_openCodeCompactionSync;
	uam::ComputerUseService m_computerUseService;
	uam::WorktreeRetentionService m_worktreeRetention;
	PlatformServices* m_platformServices = nullptr;
	std::shared_ptr<uam::platform::DataRootLock> m_dataRootLock;
	std::shared_ptr<std::atomic_bool> m_lifetimeToken = std::make_shared<std::atomic_bool>(true);
	CefRefPtr<CefBrowser> m_browser;
	CefRefPtr<CefThread> m_markdownSeedWorker;
	bool m_done = false;
	bool m_settingsLoaded = false;
	bool m_cefInitialized = false;
	bool m_shutdownComplete = false;
	bool m_shellActionInvocation = false;
	std::future<std::vector<ChatFolder>> m_workspaceAvailabilityScan;
	double m_nextWorkspaceAvailabilityScan = 0.0;
	bool PollWorkspaceAvailability();
	std::filesystem::path m_pendingBundledMarkdownRoot;
	std::string m_pendingMarkdownStoreDirectory;
	std::vector<std::string> m_launchArguments;
	int m_exitCode = 0;
	bool PollHistoryDiscovery();

	// ---- startup / teardown -----------------------------------------------
	bool InitializeState();
	bool InitializeCef(CefMainArgs main_args);
	void Shutdown();

	// ---- periodic work (posted to CEF UI thread) --------------------------
	void Update();
	void ScheduleNextUpdate(int delay_ms = 1000);

	// ---- CEF ready callback -----------------------------------------------
	void OnRuntimeReady(CefRefPtr<CefBrowser> browser);
	void OnManagementReady();
};
