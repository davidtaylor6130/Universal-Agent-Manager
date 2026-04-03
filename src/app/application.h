#ifndef UAM_APP_APPLICATION_H
#define UAM_APP_APPLICATION_H


#include "app/ui_orchestration_controller.h"

#include "common/platform/platform_services.h"
#include "common/platform/sdl_includes.h"
#include "common/state/app_state.h"

class Application
{
  public:
	Application();
	~Application();
	Application(const Application&) = delete;
	Application& operator=(const Application&) = delete;
	Application(Application&&) = delete;
	Application& operator=(Application&&) = delete;

	int Run();

 private:
	uam::AppState m_app;
	ChatDetailView m_chatDetailView;
	ModalHostView m_modalHostView;
	UiController m_uiController;
	PlatformServices* m_platformServices = nullptr;
	SDL_Window* m_window = nullptr;
	SDL_GLContext m_glContext = nullptr;
	const char* m_glslVersion = nullptr;
	float m_platformUiScale = 1.0f;
	bool m_done = false;
	bool m_terminalsStoppedForShutdown = false;
	bool m_sdlInitialized = false;
	bool m_imguiInitialized = false;
	bool m_curlInitialized = false;
	int m_exitCode = 0;

	bool OnLoad();
	bool Update();
	void Shutdown();
	bool InitializeState();
	bool InitializeWindowAndUi();
	void PersistWindowStateAndSettings();
	void PresentFrame();
};

#endif // UAM_APP_APPLICATION_H
