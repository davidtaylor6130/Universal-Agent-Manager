#pragma once

#include "common/models/app_models.h"
#include "common/utils/string_utils.h"

#include <algorithm>

namespace uam
{
	/// Native-history eligibility excludes setup, whitespace and status-only messages.
	inline bool HasImportedConversationContent(const Message& message)
	{
		if (message.role != MessageRole::User && message.role != MessageRole::Assistant) return false;
		return !strings::IsBlank(message.content) || !strings::IsBlank(message.thoughts) ||
		    !message.attachments.empty() || !message.tool_calls.empty() || !strings::IsBlank(message.plan_summary) ||
		    !message.plan_entries.empty() || std::ranges::any_of(message.blocks, [](const MessageBlock& block)
		    {
			return (block.type == "assistant_text" || block.type == "thought") && !strings::IsBlank(block.text);
		    });
	}

	inline bool HasImportedConversationContent(const std::vector<Message>& messages)
	{
		return std::ranges::any_of(messages, [](const Message& message) { return HasImportedConversationContent(message); });
	}
}
