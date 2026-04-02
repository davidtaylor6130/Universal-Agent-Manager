#pragma once

/// <summary>
/// Shared geometry and rendering helpers for chat message bubbles.
/// </summary>
struct ChatBubbleLayout
{
	float mf_contentWidth = 0.0f;
	float mf_maxWidth = 0.0f;
	float mf_padX = 16.0f;
	float mf_padY = 14.0f;
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
/// Computes message bubble placement and dimensions for a row.
/// </summary>
static ChatBubbleLayout BuildChatBubbleLayout(const std::string& content, const float content_width, const bool align_right)
{
	ChatBubbleLayout layout;
	layout.mf_contentWidth = content_width;
	layout.mf_maxWidth = content_width * 0.78f;
	layout.mf_textWrapWidth = layout.mf_maxWidth - (layout.mf_padX * 2.0f);

	const ImVec2 text_size = ImGui::CalcTextSize(content.c_str(), nullptr, false, layout.mf_textWrapWidth);
	layout.mf_headerHeight = ImGui::GetTextLineHeight();
	layout.mf_metaHeight = ImGui::GetTextLineHeight();
	layout.mf_bubbleWidth = std::max(220.0f, std::min(layout.mf_maxWidth, text_size.x + layout.mf_padX * 2.0f));
	layout.mf_bubbleHeight = layout.mf_padY + layout.mf_headerHeight + 8.0f + text_size.y + 10.0f + layout.mf_metaHeight + layout.mf_padY;

	layout.m_cursor = ImGui::GetCursorScreenPos();
	const float bubble_x = align_right ? (layout.m_cursor.x + content_width - layout.mf_bubbleWidth) : layout.m_cursor.x;
	layout.m_min = ImVec2(bubble_x, layout.m_cursor.y);
	layout.m_max = ImVec2(bubble_x + layout.mf_bubbleWidth, layout.m_cursor.y + layout.mf_bubbleHeight);
	return layout;
}

/// <summary>
/// Draws the bubble card background and border.
/// </summary>
static void DrawChatBubbleFrame(const ChatBubbleLayout& layout, const ImVec4& background, const ImVec4& border)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(ImVec2(layout.m_min.x + 1.0f, layout.m_min.y + 3.0f), ImVec2(layout.m_max.x + 1.0f, layout.m_max.y + 3.0f), ImGui::GetColorU32(ui::kShadowSoft), 12.0f);
	draw->AddRectFilled(layout.m_min, layout.m_max, ImGui::GetColorU32(background), 12.0f);
	draw->AddRect(layout.m_min, layout.m_max, ImGui::GetColorU32(border), 12.0f, 0, 1.1f);
}

/// <summary>
/// Draws a role label at the top-left of the bubble.
/// </summary>
static void DrawChatBubbleRoleLabel(const ChatBubbleLayout& layout, const char* role_label, const ImVec4& role_color)
{
	ImGui::SetCursorScreenPos(ImVec2(layout.m_min.x + layout.mf_padX, layout.m_min.y + layout.mf_padY));
	ImGui::TextColored(role_color, "%s", role_label);
}

/// <summary>
/// Draws wrapped message content inside the bubble.
/// </summary>
static void DrawChatBubbleContent(const ChatBubbleLayout& layout, const std::string& content)
{
	ImGui::SetCursorScreenPos(ImVec2(layout.m_min.x + layout.mf_padX, layout.m_min.y + layout.mf_padY + layout.mf_headerHeight + 2.0f));
	const float wrap_pos_x = ImGui::GetCursorPosX() + (layout.mf_bubbleWidth - (layout.mf_padX * 2.0f));
	ImGui::PushTextWrapPos(wrap_pos_x);
	ImGui::TextUnformatted(content.c_str());
	ImGui::PopTextWrapPos();
}

/// <summary>
/// Draws message metadata (timestamp) at the bottom-left of the bubble.
/// </summary>
static void DrawChatBubbleTimestamp(const ChatBubbleLayout& layout, const std::string& created_at)
{
	ImGui::SetCursorScreenPos(ImVec2(layout.m_min.x + layout.mf_padX, layout.m_max.y - layout.mf_padY - layout.mf_metaHeight));
	ImGui::TextColored(ui::kTextMuted, "%s", created_at.c_str());
}

/// <summary>
/// Ends the current bubble row and advances cursor to the next row.
/// </summary>
static void EndChatBubbleRow(const ChatBubbleLayout& layout)
{
	ImGui::SetCursorScreenPos(ImVec2(layout.m_cursor.x, layout.m_max.y + ui::kSpace16));
	ImGui::Dummy(ImVec2(layout.mf_contentWidth, 0.0f));
}
