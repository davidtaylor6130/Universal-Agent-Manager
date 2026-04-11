#pragma once

#include "common/ui/chat_detail/chat_detail_cli_panel.h"
#include "common/ui/chat_detail/chat_detail_header_bar.h"

/// <summary>
/// Draws the center chat detail pane for the Gemini CLI slice.
/// </summary>
inline void DrawChatDetailPane(AppState& app, ChatSession& chat)
{
	MarkSelectedChatSeen(app);
	BeginPanel("main_chat_panel", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(ui::kSpace16, ui::kSpace16));

	DrawChatDetailHeaderBar(app, chat);
	DrawChatDetailCliConsoleBody(app, chat);

	EndPanel();
}
