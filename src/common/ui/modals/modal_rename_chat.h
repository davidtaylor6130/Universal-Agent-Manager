#pragma once

/// <summary>
/// Chat rename modal shared by sidebar actions.
/// </summary>
inline void DrawRenameChatModal(AppState& app)
{
	if (app.open_rename_chat_popup && !app.rename_chat_target_id.empty())
	{
		ImGui::OpenPopup("rename_chat_popup");
		app.open_rename_chat_popup = false;
	}

	if (!ImGui::BeginPopupModal("rename_chat_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	const int chat_index = ChatDomainService().FindChatIndexById(app, app.rename_chat_target_id);

	if (chat_index < 0)
	{
		ImGui::TextColored(ui::kWarning, "Chat no longer exists.");
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));

		if (DrawButton("Close", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
		{
			app.rename_chat_target_id.clear();
			app.rename_chat_input.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return;
	}

	ChatSession& chat = app.chats[chat_index];
	ImGui::TextColored(ui::kTextPrimary, "Rename Chat");
	ImGui::TextColored(ui::kTextMuted, "Update the local UAM title shown in the sidebar and header.");
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	PushInputChrome();
	ImGui::SetNextItemWidth(420.0f);

	if (ImGui::IsWindowAppearing())
	{
		app.rename_chat_input = chat.title;
		ImGui::SetKeyboardFocusHere();
	}

	const bool submit = ImGui::InputText("Title##rename_chat_title", &app.rename_chat_input, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	PopInputChrome();
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));

	const auto close_modal = [&]()
	{
		app.rename_chat_target_id.clear();
		app.rename_chat_input = chat.title;
		ImGui::CloseCurrentPopup();
	};

	if (submit || DrawButton("Save", ImVec2(96.0f, 32.0f), ButtonKind::Primary))
	{
		if (ChatHistorySyncService().RenameChat(app, chat, app.rename_chat_input))
		{
			app.inline_title_editing_chat_id = chat.id;
			close_modal();
		}
		else
		{
			app.rename_chat_input = chat.title;
		}
	}

	ImGui::SameLine();

	if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost) || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		close_modal();
	}

	ImGui::EndPopup();
}
