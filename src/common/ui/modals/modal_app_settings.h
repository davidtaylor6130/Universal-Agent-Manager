#pragma once

#include "common/ui/modals/modal_app_settings_appearance_section.h"
#include "common/ui/modals/modal_app_settings_behavior_section.h"
#include "common/ui/modals/modal_app_settings_commit_section.h"
#include "common/ui/modals/modal_app_settings_compatibility_section.h"
#include "common/ui/modals/modal_app_settings_diagnostics_section.h"
#include "common/ui/modals/modal_app_settings_shortcuts_section.h"
#include "common/ui/modals/modal_app_settings_startup_section.h"
#include "common/ui/modals/modal_app_settings_templates_section.h"

/// <summary>
/// Draws the application settings modal using section-level UI components.
/// </summary>
static void DrawAppSettingsModal(AppState& app, const float platform_scale)
{
	static AppSettings draft_settings{};
	static bool initialized = false;

	if (app.open_app_settings_popup)
	{
		draft_settings = app.settings;

		if (Trim(draft_settings.prompt_profile_root_path).empty())
		{
			draft_settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
		}

		draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
		initialized = true;
		ImGui::OpenPopup("app_settings_popup");
		app.open_app_settings_popup = false;
		StartGeminiVersionCheck(app, true);
	}

	if (ImGui::BeginPopupModal("app_settings_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (!initialized)
		{
			draft_settings = app.settings;

			if (Trim(draft_settings.prompt_profile_root_path).empty())
			{
				draft_settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
			}

			draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
			initialized = true;
			StartGeminiVersionCheck(app, false);
		}

		RefreshTemplateCatalog(app);

		ImGui::TextColored(ui::kTextPrimary, "Application Settings");
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
		DrawSoftDivider();

		DrawAppSettingsAppearanceSection(app, draft_settings, platform_scale);

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
		DrawSoftDivider();
		DrawAppSettingsBehaviorSection(app, draft_settings);

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
		DrawSoftDivider();
		DrawAppSettingsTemplatesSection(app, draft_settings);

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
		DrawSoftDivider();
		const ProviderProfile* active_profile = ProviderProfileStore::FindById(app.provider_profiles, draft_settings.active_provider_id);

		if (active_profile != nullptr && IsGeminiProviderId(active_profile->id))
		{
			DrawAppSettingsCompatibilitySection(app);
		}
		else
		{
			ImGui::TextColored(ui::kTextSecondary, "Provider Compatibility");
			ImGui::TextColored(ui::kTextMuted, "Gemini compatibility checks are shown when Gemini is the active provider.");
		}

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
		DrawSoftDivider();
		DrawAppSettingsStartupSection(draft_settings);

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
		DrawSoftDivider();
		DrawAppSettingsDiagnosticsSection(app, draft_settings);

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
		DrawSoftDivider();
		DrawAppSettingsShortcutsSection();

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
		DrawAppSettingsCommitSection(app, draft_settings, platform_scale, initialized);
		ImGui::EndPopup();
	}
}
