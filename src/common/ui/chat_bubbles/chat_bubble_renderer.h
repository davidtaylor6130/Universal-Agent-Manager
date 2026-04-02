#pragma once

#include "common/ui/chat_bubbles/chat_bubble_assistant.h"
#include "common/ui/chat_bubbles/chat_bubble_system.h"
#include "common/ui/chat_bubbles/chat_bubble_user.h"

/// <summary>
/// Dispatches message rendering to role-specific bubble renderers.
/// </summary>
static void DrawMessageBubble(AppState& app, ChatSession& chat, const int message_index, const float content_width)
{
	if (message_index < 0 || message_index >= static_cast<int>(chat.messages.size()))
	{
		return;
	}

	const Message& message = chat.messages[message_index];
	const bool align_right = (message.role == MessageRole::User);
	const ChatBubbleLayout layout = BuildChatBubbleLayout(message.content, content_width, align_right);

	if (message.role == MessageRole::User)
	{
		DrawUserMessageBubble(app, chat, message_index, message, layout);
	}
	else if (message.role == MessageRole::Assistant)
	{
		DrawAssistantMessageBubble(message, layout);
	}
	else
	{
		DrawSystemMessageBubble(message, layout);
	}

	const float extra_bottom = (message.role == MessageRole::User) ? 26.0f : 0.0f;
	EndChatBubbleRow(layout, extra_bottom);
}
