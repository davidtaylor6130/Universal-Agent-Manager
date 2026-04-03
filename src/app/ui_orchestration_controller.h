#ifndef UAM_APP_UI_ORCHESTRATION_CONTROLLER_H
#define UAM_APP_UI_ORCHESTRATION_CONTROLLER_H


#include "common/platform/platform_services.h"
#include "common/state/app_state.h"

#include <imgui.h>

class MainMenuBarView
{
  public:
	void Draw(uam::AppState& p_app, bool& p_done) const;
};

class SidebarView
{
  public:
	void Draw(uam::AppState& p_app) const;
};

class ChatDetailView
{
  public:
	void Draw(uam::AppState& p_app, ChatSession* p_selectedChat) const;
};

class ModalHostView
{
  public:
	void Draw(uam::AppState& p_app, float p_platformUiScale) const;
};

class ShortcutHandler
{
  public:
	void Handle(uam::AppState& p_app) const;
};

class ThemeController
{
  public:
	void ApplyFromSettings(uam::AppState& p_app) const;
	void CaptureScaleBaseStyle() const;
	void ApplyUserScale(ImGuiIO& p_io, float p_userScaleMultiplier) const;
	float EffectiveScale() const;
};

class UiController
{
  public:
	void DrawFrame(uam::AppState& p_app,
	               bool& p_done,
	               float p_platformUiScale,
	               const IPlatformUiTraits& p_uiTraits,
	               const ShortcutHandler& p_shortcuts,
	               const ThemeController& p_theme,
	               const MainMenuBarView& p_menuBar,
	               const SidebarView& p_sidebar,
	               const ChatDetailView& p_chatDetail,
	               const ModalHostView& p_modalHost) const;
};

#endif // UAM_APP_UI_ORCHESTRATION_CONTROLLER_H
