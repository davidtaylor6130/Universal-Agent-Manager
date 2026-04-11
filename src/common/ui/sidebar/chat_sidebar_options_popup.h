#pragma once

#include "app/chat_domain_service.h"

/// <summary>
/// Draws the sidebar chat context menu popup.
/// </summary>
inline void DrawSidebarChatOptionsPopup(AppState& app)
{
	if (app.open_sidebar_chat_options_popup && !app.sidebar_chat_options_popup_chat_id.empty())
	{
		ImGui::OpenPopup("sidebar_chat_options_popup");
		app.open_sidebar_chat_options_popup = false;
	}

	if (!ImGui::BeginPopup("sidebar_chat_options_popup"))
	{
		return;
	}

	const int chat_index = ChatDomainService().FindChatIndexById(app, app.sidebar_chat_options_popup_chat_id);

	if (chat_index < 0)
	{
		ImGui::TextColored(ui::kTextMuted, "Chat no longer exists.");
		ImGui::EndPopup();
		return;
	}

	ChatSession& popup_chat = app.chats[chat_index];
	const auto ensure_selected_chat = [&]()
	{
		if (app.selected_chat_index != chat_index)
		{
			ChatDomainService().SelectChatById(app, popup_chat.id);
			PersistenceCoordinator().SaveSettings(app);
		}
	};

	if (ImGui::MenuItem("Rename..."))
	{
		ensure_selected_chat();
		app.rename_chat_target_id = popup_chat.id;
		app.rename_chat_input = popup_chat.title;
		app.inline_title_editing_chat_id = popup_chat.id;
		app.open_rename_chat_popup = true;
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	ImGui::EndPopup();
}
