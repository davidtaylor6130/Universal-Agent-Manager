#pragma once
#include "common/platform/platform_services.h"

/// <summary>
/// Draws the slim chat title/status header row above the main conversation content.
/// </summary>
inline void DrawChatDetailHeaderBar(AppState& app, ChatSession& chat)
{
	if (BeginPanel("chat_header_bar", ImVec2(0.0f, 52.0f), PanelTone::Primary, false, 0, ImVec2(12.0f, 10.0f), ui::kRadiusInput))
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const float line_h = ImGui::GetTextLineHeight();
		const ImVec2 panel_pos = ImGui::GetWindowPos();
		const float row_y = panel_pos.y + 10.0f + line_h * 0.5f;

		// Status dot — animated when running
		const bool running = HasPendingCallForChat(app, chat.id);
		const ImVec4 dot_color = running ? ui::kWarning : ui::kSuccess;
		const float base_r = 4.0f;
		const float dot_r = running ? (base_r + 1.2f * std::abs(std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f))) : base_r;
		const float dot_x = panel_pos.x + 12.0f + dot_r;
		draw->AddCircleFilled(ImVec2(dot_x, row_y), dot_r, ImGui::GetColorU32(dot_color), 16);

		// Transparent-chrome title input (no visible border/background until focused)
		ImGui::SetCursorPos(ImVec2(dot_r * 2.0f + 20.0f, 8.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::kTransparent);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IsLightPaletteActive() ? Rgb(0, 0, 0, 0.04f) : Rgb(255, 255, 255, 0.04f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IsLightPaletteActive() ? Rgb(0, 0, 0, 0.07f) : Rgb(255, 255, 255, 0.06f));
		ImGui::PushStyleColor(ImGuiCol_Border, ui::kTransparent);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 3.0f));

		const float title_max_w = ImGui::GetContentRegionAvail().x * 0.55f;
		ImGui::SetNextItemWidth(title_max_w);
		const std::string title_id = "##chat_title_" + chat.id;

		if (ImGui::InputText(title_id.c_str(), &chat.title))
		{
			chat.updated_at = TimestampNow();
			SaveAndUpdateStatus(app, chat, "Chat title updated.", "Chat title changed in UI, but failed to save.");
		}

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(4);

		// Inline mode + timestamp chips
		ImGui::SameLine(0.0f, 10.0f);
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		const char* mode_label = ProviderRuntime::UsesCliOutput(provider) ? "· CLI" : "· Structured";
		ImGui::TextColored(ui::kTextMuted, "%s", mode_label);
		ImGui::SameLine(0.0f, 12.0f);
		ImGui::TextColored(ui::kTextMuted, "· %s", CompactPreview(chat.updated_at, 20).c_str());

		if (HasAnyPendingCall(app) && !running)
		{
			ImGui::SameLine(0.0f, 12.0f);
			ImGui::TextColored(ui::kTextMuted, "· busy");
		}
	}

	EndPanel();
}
