#ifndef UAM_COMMON_UI_MODALS_MODAL_APP_SETTINGS_COMPATIBILITY_SECTION_H
#define UAM_COMMON_UI_MODALS_MODAL_APP_SETTINGS_COMPATIBILITY_SECTION_H

#include "common/constants/app_constants.h"
#include "common/platform/platform_services.h"
#include "common/runtime/provider_cli_compatibility_service.h"

/// <summary>
/// Draws Gemini CLI compatibility status and actions in the app settings modal.
/// </summary>
inline void DrawAppSettingsCompatibilitySection(AppState& app)
{
	ImGui::TextColored(ui::kTextSecondary, "Gemini CLI Compatibility");
	ImGui::TextColored(ui::kTextMuted, "Supported version: %s", uam::constants::kSupportedGeminiVersion);

	if (app.runtime_cli_version_check_task.running)
	{
		ImGui::TextColored(ui::kTextMuted, "Checking installed Gemini version...");
	}
	else if (!app.runtime_cli_version_checked)
	{
		ImGui::TextColored(ui::kTextMuted, "Installed Gemini version has not been checked yet.");
	}
	else if (!app.runtime_cli_installed_version.empty())
	{
		const bool supported = app.runtime_cli_version_supported;
		ImGui::TextColored(supported ? ui::kSuccess : ui::kWarning, "Installed: %s (%s)", app.runtime_cli_installed_version.c_str(), supported ? "supported" : "unsupported");
	}
	else
	{
		ImGui::TextColored(ui::kWarning, "Installed Gemini version could not be detected.");
	}

	if (!app.runtime_cli_version_message.empty())
	{
		ImGui::TextWrapped("%s", app.runtime_cli_version_message.c_str());
	}

	if (app.runtime_cli_version_check_task.running)
	{
		ImGui::BeginDisabled();
	}

	if (DrawButton("Re-check Version", ImVec2(130.0f, 30.0f), ButtonKind::Ghost))
	{
		ProviderCliCompatibilityService().StartVersionCheck(app, true);
	}

	if (app.runtime_cli_version_check_task.running)
	{
		ImGui::EndDisabled();
	}

	const bool show_downgrade_action = app.runtime_cli_version_checked && !app.runtime_cli_installed_version.empty() && !app.runtime_cli_version_supported;

	if (show_downgrade_action)
	{
		ImGui::SameLine();

		if (app.runtime_cli_pin_task.running)
		{
			ImGui::BeginDisabled();
		}

		if (DrawButton("Downgrade to 0.30.0", ImVec2(166.0f, 30.0f), ButtonKind::Primary))
		{
			ProviderCliCompatibilityService().StartPinToSupported(app);
		}

		if (app.runtime_cli_pin_task.running)
		{
			ImGui::EndDisabled();
		}

		ImGui::TextColored(ui::kTextMuted,
		                   "Downgrade command: %s",
		                   PlatformServicesFactory::Instance().process_service.GeminiDowngradeCommand().c_str());
	}

	if (app.runtime_cli_pin_task.running)
	{
		ImGui::TextColored(ui::kTextMuted, "Downgrade in progress...");
	}
	else if (!app.runtime_cli_pin_output.empty())
	{
		ImGui::TextColored(ui::kTextMuted, "Last downgrade output");
		std::string downgrade_output_preview = app.runtime_cli_pin_output;
		PushInputChrome();
		ImGui::InputTextMultiline("##runtime_cli_pin_output", &downgrade_output_preview, ImVec2(520.0f, 88.0f), ImGuiInputTextFlags_ReadOnly);
		PopInputChrome();
	}
}

#endif // UAM_COMMON_UI_MODALS_MODAL_APP_SETTINGS_COMPATIBILITY_SECTION_H
