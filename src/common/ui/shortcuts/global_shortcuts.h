#pragma once
#include "common/platform/sdl_includes.h"

/// <summary>
/// Handles global keyboard shortcuts for app-level chat actions.
/// </summary>
inline void HandleGlobalShortcuts(AppState& app)
{
	ImGuiIO& io = ImGui::GetIO();
	const bool ctrl = io.KeyCtrl;

	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false))
	{
		app.open_app_settings_popup = true;
	}

	const bool allow_global_action = !io.WantTextInput && !ImGui::IsAnyItemActive();

	if (allow_global_action && ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false))
	{
		CreateAndSelectChat(app);
	}

	if (allow_global_action && ctrl && ImGui::IsKeyPressed(ImGuiKey_R, false))
	{
		ChatHistorySyncService().RefreshChatHistory(app);
	}

	if (allow_global_action && ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
	{
		app.settings.provider_yolo_mode = !app.settings.provider_yolo_mode;
		app.settings.gemini_yolo_mode = app.settings.provider_yolo_mode;
		SaveSettings(app);
		app.status_line = app.settings.provider_yolo_mode ? "YOLO mode enabled." : "YOLO mode disabled.";
	}

	if (allow_global_action && ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false))
	{
		SDL_Event quit_event{};
		quit_event.type = SDL_QUIT;
		SDL_PushEvent(&quit_event);
	}
}
