#ifndef UAM_COMMON_UI_MODALS_MODAL_WINDOW_STATE_H
#define UAM_COMMON_UI_MODALS_MODAL_WINDOW_STATE_H

#include "common/state/app_state.h"
#include "common/platform/sdl_includes.h"

#include <algorithm>

/// <summary>
/// Window bounds and scale clamping helpers used by app settings and shutdown capture.
/// </summary>
inline void ClampWindowSettings(AppSettings& settings)
{
	settings.window_width = std::clamp(settings.window_width, 960, 8192);
	settings.window_height = std::clamp(settings.window_height, 620, 8192);
	settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
}

inline void CaptureWindowState(uam::AppState& app, SDL_Window* window)
{
	if (window == nullptr)
	{
		return;
	}

	int width = app.settings.window_width;
	int height = app.settings.window_height;
	SDL_GetWindowSize(window, &width, &height);
	app.settings.window_width = width;
	app.settings.window_height = height;
	app.settings.window_maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
	ClampWindowSettings(app.settings);
}

#endif // UAM_COMMON_UI_MODALS_MODAL_WINDOW_STATE_H
