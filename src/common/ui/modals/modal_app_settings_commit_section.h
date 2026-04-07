#ifndef UAM_COMMON_UI_MODALS_MODAL_APP_SETTINGS_COMMIT_SECTION_H
#define UAM_COMMON_UI_MODALS_MODAL_APP_SETTINGS_COMMIT_SECTION_H

/// <summary>
/// Draws save/cancel actions and applies app settings draft values.
/// </summary>
inline void DrawAppSettingsCommitSection(AppState& app, AppSettings& draft_settings, const float platform_scale, bool& initialized)
{
	if (DrawButton("Save Preferences", ImVec2(138.0f, 34.0f), ButtonKind::Primary))
	{
		const std::string previous_global_root = app.settings.prompt_profile_root_path;
		draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
#if UAM_ENABLE_ENGINE_RAG
		draft_settings.vector_db_backend = (draft_settings.vector_db_backend == "none") ? "none" : "ollama-engine";
#else
		draft_settings.vector_db_backend = "none";
		draft_settings.selected_vector_model_id.clear();
#endif
		draft_settings.cli_idle_timeout_seconds = std::clamp(draft_settings.cli_idle_timeout_seconds, 30, 3600);

		if (Trim(draft_settings.prompt_profile_root_path).empty())
		{
			draft_settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
		}

		ClampWindowSettings(draft_settings);
		app.settings = draft_settings;
		const ProviderProfile* active_profile = ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);

		if (active_profile != nullptr && ProviderRuntime::UsesCliOutput(*active_profile))
		{
			MarkSelectedCliTerminalForLaunch(app);
		}

		ApplyThemeFromSettings(app);
		g_platform_layout_scale = std::clamp(platform_scale, 1.0f, 2.25f);

		if (platform_scale > 1.01f)
		{
			ImGui::GetStyle().ScaleAllSizes(platform_scale);
		}

		CaptureUiScaleBaseStyle();
		ApplyUserUiScale(ImGui::GetIO(), app.settings.ui_scale_multiplier);
		PersistenceCoordinator().SaveSettings(app);

		if (previous_global_root != app.settings.prompt_profile_root_path)
		{
			app.template_catalog_dirty = true;
			TemplateRuntimeService().RefreshTemplateCatalog(app, true);
		}

		app.status_line = "Preferences saved.";
		initialized = false;
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();

	if (DrawButton("Cancel", ImVec2(96.0f, 34.0f), ButtonKind::Ghost))
	{
		initialized = false;
		ImGui::CloseCurrentPopup();
	}
}

#endif // UAM_COMMON_UI_MODALS_MODAL_APP_SETTINGS_COMMIT_SECTION_H
