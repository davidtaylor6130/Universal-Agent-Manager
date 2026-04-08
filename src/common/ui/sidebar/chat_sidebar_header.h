#ifndef UAM_COMMON_UI_SIDEBAR_CHAT_SIDEBAR_HEADER_H
#define UAM_COMMON_UI_SIDEBAR_CHAT_SIDEBAR_HEADER_H

#include "common/constants/app_constants.h"
#include "common/platform/platform_services.h"

#include <filesystem>

/// <summary>
/// Draws the sidebar header: "Chats · N" title with compact icon buttons on the right.
/// </summary>
inline void DrawChatSidebarHeader(AppState& app)
{
	const float btn_w = 22.0f;
	const float btn_gap = 6.0f;
	const float controls_w = ScaleUiLength(btn_w * 2.0f + btn_gap);

	PushFontIfAvailable(g_font_title);
	ImGui::TextColored(ui::kTextPrimary, "Chats");
	PopFontIfAvailable(g_font_title);
	ImGui::SameLine(0.0f, 6.0f);
	ImGui::TextColored(ui::kTextMuted, "\xc2\xb7 %zu", app.chats.size());

	// Icon buttons on the SAME line, right-aligned relative to the available width.
	// Using GetContentRegionAvail instead of Max for better accuracy in padded panels.
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - controls_w + ImGui::GetCursorPosX());

	if (DrawMiniIconButton("new_chat_global", "icon:new_chat", ImVec2(btn_w, btn_w)))
	{
		CreateAndSelectChatInFolder(app, uam::constants::kDefaultFolderId);
	}

	ImGui::SameLine(0.0f, ScaleUiLength(btn_gap));

	if (DrawMiniIconButton("new_folder", "icon:new_folder", ImVec2(btn_w, btn_w)))
	{
		app.pending_move_chat_to_new_folder_id.clear();
		app.new_folder_title_input.clear();
		app.new_folder_directory_input = std::filesystem::current_path().string();
		ImGui::OpenPopup("new_folder_popup");
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	DrawSoftDivider();
}

#endif // UAM_COMMON_UI_SIDEBAR_CHAT_SIDEBAR_HEADER_H
