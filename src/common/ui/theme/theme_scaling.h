#pragma once

/// <summary>
/// Platform/UI scaling helpers and runtime scale application.
/// </summary>
static float DetectUiScale(SDL_Window* window) {
#if defined(__APPLE__)
  // macOS windows use logical points (not raw pixels), so additional
  // DPI-based scaling causes the UI to be oversized on Retina displays.
  (void)window;
  return 1.0f;
#else
  int display_index = 0;
  if (window != nullptr) {
    const int window_display = SDL_GetWindowDisplayIndex(window);
    if (window_display >= 0) {
      display_index = window_display;
    }
  }

  float ddpi = 96.0f;
  float hdpi = 96.0f;
  float vdpi = 96.0f;
  if (SDL_GetDisplayDPI(display_index, &ddpi, &hdpi, &vdpi) == 0 && ddpi > 0.0f) {
    return std::clamp(ddpi / 96.0f, 1.0f, 2.25f);
  }
  return 1.0f;
#endif
}

static float PlatformUiSpacingScale() {
#if defined(_WIN32)
  return 1.14f;
#else
  return 1.0f;
#endif
}

static float EffectiveUiScale() {
  return g_ui_layout_scale * g_platform_layout_scale;
}

static float ScaleUiLength(const float value) {
  return value * EffectiveUiScale();
}

static ImVec2 ScaleUiSize(const ImVec2& value) {
  const float scale = EffectiveUiScale();
  const float scaled_x = (value.x > 0.0f) ? (value.x * scale) : value.x;
  const float scaled_y = (value.y > 0.0f) ? (value.y * scale) : value.y;
  return ImVec2(scaled_x, scaled_y);
}

static void CaptureUiScaleBaseStyle() {
  g_user_scale_base_style = ImGui::GetStyle();
  g_user_scale_base_style_ready = true;
  g_last_applied_user_scale = -1.0f;
}

static void ApplyUserUiScale(ImGuiIO& io, float user_scale_multiplier) {
  const float clamped = std::clamp(user_scale_multiplier, 0.85f, 1.75f);
  g_ui_layout_scale = clamped;
  if (!g_user_scale_base_style_ready) {
    CaptureUiScaleBaseStyle();
  }
  if (std::fabs(g_last_applied_user_scale - clamped) > 0.0001f) {
    ImGuiStyle scaled = g_user_scale_base_style;
    scaled.ScaleAllSizes(clamped);
    ImGui::GetStyle() = scaled;
    g_last_applied_user_scale = clamped;
  }
  io.FontGlobalScale = clamped;
}
