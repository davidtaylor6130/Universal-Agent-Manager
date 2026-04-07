#include "common/ui/theme/theme_runtime_state.h"

ImFont* g_font_ui = nullptr;
ImFont* g_font_title = nullptr;
ImFont* g_font_mono = nullptr;
ImGuiStyle g_user_scale_base_style;
bool g_user_scale_base_style_ready = false;
float g_last_applied_user_scale = -1.0f;
float g_ui_layout_scale = 1.0f;
float g_platform_layout_scale = 1.0f;
