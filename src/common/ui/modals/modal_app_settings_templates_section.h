#pragma once

/// <summary>
/// Draws template-related controls in the app settings modal.
/// </summary>
static void DrawAppSettingsTemplatesSection(AppState& app, AppSettings& draft_settings)
{
	ImGui::TextColored(ui::kTextSecondary, "Prompt Profiles");
	PushInputChrome();
	std::string browse_error;
	DrawPathInputWithBrowseButton("Global Root", draft_settings.prompt_profile_root_path, "app_settings_prompt_profile_root_picker", PathBrowseTarget::Directory, 520.0f, nullptr, nullptr, &browse_error);

	if (!browse_error.empty())
	{
		app.status_line = browse_error;
	}

	PopInputChrome();
	ImGui::TextColored(ui::kTextMuted, "Catalog folder: <root>/Markdown_Templates");
	const std::string current_default_template = TemplateLabelOrFallback(app, app.settings.default_prompt_profile_id);
	ImGui::TextColored(ui::kTextMuted, "Current default: %s", current_default_template.c_str());

	if (DrawButton("Open Markdown Template Manager", ImVec2(264.0f, 30.0f), ButtonKind::Ghost))
	{
		app.open_template_manager_popup = true;
	}
}
