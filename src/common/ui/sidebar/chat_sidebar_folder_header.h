#pragma once
#include <algorithm>
#include "app/chat_domain_service.h"
#include "common/platform/platform_services.h"

/// <summary>
/// UI actions emitted by a sidebar folder header row.
/// </summary>
struct FolderHeaderAction
{
	bool toggle = false;
};

/// <summary>
/// Draws the folder header row with count and quick actions.
/// </summary>
inline FolderHeaderAction DrawFolderHeaderItem(const ChatFolder& folder, const int chat_count)
{
	const bool light = IsLightPaletteActive();
	FolderHeaderAction action;

	ImGui::PushID(folder.id.c_str());
	float row_h = 30.0f;
	float row_rounding = ui::kRadiusSmall;
	float marker_x_offset = 8.0f;
	float title_x_offset = 22.0f;
	float text_y_offset = 7.0f;
	float count_x_offset = 74.0f;
	float row_bottom_gap = 4.0f;
	int title_limit = 22;

	if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
	{
		// Windows-only DPI/layout mitigation for folder rows. Keeps controls and
		// count text from colliding when text scale is increased.
		row_h = std::max(ScaleUiLength(30.0f), ImGui::GetTextLineHeight() + ScaleUiLength(12.0f));
		row_rounding = ScaleUiLength(ui::kRadiusSmall);
		marker_x_offset = ScaleUiLength(8.0f);
		title_x_offset = ScaleUiLength(22.0f);
		text_y_offset = (row_h - ImGui::GetTextLineHeight()) * 0.5f;
		row_bottom_gap = ScaleUiLength(4.0f);
	}
	const ImVec2 row_size(ImGui::GetContentRegionAvail().x, row_h);
	const ImVec2 min = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("folder_row", row_size);
	ImGui::SetItemAllowOverlap();
	const bool hovered = ImGui::IsItemHovered();
	action.toggle = ImGui::IsItemClicked();
	const ImVec2 max_pos(min.x + row_size.x, min.y + row_size.y);

	ImDrawList* draw = ImGui::GetWindowDrawList();

	const ImU32 bg_color = (hovered || !folder.collapsed) 
		? ImGui::GetColorU32(light ? Rgb(9, 31, 63, 0.08f) : Rgb(255, 255, 255, 0.07f))
		: ImGui::GetColorU32(light ? Rgb(9, 31, 63, 0.04f) : Rgb(255, 255, 255, 0.03f));

	draw->AddRectFilled(min, max_pos, bg_color, row_rounding);
	draw->AddRect(min, max_pos, ImGui::GetColorU32(ui::kBorder), row_rounding);

	const std::string marker = folder.collapsed ? ">" : "v";
	const std::string title = ChatDomainService().FolderTitleOrFallback(folder);
	const std::string count_text = std::to_string(chat_count);
	float count_x = max_pos.x - count_x_offset;

	if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
	{
		const ImVec2 count_size = ImGui::CalcTextSize(count_text.c_str());
		count_x = max_pos.x - ScaleUiLength(12.0f) - count_size.x;
		const float avg_char_w = std::max(4.0f, ImGui::CalcTextSize("ABCDEFGHIJKLMNOPQRSTUVWXYZ").x / 26.0f);
		const float available_title_w = std::max(40.0f, count_x - (min.x + title_x_offset) - ScaleUiLength(8.0f));
		title_limit = std::max(8, static_cast<int>(std::floor(available_title_w / avg_char_w)));
	}
	draw->AddText(ImVec2(min.x + marker_x_offset, min.y + text_y_offset), ImGui::GetColorU32(ui::kTextMuted), marker.c_str());
	draw->AddText(ImVec2(min.x + title_x_offset, min.y + text_y_offset), ImGui::GetColorU32(ui::kTextSecondary), CompactPreview(title, title_limit).c_str());
	draw->AddText(ImVec2(count_x, min.y + text_y_offset), ImGui::GetColorU32(ui::kTextMuted), count_text.c_str());

	ImGui::SetCursorScreenPos(ImVec2(min.x, max_pos.y + row_bottom_gap));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
	ImGui::PopID();
	return action;
}
