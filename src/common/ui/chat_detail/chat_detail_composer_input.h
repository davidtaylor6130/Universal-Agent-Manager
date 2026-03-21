#pragma once

/// <summary>
/// Draws the structured composer container and send actions.
/// </summary>
static void DrawInputContainer(AppState& app, const ChatSession& chat) {
  BeginPanel("input_container", ImVec2(0.0f, 166.0f), PanelTone::Secondary, true, 0, ImVec2(12.0f, 12.0f), ui::kRadiusInput);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 min = ImGui::GetWindowPos();
  const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
  draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 4.0f), ImVec2(max.x + 2.0f, max.y + 4.0f), ImGui::GetColorU32(ui::kShadowSoft), ui::kRadiusInput);

  ImGui::TextColored(ui::kTextSecondary, "Composer");
  ImGui::SameLine();
  ImGui::TextColored(ui::kTextMuted, "Ctrl+Enter to send");
  PushInputChrome(ui::kRadiusInput);
  const bool send_visible = FrontendActionVisible(app, "send_prompt", true);
#if defined(_WIN32)
  // Windows-only composer fit: reserve right-side width for the SEND button so
  // the multiline input shrinks first at larger UI scales. If this appears on
  // macOS later, we can move this logic to all platforms.
  const float send_gap = ScaleUiLength(8.0f);
  const float send_button_w = ScaleUiLength(80.0f);
  const float composer_h = std::max(ScaleUiLength(110.0f), ImGui::GetTextLineHeight() * 5.2f);
  const float reserved_send_w = send_visible ? (send_button_w + send_gap + ScaleUiLength(2.0f)) : 0.0f;
  const float input_w = std::max(ScaleUiLength(180.0f), ImGui::GetContentRegionAvail().x - reserved_send_w);
  ImGui::InputTextMultiline("##composer", &app.composer_text, ImVec2(input_w, composer_h), ImGuiInputTextFlags_AllowTabInput);
#else
  ImGui::InputTextMultiline("##composer", &app.composer_text, ImVec2(-96.0f, 110.0f), ImGuiInputTextFlags_AllowTabInput);
#endif
  PopInputChrome();

  float send_same_line_gap = 0.0f;
#if defined(_WIN32)
  send_same_line_gap = ScaleUiLength(8.0f);
#endif
  ImGui::SameLine(0.0f, send_same_line_gap);
  const bool can_send = !HasPendingCallForChat(app, chat.id);
  if (!can_send) {
    ImGui::BeginDisabled();
  }
  if (send_visible) {
    float send_y_nudge = 36.0f;
#if defined(_WIN32)
    const float composer_h = std::max(ScaleUiLength(110.0f), ImGui::GetTextLineHeight() * 5.2f);
    const float send_h = ScaleUiLength(42.0f);
    send_y_nudge = std::max(0.0f, (composer_h - send_h) * 0.5f);
#endif
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + send_y_nudge);
    if (DrawButton("SEND", ImVec2(80.0f, 42.0f), ButtonKind::Accent)) {
      StartGeminiRequest(app);
    }
  }
  if (!can_send) {
    ImGui::EndDisabled();
  }

  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      ImGui::IsKeyPressed(ImGuiKey_Enter) &&
      ImGui::GetIO().KeyCtrl &&
      !HasPendingCallForChat(app, chat.id)) {
    StartGeminiRequest(app);
  }
  EndPanel();
}
