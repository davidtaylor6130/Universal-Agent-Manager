#pragma once

/// <summary>
/// Draws the template-manager catalog list and selected template state.
/// </summary>
inline const TemplateCatalogEntry* DrawMarkdownTemplateManagerCatalogSection(AppState& app, bool& has_selection_out)
{
	ImGui::TextColored(ui::kTextSecondary, "Catalog Entries (%zu)", app.template_catalog.size());

	if (ImGui::BeginChild("template_catalog_list", ImVec2(560.0f, 220.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
	{
		for (const TemplateCatalogEntry& entry : app.template_catalog)
		{
			const bool selected = (entry.id == app.template_manager_selected_id);
			std::string line = entry.display_name + "  (" + entry.id + ")";

			if (ImGui::Selectable(line.c_str(), selected))
			{
				app.template_manager_selected_id = entry.id;
				app.template_rename_input = entry.display_name;
			}
		}
	}

	ImGui::EndChild();

	const TemplateCatalogEntry* selected_entry = TemplateRuntimeService().FindTemplateEntryById(app, app.template_manager_selected_id);
	has_selection_out = (selected_entry != nullptr);

	if (!has_selection_out)
	{
		app.template_rename_input.clear();
	}

	return selected_entry;
}
