#pragma once

/// <summary>
/// Repository command output modal renderer.
/// </summary>
static void DrawVcsOutputModal(AppState& app) {
  if (app.open_vcs_output_popup) {
    ImGui::OpenPopup("vcs_output_popup");
    app.open_vcs_output_popup = false;
  }
  if (!ImGui::BeginPopupModal("vcs_output_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  ImGui::TextColored(ui::kTextPrimary, "%s", app.vcs_output_popup_title.empty() ? "Repository Output" : app.vcs_output_popup_title.c_str());
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  PushFontIfAvailable(g_font_mono);
  PushInputChrome();
  std::string output_text = app.vcs_output_popup_content;
  ImGui::InputTextMultiline("##vcs_output_text",
                            &output_text,
                            ImVec2(760.0f, 420.0f),
                            ImGuiInputTextFlags_ReadOnly);
  PopInputChrome();
  PopFontIfAvailable(g_font_mono);
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
  if (DrawButton("Close", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}
