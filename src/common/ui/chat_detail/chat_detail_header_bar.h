#pragma once

/// <summary>
/// Draws the chat title/status header block above the main conversation content.
/// </summary>
static void DrawChatDetailHeaderBar(AppState& app, ChatSession& chat) {
  if (BeginPanel("chat_header_bar", ImVec2(0.0f, 92.0f), PanelTone::Secondary, true, 0, ImVec2(12.0f, 10.0f), ui::kRadiusInput)) {
    if (ImGui::BeginTable("chat_header_layout", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp |
                                                   ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoBordersInBody)) {
      float mode_column_w = 164.0f;
#if defined(_WIN32)
      mode_column_w = ScaleUiLength(164.0f);
#endif
      ImGui::TableSetupColumn("meta", ImGuiTableColumnFlags_WidthStretch, 0.72f);
      ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed, mode_column_w);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      PushInputChrome();
      ImGui::SetNextItemWidth(-1.0f);
      const std::string chat_title_input_id = "##chat_title_" + chat.id;
      if (ImGui::InputText(chat_title_input_id.c_str(), &chat.title)) {
        chat.updated_at = TimestampNow();
        SaveAndUpdateStatus(app, chat, "Chat title updated.", "Chat title changed in UI, but failed to save.");
      }
      PopInputChrome();

      if (HasPendingCallForChat(app, chat.id)) {
        static constexpr const char kSpinnerFrames[4] = {'|', '/', '-', '\\'};
        const int spinner_index = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
        ImGui::TextColored(ui::kWarning, "Provider running %c", kSpinnerFrames[spinner_index]);
      } else if (HasAnyPendingCall(app)) {
        ImGui::TextColored(ui::kTextSecondary, "Provider running in another chat");
      } else {
        ImGui::TextColored(ui::kSuccess, "Ready");
      }
      ImGui::SameLine();
      ImGui::TextColored(ui::kTextMuted, "Updated %s", CompactPreview(chat.updated_at, 20).c_str());

      ImGui::TableSetColumnIndex(1);
      const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);
      const char* mode_label = ProviderRuntime::UsesCliOutput(provider) ? "CLI" : "Structured";
      float mode_y_nudge = 3.0f;
#if defined(_WIN32)
      mode_y_nudge = ScaleUiLength(3.0f);
#endif
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mode_y_nudge);
      ImGui::TextColored(ui::kTextMuted, "Output");
      ImGui::SameLine();
      ImGui::TextColored(ui::kTextPrimary, "%s", mode_label);
      ImGui::TextColored(ui::kTextMuted, "Locked by provider");

      ImGui::EndTable();
    }
  }
  EndPanel();
}
