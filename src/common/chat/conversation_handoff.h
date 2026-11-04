#pragma once

#include "common/models/app_models.h"
#include "common/utils/string_utils.h"

#include <string>

namespace uam::chat
{
	inline constexpr const char* kAmbiguousCompactionHistory = "The interrupted compaction history cannot be safely reconstructed. The saved summary and input are retained.";

	inline bool HandoffHasQuarantinedSummary(const Message& message)
	{
		if (message.role != MessageRole::Assistant) return false;
		for (const MessageBlock& block : message.blocks)
			if (block.type == "context_compaction_summary") return true;
		return false;
	}

	/// <summary>Uses native block order only when every saved content byte is represented.</summary>
	inline bool HandoffUsesBlockOrder(const Message& message)
	{
		if (message.role != MessageRole::Assistant) return false;
		std::size_t ordinary_offset = 0, complete_offset = 0;
		bool ordinary_matches = true, complete_matches = true, has_compaction = false;
		for (const MessageBlock& block : message.blocks)
		{
			// Quarantined summary bytes prove ordering but never become ordinary reply context.
			const bool compaction = block.type == "context_compaction" || block.type == "context_compaction_interrupted" || block.type == "context_compaction_summary";
			if (block.type != "assistant_text" && !compaction) continue;
			has_compaction = has_compaction || compaction;
			if (complete_matches)
			{
				complete_matches = message.content.compare(complete_offset, block.text.size(), block.text) == 0;
				if (complete_matches) complete_offset += block.text.size();
			}
			if (ordinary_matches && block.type == "assistant_text")
			{
				ordinary_matches = message.content.compare(ordinary_offset, block.text.size(), block.text) == 0;
				if (ordinary_matches) ordinary_offset += block.text.size();
			}
		}
		return has_compaction && ((ordinary_matches && ordinary_offset == message.content.size()) ||
		    (complete_matches && complete_offset == message.content.size()));
	}

	/// <summary>Preserves conversation and compaction summaries without replayable tool arguments.</summary>
	inline bool BuildConversationHandoff(const ChatSession& source, std::string& context, std::string* error = nullptr)
	{
		std::string snapshot;
		for (const Message& message : source.messages)
		{
			if (message.acp_prompt_discarded) continue;
			const bool ordered = HandoffUsesBlockOrder(message);
			if (!ordered && HandoffHasQuarantinedSummary(message))
			{
				if (error != nullptr) *error = kAmbiguousCompactionHistory;
				return false;
			}
			if (ordered)
			{
				for (const MessageBlock& block : message.blocks)
				{
					if ((block.type == "context_compaction" || block.type == "context_compaction_interrupted") && !strings::IsBlank(block.text))
						snapshot += (block.type == "context_compaction_interrupted" ? "Interrupted conversation summary: " : "Conversation summary: ") + block.text + "\n\n";
					else if (block.type == "assistant_text" && !strings::IsBlank(block.text))
						snapshot += "Assistant: " + block.text + "\n\n";
				}
			}
			else
			{
				for (const MessageBlock& block : message.blocks)
					if ((block.type == "context_compaction" || block.type == "context_compaction_interrupted") && !strings::IsBlank(block.text))
						snapshot += (block.type == "context_compaction_interrupted" ? "Interrupted conversation summary: " : "Conversation summary: ") + block.text + "\n\n";
				if ((message.role == MessageRole::User || message.role == MessageRole::Assistant) && !strings::IsBlank(message.content))
					snapshot += (message.role == MessageRole::User ? "User: " : "Assistant: ") + message.content + "\n\n";
			}
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
