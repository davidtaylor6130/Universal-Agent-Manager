#pragma once

#include "app/chat_domain_service.h"

/// <summary>
/// Draws the create-folder modal used by the sidebar.
/// </summary>
inline void DrawSidebarNewFolderPopup(AppState& app)
{
	if (!ImGui::IsPopupOpen("new_folder_popup") && !app.pending_move_chat_to_new_folder_id.empty())
	{
		app.pending_move_chat_to_new_folder_id.clear();
	}

	if (ImGui::BeginPopupModal("new_folder_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextColored(ui::kTextPrimary, "Create chat folder");
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
		ImGui::SetNextItemWidth(420.0f);
		PushInputChrome();
		ImGui::InputText("Title", &app.new_folder_title_input);
		std::string browse_error;
		DrawPathInputWithBrowseButton("Directory", app.new_folder_directory_input, "new_folder_directory_picker", PathBrowseTarget::Directory, 420.0f, nullptr, nullptr, &browse_error);

		if (!browse_error.empty())
		{
			app.status_line = browse_error;
		}

		PopInputChrome();
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));

		if (DrawButton("Create", ImVec2(96.0f, 32.0f), ButtonKind::Primary))
		{
			const std::string folder_title = Trim(app.new_folder_title_input);
			const std::string folder_dir = Trim(app.new_folder_directory_input);

			if (folder_title.empty())
			{
				app.status_line = "Folder title is required.";
			}
			else if (folder_dir.empty())
			{
				app.status_line = "Folder directory is required.";
			}
			else
			{
				std::string created_folder_id;

				if (!CreateFolder(app, folder_title, folder_dir, &created_folder_id))
				{
					return;
				}

				const int move_chat_index = ChatDomainService().FindChatIndexById(app, app.pending_move_chat_to_new_folder_id);
				bool should_close = true;

				if (move_chat_index >= 0)
				{
					ChatSession& moved_chat = app.chats[move_chat_index];
					should_close = ChatHistorySyncService().MoveChatToFolder(app, moved_chat, created_folder_id) || app.move_chat_show_missing_session_warning;
				}
				else
				{
					app.status_line = "Project folder created.";
				}

				if (should_close)
				{
					app.pending_move_chat_to_new_folder_id.clear();
					ImGui::CloseCurrentPopup();
				}
			}
		}

		ImGui::SameLine();

		if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
		{
			app.pending_move_chat_to_new_folder_id.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}
