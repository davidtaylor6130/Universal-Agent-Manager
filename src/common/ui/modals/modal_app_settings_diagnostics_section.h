#pragma once

/// <summary>
/// Draws diagnostics information in the app settings modal.
/// </summary>
inline void DrawAppSettingsDiagnosticsSection(const AppState& app, const AppSettings& draft_settings)
{
	ImGui::TextColored(ui::kTextSecondary, "Diagnostics / About");
	ImGui::TextWrapped("Data Root: %s", app.data_root.string().c_str());
	ImGui::TextWrapped("Provider Profiles: %s", (app.data_root / "providers.txt").string().c_str());
	ImGui::TextWrapped("Action Map: %s", (app.data_root / "frontend_actions.txt").string().c_str());
	ImGui::TextWrapped("Gemini Home: %s", AppPaths::GeminiHomePath().string().c_str());
	ImGui::TextWrapped("Prompt Profile Root: %s", ResolvePromptProfileRootPath(draft_settings).string().c_str());
	ImGui::TextColored(ui::kTextMuted, "Build: %s %s", __DATE__, __TIME__);
}
