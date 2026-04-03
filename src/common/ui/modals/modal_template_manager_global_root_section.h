#ifndef UAM_COMMON_UI_MODALS_MODAL_TEMPLATE_MANAGER_GLOBAL_ROOT_SECTION_H
#define UAM_COMMON_UI_MODALS_MODAL_TEMPLATE_MANAGER_GLOBAL_ROOT_SECTION_H

/// <summary>
/// Draws template-manager controls for global root selection and catalog folder access.
/// </summary>
inline void DrawMarkdownTemplateManagerGlobalRootSection(AppState& app, const fs::path& global_root, const fs::path& catalog_path)
{
	(void)global_root;
	ImGui::TextColored(ui::kTextSecondary, "Global Root");
	PushInputChrome();
	std::string browse_error;
	DrawPathInputWithBrowseButton("##prompt_profile_global_root", app.settings.prompt_profile_root_path, "template_manager_global_root_picker", PathBrowseTarget::Directory, 560.0f, nullptr, nullptr, &browse_error);

	if (!browse_error.empty())
	{
		app.status_line = browse_error;
	}

	PopInputChrome();

	if (DrawButton("Save Root", ImVec2(110.0f, 30.0f), ButtonKind::Ghost))
	{
		if (Trim(app.settings.prompt_profile_root_path).empty())
		{
			app.settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
		}

		SaveSettings(app);
		app.template_catalog_dirty = true;
		TemplateRuntimeService().RefreshTemplateCatalog(app, true);
		app.status_line = "Global prompt-profile root saved.";
	}

	ImGui::SameLine();

	if (DrawButton("Open Catalog Folder", ImVec2(148.0f, 30.0f), ButtonKind::Ghost))
	{
		std::string error;

		if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInFileManager(catalog_path, &error))
		{
			app.status_line = error;
		}
	}

	ImGui::TextColored(ui::kTextMuted, "%s", catalog_path.string().c_str());
}

#endif // UAM_COMMON_UI_MODALS_MODAL_TEMPLATE_MANAGER_GLOBAL_ROOT_SECTION_H
