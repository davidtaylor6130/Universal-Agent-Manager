#pragma once

/// <summary>
/// Shared geometry and rendering helpers for chat message rows (flat Codex/T3 style).
/// </summary>
struct ChatBubbleLayout
{
	float mf_contentWidth = 0.0f;
	float mf_maxWidth = 0.0f;
	float mf_padX = 10.0f;
	float mf_padY = 8.0f;
	float mf_textWrapWidth = 0.0f;
	float mf_headerHeight = 0.0f;
	float mf_metaHeight = 0.0f;
	float mf_bubbleWidth = 0.0f;
	float mf_bubbleHeight = 0.0f;
	ImVec2 m_cursor = ImVec2(0.0f, 0.0f);
	ImVec2 m_min = ImVec2(0.0f, 0.0f);
	ImVec2 m_max = ImVec2(0.0f, 0.0f);
};

/// <summary>
/// Computes message row placement and dimensions.
/// </summary>
static ChatBubbleLayout BuildChatBubbleLayout(const std::string& content, const float content_width, const bool align_right)
{
	ChatBubbleLayout layout;
	layout.mf_contentWidth = content_width;
	layout.mf_maxWidth = content_width * 0.78f;
	layout.mf_textWrapWidth = layout.mf_maxWidth - (layout.mf_padX * 2.0f);

	const ImVec2 text_size = ImGui::CalcTextSize(content.c_str(), nullptr, false, layout.mf_textWrapWidth);
	layout.mf_headerHeight = 0.0f; // No header row in flat style
	layout.mf_metaHeight = ImGui::GetTextLineHeight();
	layout.mf_bubbleWidth = std::max(180.0f, std::min(layout.mf_maxWidth, text_size.x + layout.mf_padX * 2.0f));
	layout.mf_bubbleHeight = layout.mf_padY + text_size.y + 8.0f + layout.mf_metaHeight + layout.mf_padY;

	layout.m_cursor = ImGui::GetCursorScreenPos();
	const float bubble_x = align_right ? (layout.m_cursor.x + content_width - layout.mf_bubbleWidth) : layout.m_cursor.x;
	layout.m_min = ImVec2(bubble_x, layout.m_cursor.y);
	layout.m_max = ImVec2(bubble_x + layout.mf_bubbleWidth, layout.m_cursor.y + layout.mf_bubbleHeight);
	return layout;
}

/// <summary>
/// Draws a subtle filled pill for user messages (no shadow, no border).
/// </summary>
static void DrawUserBubblePill(const ChatBubbleLayout& layout)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const bool light = IsLightPaletteActive();
	const ImVec4 fill = light ? Rgb(66, 126, 228, 0.10f) : Rgb(94, 160, 255, 0.10f);
	draw->AddRectFilled(layout.m_min, layout.m_max, ImGui::GetColorU32(fill), 10.0f);
}

/// <summary>
/// Draws wrapped message content inside the row.
/// </summary>
static void DrawChatBubbleContent(const ChatBubbleLayout& layout, const std::string& content)
{
	ImGui::SetCursorScreenPos(ImVec2(layout.m_min.x + layout.mf_padX, layout.m_min.y + layout.mf_padY));
	const float wrap_pos_x = ImGui::GetCursorPosX() + (layout.mf_bubbleWidth - (layout.mf_padX * 2.0f));
	ImGui::PushTextWrapPos(wrap_pos_x);
	ImGui::TextUnformatted(content.c_str());
	ImGui::PopTextWrapPos();
}

/// <summary>
/// Draws a muted timestamp below the content.
/// </summary>
static void DrawChatBubbleTimestamp(const ChatBubbleLayout& layout, const std::string& created_at, const bool right_align = false)
{
	const ImVec2 ts_size = ImGui::CalcTextSize(created_at.c_str());
	const float ts_x = right_align ? (layout.m_max.x - layout.mf_padX - ts_size.x) : (layout.m_min.x + layout.mf_padX);
	ImGui::SetCursorScreenPos(ImVec2(ts_x, layout.m_max.y - layout.mf_padY - layout.mf_metaHeight));
	ImGui::TextColored(ui::kTextMuted, "%s", created_at.c_str());
}

/// <summary>
/// Ends the current message row and advances cursor to the next row.
/// </summary>
static void EndChatBubbleRow(const ChatBubbleLayout& layout, const float extra_bottom = 0.0f)
{
	ImGui::SetCursorScreenPos(ImVec2(layout.m_cursor.x, layout.m_max.y + extra_bottom + ui::kSpace16));
	ImGui::Dummy(ImVec2(layout.mf_contentWidth, 0.0f));
}
