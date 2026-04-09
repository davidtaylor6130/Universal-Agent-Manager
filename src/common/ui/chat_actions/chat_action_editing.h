#pragma once

#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"

/// <summary>
/// Edit-message workflow actions for user message rewinds and resend.
/// </summary>
inline void ClearEditMessageState(AppState& app)
{
	app.editing_chat_id.clear();
	app.editing_message_index = -1;
	app.editing_message_text.clear();
	app.open_edit_message_popup = false;
}

inline void BeginEditMessage(AppState& app, const ChatSession& chat, const int message_index)
{
	if (message_index < 0 || message_index >= static_cast<int>(chat.messages.size()))
	{
		return;
	}

	if (chat.messages[message_index].role != MessageRole::User)
	{
		return;
	}

	app.editing_chat_id = chat.id;
	app.editing_message_index = message_index;
	app.editing_message_text = chat.messages[message_index].content;
	app.open_edit_message_popup = true;
}

inline bool ContinueFromEditedUserMessage(AppState& app, ChatSession& chat)
{
	if (HasPendingCallForChat(app, chat.id))
	{
		app.status_line = "Cannot edit while Gemini is already running for this chat.";
		return false;
	}

	if (app.editing_chat_id != chat.id)
	{
		app.status_line = "Edit target no longer matches selected chat.";
		return false;
	}

	const int message_index = app.editing_message_index;

	if (message_index < 0 || message_index >= static_cast<int>(chat.messages.size()))
	{
		app.status_line = "Selected message index is no longer valid.";
		return false;
	}

	if (chat.messages[message_index].role != MessageRole::User)
	{
		app.status_line = "Only user messages can be edited.";
		return false;
	}

	const std::string prompt_text = Trim(app.editing_message_text);

	if (prompt_text.empty())
	{
		app.status_line = "Edited message cannot be empty.";
		return false;
	}

	if (CliTerminalState* terminal = FindCliTerminalForChat(app, chat.id))
	{
		if (terminal->running)
		{
			app.status_line = "Stop the live provider terminal for this chat before editing a previous message.";
			return false;
		}
	}

	if (ProviderResolutionService().ActiveProviderUsesNativeOverlayHistory(app) && NativeSessionLinkService().HasRealNativeSessionId(chat))
	{
		std::string native_error;

		if (!ChatHistorySyncService().TruncateNativeSessionFromDisplayedMessage(app, chat, message_index, &native_error))
		{
			app.status_line = "Failed to trim native Gemini session: " + native_error;
			return false;
		}
	}

	chat.messages.erase(chat.messages.begin() + message_index, chat.messages.end());
	chat.updated_at = TimestampNow();

	if (message_index == 0)
	{
		std::string title = prompt_text;

		if (title.size() > 48)
		{
			title = title.substr(0, 45) + "...";
		}

		chat.title = title;
	}

	ChatHistorySyncService().SaveChatWithStatus(app, chat, "Chat rewound to edited message.", "Chat rewound in UI, but failed to save chat data.");

	app.composer_text = prompt_text;
	ProviderRequestService().StartSelectedChatRequest(app);
	return true;
}
