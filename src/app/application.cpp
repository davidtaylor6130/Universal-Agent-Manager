#include "application.h"
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <curl/curl.h>

#include "runtime_local_service.h"

#include "common/runtime/provider_cli_compatibility_service.h"

Application::Application()
{
	if (!OnLoad())
	{
		m_done = true;
		m_exitCode = (m_exitCode != 0) ? m_exitCode : 1;
	}
}

Application::~Application()
{
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
	while (!m_done && m_window != nullptr)
	{
		SDL_Event l_event;

		while (SDL_PollEvent(&l_event))
		{
			if (m_terminalSessionManager.ForwardEscapeToSelectedTerminal(m_app, l_event))
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
					PersistWindowStateAndSettings();
				}
			}
		}

		if (!Update())
		{
			break;
		}
	}

	return m_exitCode;
}

bool Application::OnLoad()
{
	m_app = uam::AppState{};
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
		m_terminalSessionManager.FastStopTerminalsForExit(m_app);
		m_terminalsStoppedForShutdown = true;
		return false;
	}

	m_pendingRuntimeCallService.Poll(m_app);
	m_terminalPollingService.PollAllTerminals(m_app);
	GetProviderCliCompatibilityService().Poll(m_app);

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	m_uiController.DrawFrame(m_app,
	                         m_done,
	                         m_platformUiScale,
	                         m_platformServices->ui_traits,
	                         m_shortcutHandler,
	                         m_themeController,
	                         m_mainMenuBarView,
	                         m_sidebarView,
	                         m_chatDetailView,
	                         m_modalHostView);

	ImGui::Render();
	PresentFrame();
	return !m_done;
}

void Application::Shutdown()
{
	PersistWindowStateAndSettings();
	m_app.pending_calls.clear();
	m_app.resolved_native_sessions_by_chat_id.clear();

	if (!m_terminalsStoppedForShutdown)
	{
		m_terminalSessionManager.StopAllTerminals(m_app, true);
		m_terminalsStoppedForShutdown = true;
	}

	GetRuntimeLocalService().StopLocalBridge(m_app);
}
