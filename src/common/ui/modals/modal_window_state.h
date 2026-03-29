#pragma once

/// <summary>
/// Window bounds and scale clamping helpers used by app settings and shutdown capture.
/// </summary>
static void ClampWindowSettings(AppSettings& settings) {
  settings.window_width = std::clamp(settings.window_width, 960, 8192);
  settings.window_height = std::clamp(settings.window_height, 620, 8192);
  settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
  settings.cli_max_columns = std::clamp(settings.cli_max_columns, 40, 512);
}

static void CaptureWindowState(AppState& app, SDL_Window* window) {
  if (window == nullptr) {
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

static void PrepareCenteredModal(const ImVec2& desired_size = ImVec2(0.0f, 0.0f),
                                 const ImGuiCond size_condition = ImGuiCond_Appearing) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  if (viewport == nullptr) {
    return;
  }
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x * 0.5f),
                                 viewport->WorkPos.y + (viewport->WorkSize.y * 0.5f)),
                          ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  if (desired_size.x > 0.0f && desired_size.y > 0.0f) {
    ImGui::SetNextWindowSize(ScaleUiSize(desired_size), size_condition);
  }
}

static bool BeginCenteredPopupModal(const char* label,
                                    bool* open = nullptr,
                                    const ImGuiWindowFlags flags = 0,
                                    const ImVec2& desired_size = ImVec2(0.0f, 0.0f),
                                    const ImGuiCond size_condition = ImGuiCond_Appearing) {
  PrepareCenteredModal(desired_size, size_condition);
  return ImGui::BeginPopupModal(label, open, flags);
}
