#pragma once

/// <summary>
/// Draws the template card in the chat settings side pane.
/// </summary>
inline void DrawChatSettingsTemplateCard(AppState& app, ChatSession& chat)
{
	DrawSectionHeader("Prompt Profile");

	if (BeginSectionCard("template_card"))
	{
		const std::string global_label = TemplateLabelOrFallback(app, app.settings.default_prompt_profile_id);
		ImGui::TextColored(ui::kTextMuted, "Global default");
		ImGui::TextWrapped("%s", global_label.c_str());
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

		const std::string override_preview = chat.template_override_id.empty() ? "Use global default" : TemplateLabelOrFallback(app, chat.template_override_id);

		if (ImGui::BeginCombo("Per-chat override", override_preview.c_str()))
		{
			const bool using_global = chat.template_override_id.empty();

			if (ImGui::Selectable("Use global default", using_global) && !using_global)
			{
				if (!chat.messages.empty())
				{
					app.pending_template_change_chat_id = chat.id;
					app.pending_template_change_override_id.clear();
					app.open_template_change_warning_popup = true;
				}
				else
				{
					ApplyChatTemplateOverride(app, chat, "", false);
				}
			}

			ImGui::Separator();

			for (const TemplateCatalogEntry& entry : app.template_catalog)
			{
				const bool selected = (chat.template_override_id == entry.id);

				if (ImGui::Selectable(entry.display_name.c_str(), selected) && !selected)
				{
					if (!chat.messages.empty())
					{
						app.pending_template_change_chat_id = chat.id;
						app.pending_template_change_override_id = entry.id;
						app.open_template_change_warning_popup = true;
					}
					else
					{
						ApplyChatTemplateOverride(app, chat, entry.id, false);
					}
				}

				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}

		std::string effective_template_id = chat.template_override_id.empty() ? app.settings.default_prompt_profile_id : chat.template_override_id;
		ImGui::TextColored(ui::kTextMuted, "Effective template");
		ImGui::TextWrapped("%s", TemplateLabelOrFallback(app, effective_template_id).c_str());
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

		if (DrawButton("Manage Templates", ImVec2(140.0f, 32.0f), ButtonKind::Ghost))
		{
			app.open_template_manager_popup = true;
		}

		ImGui::SameLine();

		if (DrawButton("Open Catalog", ImVec2(120.0f, 32.0f), ButtonKind::Ghost))
		{
			std::string error;
			const fs::path catalog_path = MarkdownTemplateCatalog::CatalogPath(ResolvePromptProfileRootPath(app.settings));

			if (!OpenFolderInFileManager(catalog_path, &error))
			{
				app.status_line = error;
			}
		}
	}

	EndPanel();
}
