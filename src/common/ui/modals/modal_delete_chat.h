#pragma once

/// <summary>
/// Delete-chat confirmation modal renderer.
/// </summary>
static void DrawDeleteChatConfirmationModal(AppState& app)
{
	if (app.open_delete_chat_popup)
	{
		ImGui::OpenPopup("confirm_delete_chat_popup");
		app.open_delete_chat_popup = false;
	}

	if (ImGui::BeginPopupModal("confirm_delete_chat_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const int chat_index = FindChatIndexById(app, app.pending_delete_chat_id);
		const std::string chat_title = (chat_index >= 0) ? CompactPreview(app.chats[chat_index].title, 42) : "Unknown chat";
		ImGui::TextColored(ui::kTextPrimary, "Delete chat?");
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
		ImGui::TextWrapped("This will remove local metadata and any linked native Gemini session.");
		ImGui::TextColored(ui::kTextMuted, "Target: %s", chat_title.c_str());
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));

		if (DrawButton("Delete Chat", ImVec2(110.0f, 32.0f), ButtonKind::Primary))
		{
			RemoveChatById(app, app.pending_delete_chat_id);
			app.pending_delete_chat_id.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
		{
			app.pending_delete_chat_id.clear();
			app.status_line = "Delete chat cancelled.";
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}
