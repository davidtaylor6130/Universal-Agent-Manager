#include "application.h"
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <curl/curl.h>

#include "runtime_local_service.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal_polling.h"
#include "common/ui/chat_actions/chat_action_pending_calls.h"

#include "common/runtime/provider_cli_compatibility_service.h"

#define UAM_TARGET_FPS 30

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
	m_app.pending_calls.clear();
	m_app.resolved_native_sessions_by_chat_id.clear();

	if (!m_terminalsStoppedForShutdown)
	{
		StopAllCliTerminals(m_app, true);
		m_terminalsStoppedForShutdown = true;
	}

	RuntimeLocalService().StopLocalBridge(m_app);
}
