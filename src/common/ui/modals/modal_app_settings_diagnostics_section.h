#pragma once

/// <summary>
/// Draws diagnostics information in the app settings modal.
/// </summary>
inline void DrawAppSettingsDiagnosticsSection(const AppState& app, const AppSettings& draft_settings)
{
	ImGui::TextColored(ui::kTextSecondary, "Paths");
	ImGui::TextWrapped("Data Root: %s", app.data_root.string().c_str());
	ImGui::TextWrapped("Action Map: %s", (app.data_root / "frontend_actions.txt").string().c_str());
#if UAM_ENABLE_ANY_GEMINI_PROVIDER
	ImGui::TextWrapped("Gemini Home: %s", AppPaths::GeminiHomePath().string().c_str());
#endif
	ImGui::TextWrapped("Prompt Profile Root: %s", ResolvePromptProfileRootPath(draft_settings).string().c_str());
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace8)));
	ImGui::TextColored(ui::kTextSecondary, "Build");
	ImGui::TextColored(ui::kTextMuted, "%s %s", __DATE__, __TIME__);
}
