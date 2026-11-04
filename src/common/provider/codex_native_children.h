#pragma once

#include "common/models/app_models.h"
#include "common/provider/codex_thread_id.h"
#include "common/utils/nlohmann_json_utils.h"

#include <unordered_map>
#include <unordered_set>

namespace uam::codex
{
	/// <summary>Retains validated receivers in native order, with only their own reported states.</summary>
	inline std::vector<SubAgentChild> CollaborationChildren(const nlohmann::json& item)
	{
		std::vector<SubAgentChild> children;
		if (uam::nlohmann_json::StringViewOrEmpty(item, "type") != "collabAgentToolCall") return children;
		const auto* receivers = uam::nlohmann_json::FindArrayField(item, "receiverThreadIds");
		const std::string parent_key = uam::codex::ThreadIdKey(uam::nlohmann_json::StringViewOrEmpty(item, "senderThreadId"));
		std::unordered_map<std::string, std::string> child_statuses;
		if (const auto states = item.find("agentsStates"); states != item.end() && states->is_object())
		{
			for (auto state = states->begin(); state != states->end(); ++state)
			{
				const std::string key = uam::codex::ThreadIdKey(state.key());
				const std::string_view status = uam::nlohmann_json::StringViewOrEmpty(state.value(), "status");
				if (!key.empty() && !status.empty()) child_statuses.emplace(key, status);
			}
		}
		std::unordered_set<std::string> seen;
		if (receivers != nullptr)
		{
			for (const auto& receiver : *receivers)
			{
				if (!receiver.is_string()) continue;
				const std::string child_id = uam::codex::ValidThreadIdOrEmpty(receiver.get_ref<const std::string&>());
				const std::string key = uam::codex::ThreadIdKey(child_id);
				if (key.empty() || key == parent_key || !seen.insert(key).second) continue;
				SubAgentChild child;
				child.id = child_id;
				if (const auto status = child_statuses.find(key); status != child_statuses.end()) child.status = status->second;
				children.push_back(std::move(child));
			}
		}
		return children;
	}
}
