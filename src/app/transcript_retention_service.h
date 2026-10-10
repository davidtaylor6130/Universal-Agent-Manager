#pragma once

#include "common/state/app_state.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam
{
	/// <summary>Release only durable, inactive transcripts which the desktop no longer displays.</summary>
	class TranscriptRetentionService
	{
	public:
		void SetVisibleChatIds(const std::vector<std::string>& chat_ids);
		void Invalidate(const std::string& chat_id);
		std::optional<std::uint64_t> BeginRelease(const AppState& app, const std::string& chat_id);
		bool CompleteRelease(AppState& app, const std::string& chat_id, std::uint64_t generation, const ChatSession& persisted);

	private:
		bool CanRelease(const AppState& app, const ChatSession& chat) const;
		bool m_visibilityKnown = false;
		std::unordered_set<std::string> m_visibleChatIds;
		std::unordered_map<std::string, std::uint64_t> m_generations;
	};
}
