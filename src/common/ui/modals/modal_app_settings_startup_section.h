#pragma once

/// <summary>
/// Draws startup and window controls in the app settings modal.
/// </summary>
inline void DrawAppSettingsStartupSection(AppSettings& draft_settings)
{
	ImGui::SetNextItemWidth(140.0f);
	ImGui::InputInt("Window Width", &draft_settings.window_width);
	ImGui::SetNextItemWidth(140.0f);
	ImGui::InputInt("Window Height", &draft_settings.window_height);
	ImGui::Checkbox("Start maximized", &draft_settings.window_maximized);
}
