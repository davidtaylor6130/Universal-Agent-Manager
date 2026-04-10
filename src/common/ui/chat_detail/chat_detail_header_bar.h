#pragma once
#include "app/runtime_orchestration_services.h"
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

		if (app.inline_title_editing_chat_id != chat.id)
		{
			app.inline_title_editing_chat_id = chat.id;
			app.rename_chat_input = chat.title;
		}

		ImGui::SetCursorPos(ImVec2(dot_r * 2.0f + 20.0f, 8.0f));
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(ui::kTextMuted, "Title");
		ImGui::SameLine(0.0f, 10.0f);
		const bool request_focus = DrawButton("Rename", ImVec2(78.0f, 28.0f), ButtonKind::Ghost);
		ImGui::SameLine(0.0f, 10.0f);
		PushInputChrome();

		const float title_max_w = ImGui::GetContentRegionAvail().x * 0.40f;
		ImGui::SetNextItemWidth(title_max_w);
		const std::string title_id = "##chat_title_buffered_" + chat.id;

		if (request_focus)
		{
			ImGui::SetKeyboardFocusHere();
		}

		const bool commit_from_enter = ImGui::InputText(title_id.c_str(), &app.rename_chat_input, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		const bool commit_from_blur = !commit_from_enter && ImGui::IsItemDeactivatedAfterEdit();

		if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			app.rename_chat_input = chat.title;
			ImGui::ClearActiveID();
		}

		if (commit_from_enter || commit_from_blur)
		{
			ChatHistorySyncService().RenameChat(app, chat, app.rename_chat_input);
			app.rename_chat_input = chat.title;
		}

		PopInputChrome();

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
