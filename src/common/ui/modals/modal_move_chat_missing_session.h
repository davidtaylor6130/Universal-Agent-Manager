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
				ChatSession& l_chat = app.chats[l_chatIndex];
				ChatRepository::SaveChat(app.data_root, l_chat);
				app.status_line = "Chat moved to new folder.";
			}
			app.move_chat_pending_id.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
		{
			const std::string l_chatId = app.move_chat_pending_id;
			const int l_chatIndex = ChatDomainService().FindChatIndexById(app, l_chatId);
			if (l_chatIndex >= 0)
			{
				app.chats[l_chatIndex].folder_id = app.move_chat_original_folder_id;
				app.chats[l_chatIndex].workspace_directory = app.move_chat_original_workspace;
			}
			app.move_chat_pending_id.clear();
			app.status_line = "Move chat cancelled.";
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}