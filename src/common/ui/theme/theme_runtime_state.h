#ifndef UAM_COMMON_UI_THEME_THEME_RUNTIME_STATE_H
#define UAM_COMMON_UI_THEME_THEME_RUNTIME_STATE_H

#include <imgui.h>

extern ImFont* g_font_ui;
extern ImFont* g_font_title;
extern ImFont* g_font_mono;
extern ImGuiStyle g_user_scale_base_style;
extern bool g_user_scale_base_style_ready;
extern float g_last_applied_user_scale;
extern float g_ui_layout_scale;
extern float g_platform_layout_scale;

#endif // UAM_COMMON_UI_THEME_THEME_RUNTIME_STATE_H
