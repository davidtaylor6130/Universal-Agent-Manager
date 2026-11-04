#pragma once

#include "common/config/build_features.h"
#include "common/models/app_models.h"
#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/// A build that excludes a provider treats its saved chats and sub-agent runs, and everything under them, as if they did not
/// exist. Their files stay on disk untouched and they never enter the visible chat list or run ledger, so no view, count, search,
/// phone state, bulk action or cleanup reaches them; the chats are kept aside only so import and deletion still respect the native
/// sessions and workspaces they own. A build that includes the provider loads them again.
namespace uam::excluded_provider_data
{
	/// <summary>True when this build excludes the provider a saved record names. Aliases of included providers are not excluded.</summary>
	inline bool IsExcluded(std::string_view saved_provider_id)
	{
		return uam::provider_ids::IsRetainedExcludedProviderId(uam::provider_ids::CanonicalCliProviderLookupId(saved_provider_id));
	}

	/// <summary>Moves out records `excluded` selects and, transitively, records whose parent link names a moved one.</summary>
	template <typename Record, typename Excluded, typename ParentLinks>
	std::vector<Record> TakeWithDescendants(std::vector<Record>& records, Excluded excluded, ParentLinks parent_links)
	{
		std::vector<Record> taken;
#if UAM_ALL_PROVIDERS_INCLUDED
		(void)records;
		(void)excluded;
		(void)parent_links;
#else
		std::unordered_set<std::string> hidden;
		for (const Record& record : records)
			if (excluded(record)) hidden.insert(record.id);
		// ponytail: one pass per nesting level; branch and sub-agent trees are a few levels deep.
		for (bool grew = !hidden.empty(); grew;)
		{
			grew = false;
			for (const Record& record : records)
				if (!hidden.contains(record.id))
					for (const std::string* parent : parent_links(record))
						if (hidden.contains(*parent)) { grew = hidden.insert(record.id).second || grew; break; }
		}
		if (hidden.empty()) return taken;
		const auto kept = std::ranges::stable_partition(records, [&](const Record& record) { return !hidden.contains(record.id); });
		taken.assign(std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()));
		records.erase(kept.begin(), kept.end());
#endif
		return taken;
	}

	/// <summary>An excluded provider's chats, including legacy OpenCode chats saved without a provider id, and their branch,
	/// sub-agent, temporary and goal chats.</summary>
	inline std::vector<ChatSession> TakeChats(std::vector<ChatSession>& chats)
	{
		return TakeWithDescendants(chats, [](const ChatSession& chat)
		{
			return IsExcluded(chat.provider_id) || (!UAM_ENABLE_RUNTIME_OPENCODE_CLI && uam::strings::IsBlank(chat.provider_id) && !chat.native_session_id.empty());
		}, [](const ChatSession& chat)
		{
			return std::array{&chat.parent_chat_id, &chat.subagent_parent_chat_id, &chat.temporary_parent_chat_id, &chat.goal_owner_chat_id};
		});
	}

	inline void RemoveRuns(std::vector<AgentRun>& runs)
	{
		(void)TakeWithDescendants(runs, [](const AgentRun& run) { return IsExcluded(run.provider_id); }, [](const AgentRun& run) { return std::array{&run.parent_run_id}; });
	}

	/// <summary>Runs whose root or transcript chat is hidden, and runs under them, are hidden too, so none is delivered or listed.</summary>
	inline void RemoveRunsOfHiddenChats(std::vector<AgentRun>& runs, const std::vector<ChatSession>& hidden_chats)
	{
		if (hidden_chats.empty()) return;
		std::unordered_set<std::string> hidden;
		for (const ChatSession& chat : hidden_chats) hidden.insert(chat.id);
		(void)TakeWithDescendants(runs, [&](const AgentRun& run) { return hidden.contains(run.root_chat_id) || hidden.contains(run.transcript_chat_id); },
		    [](const AgentRun& run) { return std::array{&run.parent_run_id}; });
	}
} // namespace uam::excluded_provider_data
