#pragma once

#include "common/ui/chat_bubbles/chat_bubble_common.h"

/// <summary>
/// Draws a flat right-aligned user message row with a subtle pill background.
/// Action buttons (Branch, Edit) are placed BELOW the pill to avoid text overlap.
/// </summary>
static void DrawUserMessageBubble(AppState& app, ChatSession& chat, const int message_index, const Message& message, const ChatBubbleLayout& layout)
{
	DrawUserBubblePill(layout);
	DrawChatBubbleContent(layout, message.content);
	DrawChatBubbleTimestamp(layout, message.created_at, true);

	// Branch + Edit buttons sit below the pill — no text overlap
	const float btn_y = layout.m_max.y + 3.0f;

	ImGui::SetCursorScreenPos(ImVec2(layout.m_max.x - 152.0f, btn_y));

	if (DrawButton("Branch", ImVec2(68.0f, 20.0f), ButtonKind::Ghost))
	{
		app.pending_branch_chat_id = chat.id;
		app.pending_branch_message_index = message_index;
	}

	ImGui::SetCursorScreenPos(ImVec2(layout.m_max.x - 78.0f, btn_y));

	if (FrontendActionVisible(app, "edit_resubmit", true) && DrawButton("Edit", ImVec2(68.0f, 20.0f), ButtonKind::Ghost))
	{
		BeginEditMessage(app, chat, message_index);
	}
}
