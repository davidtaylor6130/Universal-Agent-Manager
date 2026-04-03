#ifndef UAM_APP_UI_ORCHESTRATION_CONTROLLER_H
#define UAM_APP_UI_ORCHESTRATION_CONTROLLER_H


#include "common/platform/platform_services.h"
#include "common/state/app_state.h"

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

class UiController
{
  public:
	void DrawFrame(uam::AppState& p_app,
	               bool& p_done,
	               float p_platformUiScale,
	               const IPlatformUiTraits& p_uiTraits,
	               const ChatDetailView& p_chatDetail,
	               const ModalHostView& p_modalHost) const;
};

#endif // UAM_APP_UI_ORCHESTRATION_CONTROLLER_H
