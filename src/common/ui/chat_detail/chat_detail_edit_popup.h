#pragma once

/// <summary>
/// Draws the edit-user-message popup used for rewind-and-continue flow.
/// </summary>
static void DrawEditUserMessagePopup(AppState& app, ChatSession& chat)
{
	if (app.open_edit_message_popup)
	{
		ImGui::OpenPopup("edit_user_message_popup");
		app.open_edit_message_popup = false;
	}

	if (ImGui::BeginPopupModal("edit_user_message_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const bool editing_selected_chat = (app.editing_chat_id == chat.id);
		const bool valid_index = (app.editing_message_index >= 0 && app.editing_message_index < static_cast<int>(chat.messages.size()));
		const bool valid_target = editing_selected_chat && valid_index && chat.messages[app.editing_message_index].role == MessageRole::User;

		if (!valid_target)
		{
			ImGui::TextColored(ui::kWarning, "The selected message is no longer editable.");
		}
		else
		{
			ImGui::TextColored(ui::kTextPrimary, "Edit message and continue from this point");
			ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
			PushInputChrome();
			ImGui::InputTextMultiline("##edited_user_message", &app.editing_message_text, ImVec2(560.0f, 170.0f));
			PopInputChrome();
			ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
			ImGui::TextColored(ui::kTextMuted, "This removes all messages after this point and re-runs Gemini.");
		}

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));

		if (valid_target)
		{
			if (DrawButton("Apply + Continue", ImVec2(150.0f, 32.0f), ButtonKind::Primary))
			{
				if (ContinueFromEditedUserMessage(app, chat))
				{
					ClearEditMessageState(app);
					ImGui::CloseCurrentPopup();
				}
			}

			ImGui::SameLine();
		}

		if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
		{
			ClearEditMessageState(app);
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}
