#pragma once

/// <summary>
/// Draws the CLI-console center pane for the selected chat.
/// </summary>
static void DrawChatDetailCliConsoleBody(AppState& app, ChatSession& chat) {
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
  BeginPanel("cli_terminal_panel", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true,
             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(8.0f, 8.0f));
  DrawCliTerminalSurface(app, chat, false);
  EndPanel();
  if (!app.status_line.empty()) {
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    ImGui::TextColored(ui::kTextSecondary, "%s", app.status_line.c_str());
  }
}
