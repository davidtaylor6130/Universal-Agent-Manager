#pragma once
#include "common/platform/platform_services.h"

/// <summary>
/// UI actions emitted by a sidebar chat row.
/// </summary>
struct SidebarItemAction
{
	bool select = false;
	bool request_delete = false;
	bool request_open_options = false;
};

/// <summary>
/// Draws one chat row in the sidebar tree and returns requested UI actions.
/// </summary>
static SidebarItemAction DrawSidebarItem(AppState& app, const ChatSession& chat, const bool selected, const std::string& item_id, const int tree_depth = 0, const bool has_children = false, const bool children_collapsed = false, bool* toggle_children = nullptr)
{
	const bool light = IsLightPaletteActive();
	SidebarItemAction action;

	ImGui::PushID(item_id.c_str());
	float row_h = 30.0f;
	float row_rounding = ui::kRadiusSmall;
	float accent_w = 3.0f;
	float title_x_offset = 11.0f;
	float title_y_offset = 7.0f;
	float options_x_offset = 42.0f;
	float delete_x_offset = 22.0f;
	float delete_y_offset = 6.0f;
	float row_bottom_gap = 4.0f;
	int title_limit = 46;
	const float depth_indent = static_cast<float>(tree_depth) * ScaleUiLength(14.0f);

	if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
	{
		// Windows-only DPI/layout mitigation: ensure row geometry scales with text so
		// large user scale values do not cause sidebar overlap. If this starts to
		// happen on macOS later, we can make this universal.
		row_h = std::max(ScaleUiLength(30.0f), ImGui::GetTextLineHeight() + ScaleUiLength(12.0f));
		row_rounding = ScaleUiLength(ui::kRadiusSmall);
		accent_w = std::max(2.0f, ScaleUiLength(3.0f));
		title_x_offset = ScaleUiLength(11.0f);
		title_y_offset = (row_h - ImGui::GetTextLineHeight()) * 0.5f;
		options_x_offset = ScaleUiLength(42.0f);
		delete_x_offset = ScaleUiLength(22.0f);
		delete_y_offset = std::max(ScaleUiLength(3.0f), (row_h - ScaleUiLength(16.0f)) * 0.5f);
		row_bottom_gap = ScaleUiLength(4.0f);
	}
	title_x_offset += depth_indent + (has_children ? ScaleUiLength(18.0f) : 0.0f);
	const ImVec2 row_size(ImGui::GetContentRegionAvail().x, row_h);
	const ImVec2 min = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("chat_row", row_size);
	ImGui::SetItemAllowOverlap();
	const bool hovered = ImGui::IsItemHovered();
	action.select = ImGui::IsItemClicked();

	if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
	{
		action.request_open_options = true;
		action.select = false;
	}

	const ImVec2 max(min.x + row_size.x, min.y + row_size.y);

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec4 row_bg = selected ? (light ? Rgb(66, 126, 228, 0.13f) : Rgb(94, 160, 255, 0.15f)) : (hovered ? (light ? Rgb(9, 31, 63, 0.06f) : Rgb(255, 255, 255, 0.06f)) : ui::kTransparent);
	const ImVec4 row_border = selected ? ui::kBorderStrong : ui::kBorder;

	if (selected || hovered)
	{
		draw->AddRectFilled(min, max, ImGui::GetColorU32(row_bg), row_rounding);
		draw->AddRect(min, max, ImGui::GetColorU32(row_border), row_rounding);
	}

	if (selected)
	{
		draw->AddRectFilled(min, ImVec2(min.x + accent_w, max.y), ImGui::GetColorU32(ui::kAccent), row_rounding, ImDrawFlags_RoundCornersLeft);
	}

	if (has_children)
	{
		ImGui::SetCursorScreenPos(ImVec2(min.x + depth_indent + ScaleUiLength(2.0f), min.y + delete_y_offset));
		const char* glyph = children_collapsed ? ">" : "v";

		if (DrawMiniIconButton("branch_toggle", glyph, ImVec2(14.0f, 14.0f), false))
		{
			if (toggle_children != nullptr)
			{
				*toggle_children = true;
			}

			action.select = false;
		}
	}
	else if (tree_depth > 0)
	{
		draw->AddText(ImVec2(min.x + depth_indent + ScaleUiLength(6.0f), min.y + title_y_offset), ImGui::GetColorU32(ui::kTextMuted), ".");
	}

	const bool running = ChatHasRunningGemini(app, chat.id);
	const bool has_unseen = !running && (app.chats_with_unseen_updates.find(chat.id) != app.chats_with_unseen_updates.end());
	const bool showing_actions = (hovered || selected);
	const float indicator_lane_x = showing_actions ? (max.x - options_x_offset - ScaleUiLength(14.0f)) : (max.x - ScaleUiLength(14.0f));
	const float title_right_limit_x = (running || has_unseen) ? (indicator_lane_x - ScaleUiLength(8.0f)) : (showing_actions ? (max.x - options_x_offset - ScaleUiLength(8.0f)) : (max.x - ScaleUiLength(10.0f)));
	const float available_title_w = std::max(44.0f, title_right_limit_x - (min.x + title_x_offset));
	const float avg_char_w = std::max(4.0f, ImGui::CalcTextSize("ABCDEFGHIJKLMNOPQRSTUVWXYZ").x / 26.0f);
	title_limit = std::max(10, static_cast<int>(std::floor(available_title_w / avg_char_w)));

	const std::string row_title = CompactPreview(Trim(chat.title).empty() ? chat.id : chat.title, title_limit);
	draw->AddText(ImVec2(min.x + title_x_offset, min.y + title_y_offset), ImGui::GetColorU32(ui::kTextPrimary), row_title.c_str());

	if (running || has_unseen)
	{
		if (running)
		{
			const float dot_spacing = ScaleUiLength(5.0f);
			const float dot_radius = ScaleUiLength(1.75f);
			const float center_y = min.y + row_h * 0.5f;
			const double t = ImGui::GetTime() * 7.0;

			for (int i = 0; i < 3; ++i)
			{
				ImVec4 dot_color = ui::kWarning;
				const double wave = 0.5 + 0.5 * std::sin(t - static_cast<double>(i) * 0.9);
				dot_color.w *= static_cast<float>(0.22 + (wave * 0.78));
				const float x = indicator_lane_x + (static_cast<float>(i) - 1.0f) * dot_spacing;
				draw->AddCircleFilled(ImVec2(x, center_y), dot_radius, ImGui::GetColorU32(dot_color), 12);
			}
		}
		else
		{
			const bool has_title_font = (g_font_title != nullptr);
			ImFont* glyph_font = has_title_font ? g_font_title : ImGui::GetFont();
			const float glyph_size = has_title_font ? (g_font_title->FontSize * 0.62f) : (ImGui::GetFontSize() * 1.20f);
			const char* unseen_glyph = has_title_font ? "\xE2\x97\x8F" : "@";
			const float indicator_x = indicator_lane_x;
			const float unseen_y = std::max(min.y, min.y + title_y_offset - ScaleUiLength(2.5f));
			draw->AddText(glyph_font, glyph_size, ImVec2(indicator_x, unseen_y), ImGui::GetColorU32(ui::kAccent), unseen_glyph);
		}
	}

	if (hovered || selected)
	{
		ImGui::SetCursorScreenPos(ImVec2(max.x - options_x_offset, min.y + delete_y_offset));

		if (DrawMiniIconButton("chat_options_menu", "icon:menu", ImVec2(16.0f, 16.0f), true))
		{
			action.request_open_options = true;
			action.select = false;
		}

		ImGui::SetCursorScreenPos(ImVec2(max.x - delete_x_offset, min.y + delete_y_offset));

		if (DrawMiniIconButton("delete_chat", "icon:delete", ImVec2(16.0f, 16.0f), true))
		{
			action.request_delete = true;
			action.select = false;
		}
	}

	ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + row_bottom_gap));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
	ImGui::PopID();
	return action;
}
