#pragma once

inline void DrawMoveChatMissingSessionModal(AppState& app)
{
	if (app.move_chat_show_missing_session_warning)
	{
		ImGui::OpenPopup("move_chat_missing_session_popup");
		app.move_chat_show_missing_session_warning = false;
	}

	if (ImGui::BeginPopupModal("move_chat_missing_session_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextColored(ui::kWarning, "Session file not found.");
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
		ImGui::TextWrapped("The native session file could not be found in the target workspace. The chat will be moved, but may not resume from its previous state.");
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));

		if (DrawButton("Move Anyway", ImVec2(110.0f, 32.0f), ButtonKind::Primary))
		{
			const std::string l_chatId = app.move_chat_pending_id;
			const int l_chatIndex = ChatDomainService().FindChatIndexById(app, l_chatId);
			if (l_chatIndex >= 0)
			{
				ChatSession& chat = app.chats[l_chatIndex];
				if (!ChatRepository::SaveChat(app.data_root, chat))
				{
					app.status_line = "Failed to persist moved chat.";
				}
				else
				{
					app.status_line = "Chat moved to new folder.";
					app.move_chat_pending_id.clear();
					app.move_chat_original_folder_id.clear();
					app.move_chat_original_workspace.clear();
					app.move_chat_target_folder_id.clear();
					app.move_chat_target_workspace.clear();
					ImGui::CloseCurrentPopup();
				}
			}
		}

		ImGui::SameLine();

		if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
		{
			const std::string l_chatId = app.move_chat_pending_id;
			const int l_chatIndex = ChatDomainService().FindChatIndexById(app, l_chatId);
			if (l_chatIndex >= 0)
			{
				ChatSession& chat = app.chats[l_chatIndex];
				const ChatSession moved_chat = chat;
				chat.folder_id = app.move_chat_original_folder_id;
				chat.workspace_directory = app.move_chat_original_workspace;
				chat.updated_at = TimestampNow();
				if (!ChatRepository::SaveChat(app.data_root, chat))
				{
					chat = moved_chat;
					app.status_line = "Failed to persist cancelled move.";
				}
				else
				{
					app.status_line = "Move chat cancelled.";
					app.move_chat_pending_id.clear();
					app.move_chat_original_folder_id.clear();
					app.move_chat_original_workspace.clear();
					app.move_chat_target_folder_id.clear();
					app.move_chat_target_workspace.clear();
					ImGui::CloseCurrentPopup();
				}
			}
		}

		ImGui::EndPopup();
	}
}
