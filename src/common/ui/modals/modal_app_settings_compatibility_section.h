#pragma once

/// <summary>
/// Draws Gemini CLI compatibility status and actions in the app settings modal.
/// </summary>
static void DrawAppSettingsCompatibilitySection(AppState& app)
{
	ImGui::TextColored(ui::kTextSecondary, "Gemini CLI Compatibility");
	ImGui::TextColored(ui::kTextMuted, "Supported version: %s", kSupportedGeminiVersion);

	if (app.gemini_version_check_task.running)
	{
		ImGui::TextColored(ui::kTextMuted, "Checking installed Gemini version...");
	}
	else if (!app.gemini_version_checked)
	{
		ImGui::TextColored(ui::kTextMuted, "Installed Gemini version has not been checked yet.");
	}
	else if (!app.gemini_installed_version.empty())
	{
		const bool supported = app.gemini_version_supported;
		ImGui::TextColored(supported ? ui::kSuccess : ui::kWarning, "Installed: %s (%s)", app.gemini_installed_version.c_str(), supported ? "supported" : "unsupported");
	}
	else
	{
		ImGui::TextColored(ui::kWarning, "Installed Gemini version could not be detected.");
	}

	if (!app.gemini_version_message.empty())
	{
		ImGui::TextWrapped("%s", app.gemini_version_message.c_str());
	}

	if (app.gemini_version_check_task.running)
	{
		ImGui::BeginDisabled();
	}

	if (DrawButton("Re-check Version", ImVec2(130.0f, 30.0f), ButtonKind::Ghost))
	{
		StartGeminiVersionCheck(app, true);
	}

	if (app.gemini_version_check_task.running)
	{
		ImGui::EndDisabled();
	}

	const bool show_downgrade_action = app.gemini_version_checked && !app.gemini_installed_version.empty() && !app.gemini_version_supported;

	if (show_downgrade_action)
	{
		ImGui::SameLine();

		if (app.gemini_downgrade_task.running)
		{
			ImGui::BeginDisabled();
		}

		if (DrawButton("Downgrade to 0.30.0", ImVec2(166.0f, 30.0f), ButtonKind::Primary))
		{
			StartGeminiDowngradeToSupported(app);
		}

		if (app.gemini_downgrade_task.running)
		{
			ImGui::EndDisabled();
		}

		ImGui::TextColored(ui::kTextMuted, "Downgrade command: %s", BuildGeminiDowngradeCommand().c_str());
	}

	if (app.gemini_downgrade_task.running)
	{
		ImGui::TextColored(ui::kTextMuted, "Downgrade in progress...");
	}
	else if (!app.gemini_downgrade_output.empty())
	{
		ImGui::TextColored(ui::kTextMuted, "Last downgrade output");
		std::string downgrade_output_preview = app.gemini_downgrade_output;
		PushInputChrome();
		ImGui::InputTextMultiline("##gemini_downgrade_output", &downgrade_output_preview, ImVec2(520.0f, 88.0f), ImGuiInputTextFlags_ReadOnly);
		PopInputChrome();
	}
}
