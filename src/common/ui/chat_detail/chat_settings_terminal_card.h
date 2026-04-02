#pragma once

/// <summary>
/// Draws a live terminal inspector card that reuses the same CLI instance as terminal mode.
/// </summary>
static void DrawChatSettingsTerminalCard(AppState& app, ChatSession& chat)
{
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
	DrawSectionHeader("Live Terminal");

	if (BeginSectionCard("live_terminal_card", 260.0f))
	{
		ImGui::TextColored(ui::kTextMuted, "Uses the same live CLI instance as this chat.");
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
		DrawCliTerminalSurface(app, chat, true);
	}

	EndPanel();
}
