#pragma once

#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"

/// <summary>
/// Status chip and global top-bar rendering helpers.
/// </summary>
inline void DrawStatusChip(const std::string& chip_id, const std::string& label, const ImVec4& fill, const ImVec4& text_color)
{
	ImGui::PushID(chip_id.c_str());
	const ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
	const ImVec2 chip_size(text_size.x + ScaleUiLength(16.0f), text_size.y + ScaleUiLength(8.0f));
	const float chip_rounding = ScaleUiLength(ui::kRadiusSmall);
	const ImVec2 min = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("chip", chip_size);
	const ImVec2 max(min.x + chip_size.x, min.y + chip_size.y);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), chip_rounding);
	draw->AddRect(min, max, ImGui::GetColorU32(ui::kBorder), chip_rounding);
	draw->AddText(ImVec2(min.x + ScaleUiLength(8.0f), min.y + ScaleUiLength(4.0f)), ImGui::GetColorU32(text_color), label.c_str());
	ImGui::PopID();
}

inline void DrawGlobalTopBar(AppState& app)
{
	if (!BeginPanel("global_top_bar", ImVec2(0.0f, ui::kTopBarHeight), PanelTone::Secondary, true, 0, ImVec2(ui::kSpace16, ui::kSpace12), ui::kRadiusPanel))
	{
		EndPanel();
		return;
	}

	const ChatSession* selected = ChatDomainService().SelectedChat(app);
	const ProviderProfile* active_provider = nullptr;

	if (selected != nullptr)
	{
		active_provider = ProviderResolutionService().ProviderForChat(app, *selected);
	}

	if (active_provider == nullptr)
	{
		active_provider = ProviderResolutionService().ActiveProvider(app);
	}

	std::string provider_label = "No Provider";

	if (active_provider != nullptr)
	{
		provider_label = Trim(active_provider->title).empty() ? active_provider->id : active_provider->title;
	}

	provider_label = CompactPreview(provider_label, 28);
	const std::string output_mode_label = (active_provider != nullptr && ProviderRuntime::UsesCliOutput(*active_provider)) ? "CLI" : "Structured";

	const bool pending_any = HasAnyPendingCall(app);
	const bool pending_here = (selected != nullptr) && HasPendingCallForChat(app, selected->id);
	const bool pending_elsewhere = pending_any && !pending_here;
	const std::string runtime_label = pending_here ? "Running in current chat" : (pending_elsewhere ? "Running in another chat" : "Idle");

	if (ImGui::BeginTable("global_top_layout", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoBordersInBody))
	{
		ImGui::TableSetupColumn("meta", ImGuiTableColumnFlags_WidthStretch, 0.72f);
		ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, ScaleUiLength(318.0f));
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		PushFontIfAvailable(g_font_title);
		ImGui::TextColored(ui::kTextPrimary, "Universal Agent Manager");
		PopFontIfAvailable(g_font_title);
		ImGui::SameLine();
		ImGui::TextColored(ui::kTextMuted, "Codex Workspace");

		DrawStatusChip("provider_chip", "Provider: " + provider_label, IsLightPaletteActive() ? Rgb(9, 31, 63, 0.08f) : Rgb(255, 255, 255, 0.06f), ui::kTextSecondary);
		ImGui::SameLine();
		DrawStatusChip("output_chip", "Output: " + output_mode_label, IsLightPaletteActive() ? Rgb(9, 31, 63, 0.08f) : Rgb(255, 255, 255, 0.06f), ui::kTextSecondary);
		ImGui::SameLine();
		DrawStatusChip("runtime_chip", "Runtime: " + runtime_label, pending_here ? Rgb(245, 158, 11, IsLightPaletteActive() ? 0.18f : 0.22f) : (pending_elsewhere ? Rgb(94, 160, 255, IsLightPaletteActive() ? 0.14f : 0.18f) : Rgb(34, 197, 94, IsLightPaletteActive() ? 0.14f : 0.18f)), pending_here ? ui::kWarning : (pending_elsewhere ? ui::kAccent : ui::kSuccess));
		ImGui::SameLine();
		DrawStatusChip("chat_count_chip", "Chats: " + std::to_string(app.chats.size()), IsLightPaletteActive() ? Rgb(9, 31, 63, 0.08f) : Rgb(255, 255, 255, 0.06f), ui::kTextSecondary);

		ImGui::TableSetColumnIndex(1);
		const std::string new_chat_label = FrontendActionLabel(app, "create_chat", "New Chat");
		const std::string refresh_label = FrontendActionLabel(app, "refresh_history", "Refresh");

		const float row_w = ImGui::GetContentRegionAvail().x;
		const bool show_create = FrontendActionVisible(app, "create_chat");
		const bool show_refresh = FrontendActionVisible(app, "refresh_history");
		const float row_spacing_base = ui::kSpace8 * PlatformServicesFactory::Instance().ui_traits.PlatformUiSpacingScale();
		const float row_spacing = ScaleUiLength(row_spacing_base);
		const float action_w = show_create && show_refresh ? 96.0f : 106.0f;
		const float settings_w = 96.0f;
		const float total_w = ScaleUiLength((show_create ? action_w + row_spacing_base : 0.0f) + (show_refresh ? action_w + row_spacing_base : 0.0f) + settings_w);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, row_w - total_w));
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ScaleUiLength(4.0f));

		if (show_create)
		{
			if (DrawButton(new_chat_label.c_str(), ImVec2(action_w, 34.0f), ButtonKind::Primary))
			{
				CreateAndSelectChat(app);
			}

			ImGui::SameLine(0.0f, row_spacing);
		}

		if (show_refresh)
		{
			if (DrawButton(refresh_label.c_str(), ImVec2(action_w, 34.0f), ButtonKind::Ghost))
			{
				ChatHistorySyncService().RefreshChatHistory(app);
			}

			ImGui::SameLine(0.0f, row_spacing);
		}

		if (DrawButton("Settings", ImVec2(96.0f, 34.0f), ButtonKind::Ghost))
		{
			app.open_app_settings_popup = true;
		}

		ImGui::EndTable();
	}

	EndPanel();
}
