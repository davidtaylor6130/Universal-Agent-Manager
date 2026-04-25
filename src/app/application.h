#ifndef UAM_APP_APPLICATION_H
#define UAM_APP_APPLICATION_H

#include "cef/cef_includes.h"
#include "common/platform/platform_services.h"
#include "common/state/app_state.h"

#include <filesystem>
#include <memory>

/// <summary>
/// Top-level application lifetime manager for the CEF build.
/// Owns AppState and all service orchestration for React/CEF and xterm.js.
/// </summary>
class Application
{
  public:
	Application();
	~Application();
	Application(const Application&)            = delete;
	Application& operator=(const Application&) = delete;
	Application(Application&&)                 = delete;
	Application& operator=(Application&&)      = delete;

	/// <summary>
	/// Called from main() once CEF has been initialized.
	/// Runs CefRunMessageLoop() until the window is closed.
	/// </summary>
	int Run(CefMainArgs main_args);

	/// <summary>
	/// Called periodically from a CefTask on the UI thread.
	/// Polls runtime state and pushes updates to the React frontend.
	/// </summary>
	void PollTick();

  private:
	uam::AppState        m_app;
	PlatformServices*    m_platformServices  = nullptr;
	std::unique_ptr<uam::platform::DataRootLock> m_dataRootLock;
	CefRefPtr<CefBrowser> m_browser;
	bool                 m_done              = false;
	int                  m_exitCode          = 0;

	// ---- startup / teardown -----------------------------------------------
	bool InitializeState();
	bool InitializeCef(CefMainArgs main_args);
	void Shutdown();

	// ---- periodic work (posted to CEF UI thread) --------------------------
	void Update();
	void ScheduleNextUpdate(int delay_ms = 1000);

	// ---- CEF ready callback -----------------------------------------------
	void OnBrowserReady(CefRefPtr<CefBrowser> browser);
};

#endif // UAM_APP_APPLICATION_H
