#ifndef UAM_COMMON_UI_MODALS_MODAL_WINDOW_STATE_H
#define UAM_COMMON_UI_MODALS_MODAL_WINDOW_STATE_H

#include "common/state/app_state.h"

#include <algorithm>

/// <summary>
/// Window bounds and scale clamping helpers used by app settings and shutdown capture.
/// SDL/window-specific capture removed — window management now handled by CEF.
/// </summary>
inline void ClampWindowSettings(AppSettings& settings)
{
	settings.window_width = std::clamp(settings.window_width, 960, 8192);
	settings.window_height = std::clamp(settings.window_height, 620, 8192);
	settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
}

#endif // UAM_COMMON_UI_MODALS_MODAL_WINDOW_STATE_H
