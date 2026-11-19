#pragma once

#include "common/models/app_models.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace uam::compute_pools
{
	inline constexpr std::size_t kMaxPools = 32;
	inline constexpr std::size_t kMaxMembers = 32;
	inline constexpr std::size_t kMaxNameBytes = 80;
	inline constexpr int kMaxLimit = 64;

	/// <summary>An empty member model covers every model of that provider.</summary>
	inline bool IsMember(const ComputePool& pool, const std::string& provider_id, const std::string& model_id)
	{
		return std::ranges::any_of(pool.members, [&](const ComputePoolMember& member)
		{
			return member.provider_id == provider_id && (member.model_id.empty() || member.model_id == model_id);
		});
	}

	inline bool IsValid(const ComputePool& pool)
	{
		return !pool.id.empty() && !pool.name.empty() && pool.name.size() <= kMaxNameBytes && pool.limit >= 1 && pool.limit <= kMaxLimit &&
		    !pool.members.empty() && pool.members.size() <= kMaxMembers &&
		    std::ranges::none_of(pool.members, [](const ComputePoolMember& member) { return member.provider_id.empty(); });
	}

	/// <summary>Trims names and drops invalid pools, repeated ids and repeated members.</summary>
	inline void Normalize(std::vector<ComputePool>& pools)
	{
		std::set<std::string> ids;
		std::vector<ComputePool> result;
		for (ComputePool& pool : pools)
		{
			pool.id = strings::Trim(pool.id);
			pool.name = strings::Trim(pool.name);
			std::vector<ComputePoolMember> members;
			for (ComputePoolMember& member : pool.members)
			{
				member.provider_id = strings::Trim(member.provider_id);
				member.model_id = strings::Trim(member.model_id);
				if (std::ranges::none_of(members, [&](const ComputePoolMember& seen) { return seen.provider_id == member.provider_id && seen.model_id == member.model_id; }))
					members.push_back(std::move(member));
			}
			pool.members = std::move(members);
			if (result.size() < kMaxPools && IsValid(pool) && ids.insert(pool.id).second) result.push_back(std::move(pool));
		}
		pools = std::move(result);
	}

	inline nlohmann::json Serialize(const std::vector<ComputePool>& pools)
	{
		nlohmann::json result = nlohmann::json::array();
		for (const ComputePool& pool : pools)
		{
			nlohmann::json members = nlohmann::json::array();
			for (const ComputePoolMember& member : pool.members) members.push_back({{"providerId", member.provider_id}, {"modelId", member.model_id}});
			result.push_back({{"id", pool.id}, {"name", pool.name}, {"limit", pool.limit}, {"members", std::move(members)}});
		}
		return result;
	}

	/// <summary>Reads pools without normalizing, so a caller can refuse input that normalization would change.</summary>
	inline std::vector<ComputePool> Parse(const nlohmann::json& value)
	{
		std::vector<ComputePool> pools;
		if (!value.is_array()) return pools;
		for (const nlohmann::json& item : value)
		{
			if (!item.is_object()) continue;
			ComputePool pool;
			pool.id = item.value("id", std::string{});
			pool.name = item.value("name", std::string{});
			pool.limit = item.contains("limit") && item["limit"].is_number_integer() ? item["limit"].get<int>() : 0;
			if (item.contains("members") && item["members"].is_array())
				for (const nlohmann::json& member : item["members"])
					if (member.is_object()) pool.members.push_back({member.value("providerId", std::string{}), member.value("modelId", std::string{})});
			pools.push_back(std::move(pool));
		}
		return pools;
	}
}
