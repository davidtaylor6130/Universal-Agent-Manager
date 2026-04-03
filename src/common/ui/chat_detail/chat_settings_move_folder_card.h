#pragma once

/// <summary>
/// Draws the folder assignment card in the chat settings side pane.
/// </summary>
inline void DrawChatSettingsMoveFolderCard(AppState& app, ChatSession& chat)
{
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
	DrawSectionHeader("Move to Folder");

	if (BeginSectionCard("folder_card"))
	{
		const ChatFolder* active_folder = ChatDomainService().FindFolderById(app, chat.folder_id);
		const char* active_label = (active_folder != nullptr) ? active_folder->title.c_str() : "(Unassigned)";

		if (ImGui::BeginCombo("Folder", active_label))
		{
			for (const ChatFolder& folder : app.folders)
			{
				const bool selected = (chat.folder_id == folder.id);
				const std::string folder_name = ChatDomainService().FolderTitleOrFallback(folder);

				if (ImGui::Selectable(folder_name.c_str(), selected))
				{
					chat.folder_id = folder.id;
					chat.updated_at = TimestampNow();
					SaveAndUpdateStatus(app, chat, "Chat moved to folder.", "Moved chat in UI, but failed to save.");
				}

				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}
	}

	EndPanel();
}
