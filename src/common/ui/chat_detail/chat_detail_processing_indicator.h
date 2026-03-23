#pragma once

/// <summary>
/// Draws the structured-mode processing indicator while a provider call is running.
/// </summary>
static void DrawStructuredProcessingIndicator(const AppState& app, const ChatSession& chat) {
  if (!HasPendingCallForChat(app, chat.id)) {
    return;
  }

  BeginPanel("structured_processing_indicator", ImVec2(0.0f, 72.0f), PanelTone::Secondary, true, 0,
             ImVec2(ui::kSpace12, 10.0f), ui::kRadiusPanel);

  const int dots = static_cast<int>(ImGui::GetTime() * 2.5) % 4;
  std::string status = "Provider is processing";
  status.append(static_cast<std::size_t>(dots), '.');
  ImGui::TextColored(ui::kAccent, "%s", status.c_str());
  ImGui::TextColored(ui::kTextMuted, "You can keep browsing chats while this runs.");

  const ImVec2 bar_min = ImGui::GetCursorScreenPos();
  const float bar_w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
  const float bar_h = 6.0f;
  const ImVec2 bar_max(bar_min.x + bar_w, bar_min.y + bar_h);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(bar_min, bar_max, ImGui::GetColorU32(IsLightPaletteActive() ? Rgb(12, 30, 58, 0.12f) : Rgb(255, 255, 255, 0.10f)), 6.0f);

  const float cycle = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.9f, 1.0f);
  const float seg_w = bar_w * 0.28f;
  const float seg_x = bar_min.x + (bar_w + seg_w) * cycle - seg_w;
  const ImVec2 seg_min(std::max(bar_min.x, seg_x), bar_min.y);
  const ImVec2 seg_max(std::min(bar_max.x, seg_x + seg_w), bar_max.y);
  if (seg_max.x > seg_min.x) {
    draw->AddRectFilled(seg_min, seg_max, ImGui::GetColorU32(ui::kAccent), 6.0f);
  }
  ImGui::Dummy(ImVec2(0.0f, bar_h + 2.0f));
  EndPanel();
}
