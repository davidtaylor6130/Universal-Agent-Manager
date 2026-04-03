#pragma once

#include "common/ui/chat_bubbles/chat_bubble_common.h"

/// <summary>
/// Draws a flat left-aligned assistant message row with no background — just a small dot indicator.
/// </summary>
inline void DrawAssistantMessageBubble(const Message& message, const ChatBubbleLayout& layout)
{
	// Small colored dot indicator instead of a role label/card
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float dot_r = 3.5f;
	const float dot_cx = layout.m_min.x + dot_r + 2.0f;
	const float dot_cy = layout.m_min.y + layout.mf_padY + ImGui::GetTextLineHeight() * 0.5f;
	draw->AddCircleFilled(ImVec2(dot_cx, dot_cy), dot_r, ImGui::GetColorU32(ui::kSuccess), 12);

	DrawChatBubbleContent(layout, message.content);
	DrawChatBubbleTimestamp(layout, message.created_at, false);
}
