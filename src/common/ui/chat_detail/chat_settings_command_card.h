#pragma once

/// <summary>
/// Draws the current Gemini command preview card when a call is active.
/// </summary>
static void DrawChatSettingsCommandPreviewCard(AppState& app, ChatSession& chat) {
  if (const PendingGeminiCall* pending = FirstPendingCallForChat(app, chat.id); pending != nullptr) {
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
    DrawSectionHeader("Current Command");
    if (BeginSectionCard("command_preview_card", 128.0f)) {
      PushFontIfAvailable(g_font_mono);
      PushInputChrome();
      std::string command_preview = pending->command_preview;
      ImGui::InputTextMultiline("##cmd_preview", &command_preview, ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
      PopInputChrome();
      PopFontIfAvailable(g_font_mono);
    }
    EndPanel();
  }
}
