#pragma once

/// <summary>
/// Draws the sidebar header with title, action buttons, and chat count.
/// </summary>
static void DrawChatSidebarHeader(AppState& app) {
  PushFontIfAvailable(g_font_title);
  ImGui::TextColored(ui::kTextPrimary, "Chats");
  PopFontIfAvailable(g_font_title);

  ImGui::SameLine();
  float header_controls_x = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - 52.0f;
#if defined(_WIN32)
  const float header_button_w = ScaleUiLength(20.0f);
  const float header_gap = ScaleUiLength(6.0f);
  const float header_right_pad = ScaleUiLength(4.0f);
  const float header_controls_w = (header_button_w * 2.0f) + header_gap;
  header_controls_x = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - header_controls_w - header_right_pad;
#endif
  if (ImGui::GetCursorScreenPos().x < header_controls_x) {
    ImGui::SetCursorScreenPos(ImVec2(header_controls_x, ImGui::GetCursorScreenPos().y));
  }
  if (DrawMiniIconButton("new_chat_global", "+", ImVec2(20.0f, 20.0f))) {
    CreateAndSelectChatInFolder(app, kDefaultFolderId);
  }
  float header_button_spacing = 6.0f;
#if defined(_WIN32)
  header_button_spacing = ScaleUiLength(6.0f);
#endif
  ImGui::SameLine(0.0f, header_button_spacing);
  if (DrawMiniIconButton("new_folder", "f+", ImVec2(20.0f, 20.0f))) {
    app.pending_move_chat_to_new_folder_id.clear();
    app.new_folder_title_input.clear();
    app.new_folder_directory_input = fs::current_path().string();
    ImGui::OpenPopup("new_folder_popup");
  }

  ImGui::TextColored(ui::kTextMuted, "%zu chats", app.chats.size());
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
  DrawSoftDivider();
}
