#pragma once

#include "common/models/app_models.h"
#include "common/utils/string_utils.h"

#include <string>

namespace uam::chat
{
	/// <summary>Preserves conversation and compaction summaries without replayable tool arguments.</summary>
	inline bool BuildConversationHandoff(const ChatSession& source, std::string& context, std::string* error = nullptr)
	{
		std::string snapshot;
		for (const Message& message : source.messages)
		{
			for (const MessageBlock& block : message.blocks)
			{
				if (block.type == "context_compaction" && !strings::IsBlank(block.text))
					snapshot += "Conversation summary: " + block.text + "\n\n";
			}
			if ((message.role == MessageRole::User || message.role == MessageRole::Assistant) && !strings::IsBlank(message.content))
				snapshot += (message.role == MessageRole::User ? "User: " : "Assistant: ") + message.content + "\n\n";
			if (snapshot.size() > 256 * 1024)
			{
				if (error != nullptr) *error = "The conversation is too large to move safely. Compact it before moving.";
				return false;
			}
		}
		context = std::move(snapshot);
		return true;
	}
}
