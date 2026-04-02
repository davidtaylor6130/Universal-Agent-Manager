#pragma once

#include "common/ui/chat_bubbles/chat_bubble_common.h"

/// <summary>
/// Draws a flat system message row with a subtle warning-tinted pill background.
/// </summary>
static void DrawSystemMessageBubble(const Message& message, const ChatBubbleLayout& layout)
{
	// Subtle warning-tinted pill for system messages
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const bool light = IsLightPaletteActive();
	const ImVec4 fill = light ? Rgb(245, 158, 11, 0.08f) : Rgb(245, 158, 11, 0.10f);
	draw->AddRectFilled(layout.m_min, layout.m_max, ImGui::GetColorU32(fill), 10.0f);

	// Small warning dot indicator
	const float dot_r = 3.5f;
	const float dot_cx = layout.m_min.x + dot_r + 2.0f;
	const float dot_cy = layout.m_min.y + layout.mf_padY + ImGui::GetTextLineHeight() * 0.5f;
	draw->AddCircleFilled(ImVec2(dot_cx, dot_cy), dot_r, ImGui::GetColorU32(ui::kWarning), 12);

	DrawChatBubbleContent(layout, message.content);
	DrawChatBubbleTimestamp(layout, message.created_at, false);
}
