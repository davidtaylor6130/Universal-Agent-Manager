#pragma once

/// <summary>
/// Draws appearance controls in the app settings modal.
/// </summary>
inline void DrawAppSettingsAppearanceSection(AppState& app, AppSettings& draft_settings, const float platform_scale)
{
	ImGui::TextColored(ui::kTextSecondary, "Appearance");
	const char* theme_preview = "Dark";

	if (draft_settings.ui_theme == "light")
	{
		theme_preview = "Light";
	}
	else if (draft_settings.ui_theme == "system")
	{
		theme_preview = "System";
	}

	if (ImGui::BeginCombo("Theme", theme_preview))
	{
		const std::array<std::pair<const char*, const char*>, 3> theme_options{{
		    {"dark", "Dark"},
		    {"light", "Light"},
		    {"system", "System"},
		}};

		for (const auto& option : theme_options)
		{
			const bool selected = (draft_settings.ui_theme == option.first);

			if (ImGui::Selectable(option.second, selected))
			{
				draft_settings.ui_theme = option.first;
				app.settings.ui_theme = option.first;
				ApplyThemeFromSettings(app);
				PersistenceCoordinator().SaveSettings(app);
				app.status_line = "Theme updated.";
			}

			if (selected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}

		ImGui::EndCombo();
	}

	ImGui::SetNextItemWidth(280.0f);
	ImGui::SliderFloat("UI Scale", &draft_settings.ui_scale_multiplier, 0.85f, 1.75f, "%.2fx");
	ImGui::TextColored(ui::kTextMuted, "Platform scale: %.2fx", platform_scale);
}
