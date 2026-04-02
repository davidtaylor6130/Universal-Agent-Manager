#pragma once

/// <summary>
/// Draws template-manager actions for selected template mutation and assignment.
/// </summary>
static void DrawTemplateManagerSelectionSection(AppState& app, const fs::path& global_root, const TemplateCatalogEntry* selected_entry, const bool has_selection)
{
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));

	if (!has_selection)
	{
		ImGui::BeginDisabled();
	}

	if (DrawButton("Set as Default", ImVec2(122.0f, 30.0f), ButtonKind::Ghost) && selected_entry != nullptr)
	{
		app.settings.default_prompt_profile_id = selected_entry->id;
		SaveSettings(app);
		app.status_line = "Default prompt profile updated.";
	}

	ImGui::SameLine();

	if (DrawButton("Use for This Chat", ImVec2(126.0f, 30.0f), ButtonKind::Ghost) && selected_entry != nullptr)
	{
		ChatSession* selected_chat = SelectedChat(app);

		if (selected_chat != nullptr)
		{
			if (!selected_chat->messages.empty())
			{
				app.pending_template_change_chat_id = selected_chat->id;
				app.pending_template_change_override_id = selected_entry->id;
				app.open_template_change_warning_popup = true;
			}
			else
			{
				ApplyChatTemplateOverride(app, *selected_chat, selected_entry->id, false);
			}
		}
		else
		{
			app.status_line = "Select a chat first.";
		}
	}

	ImGui::SameLine();

	if (DrawButton("Reveal File", ImVec2(96.0f, 30.0f), ButtonKind::Ghost) && selected_entry != nullptr)
	{
		std::string error;

		if (!RevealPathInFileManager(fs::path(selected_entry->absolute_path), &error))
		{
			app.status_line = error;
		}
	}

	if (!has_selection)
	{
		ImGui::EndDisabled();
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	PushInputChrome();
	ImGui::SetNextItemWidth(360.0f);
	ImGui::InputText("Rename##template_rename_input", &app.template_rename_input);
	PopInputChrome();

	if (!has_selection)
	{
		ImGui::BeginDisabled();
	}

	if (DrawButton("Rename", ImVec2(92.0f, 30.0f), ButtonKind::Ghost) && selected_entry != nullptr)
	{
		std::string new_id;
		std::string error;

		if (GeminiTemplateCatalog::RenameTemplate(global_root, selected_entry->id, app.template_rename_input, &new_id, &error))
		{
			if (app.settings.default_prompt_profile_id == selected_entry->id)
			{
				app.settings.default_prompt_profile_id = new_id;
				SaveSettings(app);
			}

			for (ChatSession& chat : app.chats)
			{
				if (chat.template_override_id == selected_entry->id)
				{
					chat.template_override_id = new_id;
					SaveChat(app, chat);
				}
			}

			MarkTemplateCatalogDirty(app);
			RefreshTemplateCatalog(app, true);
			app.template_manager_selected_id = new_id;
			app.status_line = "Template renamed.";
		}
		else
		{
			app.status_line = error;
		}
	}

	ImGui::SameLine();

	if (DrawButton("Delete", ImVec2(92.0f, 30.0f), ButtonKind::Primary) && selected_entry != nullptr)
	{
		std::string error;
		const std::string removed_id = selected_entry->id;

		if (GeminiTemplateCatalog::RemoveTemplate(global_root, removed_id, &error))
		{
			if (app.settings.default_prompt_profile_id == removed_id)
			{
				app.settings.default_prompt_profile_id.clear();
				SaveSettings(app);
			}

			for (ChatSession& chat : app.chats)
			{
				if (chat.template_override_id == removed_id)
				{
					chat.template_override_id.clear();
					SaveChat(app, chat);
				}
			}

			app.template_manager_selected_id.clear();
			app.template_rename_input.clear();
			MarkTemplateCatalogDirty(app);
			RefreshTemplateCatalog(app, true);
			app.status_line = "Template deleted.";
		}
		else
		{
			app.status_line = error;
		}
	}

	if (!has_selection)
	{
		ImGui::EndDisabled();
	}
}
