#pragma once

/// <summary>
/// Template-change warning modal renderer for started chats.
/// </summary>
static void DrawTemplateChangeWarningModal(AppState& app) {
  if (app.open_template_change_warning_popup) {
    ImGui::OpenPopup("template_change_warning_popup");
    app.open_template_change_warning_popup = false;
  }
  if (ImGui::BeginPopupModal("template_change_warning_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const int chat_index = FindChatIndexById(app, app.pending_template_change_chat_id);
    const bool has_chat = (chat_index >= 0);
    const std::string chat_title = has_chat ? CompactPreview(app.chats[chat_index].title, 42) : "Unknown chat";
    const std::string new_template_label = app.pending_template_change_override_id.empty()
                                               ? "Use global default"
                                               : TemplateLabelOrFallback(app, app.pending_template_change_override_id);

    ImGui::TextColored(ui::kTextPrimary, "Apply template change?");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
    ImGui::TextWrapped("This chat already has messages. Template changes may not fully affect prior context.");
    ImGui::TextColored(ui::kTextMuted, "Chat: %s", chat_title.c_str());
    ImGui::TextColored(ui::kTextMuted, "New template: %s", new_template_label.c_str());
    ImGui::TextColored(ui::kTextMuted, "On confirm, Gemini will receive: @.gemini/gemini.md");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));

    if (DrawButton("Apply + Send", ImVec2(126.0f, 32.0f), ButtonKind::Primary)) {
      if (has_chat) {
        ApplyChatTemplateOverride(app, app.chats[chat_index], app.pending_template_change_override_id, true);
      } else {
        app.status_line = "Chat no longer exists.";
      }
      ClearPendingTemplateChange(app);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
      ClearPendingTemplateChange(app);
      app.status_line = "Template change cancelled.";
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}
