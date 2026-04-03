#ifndef UAM_COMMON_UI_MODALS_MODAL_TEMPLATE_MANAGER_IMPORT_SECTION_H
#define UAM_COMMON_UI_MODALS_MODAL_TEMPLATE_MANAGER_IMPORT_SECTION_H

/// <summary>
/// Draws template-manager import controls for markdown template ingestion.
/// </summary>
inline void DrawMarkdownTemplateManagerImportSection(AppState& app, const fs::path& global_root)
{
	ImGui::TextColored(ui::kTextSecondary, "Import Markdown Template");
	PushInputChrome();
	std::string browse_error;
	DrawPathInputWithBrowseButton("Path", app.template_import_path_input, "template_import_file_picker", PathBrowseTarget::File, 560.0f, nullptr, nullptr, &browse_error);

	if (!browse_error.empty())
	{
		app.status_line = browse_error;
	}

	PopInputChrome();

	if (DrawButton("Import", ImVec2(96.0f, 30.0f), ButtonKind::Primary))
	{
		std::string imported_id;
		std::string error;

		if (MarkdownTemplateCatalog::ImportMarkdownTemplate(global_root, fs::path(Trim(app.template_import_path_input)), &imported_id, &error))
		{
			app.template_catalog_dirty = true;
			TemplateRuntimeService().RefreshTemplateCatalog(app, true);
			app.template_manager_selected_id = imported_id;
			const TemplateCatalogEntry* imported = TemplateRuntimeService().FindTemplateEntryById(app, imported_id);
			app.template_rename_input = (imported != nullptr) ? imported->display_name : "";
			app.status_line = "Template imported.";
			app.template_import_path_input.clear();
		}
		else
		{
			app.status_line = error;
		}
	}
}

#endif // UAM_COMMON_UI_MODALS_MODAL_TEMPLATE_MANAGER_IMPORT_SECTION_H
