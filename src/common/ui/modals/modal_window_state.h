#pragma once
#include "common/platform/sdl_includes.h"

/// <summary>
/// Window bounds and scale clamping helpers used by app settings and shutdown capture.
/// </summary>
static void ClampWindowSettings(AppSettings& settings)
{
	settings.window_width = std::clamp(settings.window_width, 960, 8192);
	settings.window_height = std::clamp(settings.window_height, 620, 8192);
	settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
}

static void CaptureWindowState(AppState& app, SDL_Window* window)
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
