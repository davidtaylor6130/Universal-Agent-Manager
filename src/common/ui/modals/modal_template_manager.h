#pragma once

#include "common/ui/modals/modal_template_manager_catalog_section.h"
#include "common/ui/modals/modal_template_manager_global_root_section.h"
#include "common/ui/modals/modal_template_manager_import_section.h"
#include "common/ui/modals/modal_template_manager_selection_section.h"

/// <summary>
/// Draws the markdown template manager modal using section-level component renderers.
/// </summary>
static void DrawTemplateManagerModal(AppState& app)
{
	if (app.open_template_manager_popup)
	{
		RefreshTemplateCatalog(app, true);
		app.template_import_path_input.clear();
		ImGui::OpenPopup("template_manager_popup");
		app.open_template_manager_popup = false;
	}

	if (!ImGui::BeginPopupModal("template_manager_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	RefreshTemplateCatalog(app);
	const fs::path global_root = ResolveGeminiGlobalRootPath(app.settings);
	const fs::path catalog_path = MarkdownTemplateCatalog::CatalogPath(global_root);

	ImGui::TextColored(ui::kTextPrimary, "Markdown Template Manager");
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	DrawSoftDivider();

	DrawTemplateManagerGlobalRootSection(app, global_root, catalog_path);

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
	DrawSoftDivider();
	DrawTemplateManagerImportSection(app, global_root);

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
	DrawSoftDivider();
	bool has_selection = false;
	const TemplateCatalogEntry* selected_entry = DrawTemplateManagerCatalogSection(app, has_selection);
	DrawTemplateManagerSelectionSection(app, global_root, selected_entry, has_selection);

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));

	if (DrawButton("Close", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
	{
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}
