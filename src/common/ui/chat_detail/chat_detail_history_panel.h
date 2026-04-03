#pragma once

/// <summary>
/// Draws the structured conversation history list and bottom-scroll behavior.
/// </summary>
inline void DrawChatDetailConversationHistory(AppState& app, ChatSession& chat)
{
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
	// Reserve exactly the composer height (162px) plus one item-spacing gap so the
	// composer renders flush against the bottom with no wasted space.
	const float input_area_h = 162.0f + ImGui::GetStyle().ItemSpacing.y;
	BeginPanel("conversation_history", ImVec2(0.0f, -input_area_h), PanelTone::Primary, false, ImGuiWindowFlags_AlwaysVerticalScrollbar, ImVec2(2.0f, 2.0f), ui::kRadiusSmall);
	const float content_width = ImGui::GetContentRegionAvail().x;

	for (int i = 0; i < static_cast<int>(chat.messages.size()); ++i)
	{
		ImGui::PushID(i);
		DrawMessageBubble(app, chat, i, content_width);
		ImGui::PopID();
	}

	if (app.scroll_to_bottom)
	{
		ImGui::SetScrollHereY(1.0f);
		app.scroll_to_bottom = false;
	}

	EndPanel();
}
