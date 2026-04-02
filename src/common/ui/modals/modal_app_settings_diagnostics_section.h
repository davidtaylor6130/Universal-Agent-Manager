#pragma once

/// <summary>
/// Draws diagnostics information in the app settings modal.
/// </summary>
static void DrawAppSettingsDiagnosticsSection(const AppState& app, const AppSettings& draft_settings)
{
	ImGui::TextColored(ui::kTextSecondary, "Diagnostics / About");
	ImGui::TextWrapped("Data Root: %s", app.data_root.string().c_str());
	ImGui::TextWrapped("Provider Profiles: %s", ProviderProfileFilePath(app).string().c_str());
	ImGui::TextWrapped("Action Map: %s", FrontendActionFilePath(app).string().c_str());
	ImGui::TextWrapped("Gemini Home: %s", AppPaths::GeminiHomePath().string().c_str());
	ImGui::TextWrapped("Prompt Profile Root: %s", ResolveGeminiGlobalRootPath(draft_settings).string().c_str());
	ImGui::TextColored(ui::kTextMuted, "Build: %s %s", __DATE__, __TIME__);
}
