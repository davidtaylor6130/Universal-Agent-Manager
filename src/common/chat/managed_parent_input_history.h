#pragma once
#include "common/models/app_models.h"
#include <algorithm>
#include <charconv>
#include <unordered_map>
#include <unordered_set>

namespace uam::managed_parent_history
{
	// Two escaped 256-byte turn-ID fields plus bounded fixed JSON/reason overhead.
	inline constexpr std::size_t kAckReservedBytes = 2048;
	inline bool Token(std::string_view value, std::size_t limit = 256)
	{
		return !value.empty() && value.size() <= limit &&
		    std::ranges::none_of(value, [](unsigned char c) { return c <= 32 || c == 127; });
	}
	inline bool RequestId(std::string_view value)
	{
		int parsed = 0;
		const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
		return !value.empty() && result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
		    parsed > 0 && std::to_string(parsed) == value;
	}
	inline bool Valid(const ManagedParentInputReceipt& r)
	{
		return r.version == 1 && Token(r.id) && Token(r.origin_id) && Token(r.root_id) &&
		    r.provider_id == "codex-cli" && Token(r.host_id) && !r.workspace.empty() && r.workspace.size() <= 16384 &&
		    Token(r.native_thread) && Token(r.origin_turn) && !r.payload.empty() && r.payload.size() <= 72 * 1024 &&
		    (r.native_request_id.empty() || RequestId(r.native_request_id)) &&
		    (r.accepted_turn_id.empty() || (Token(r.accepted_turn_id) && RequestId(r.native_request_id) && r.accepted_turn_id != r.origin_turn)) && !r.run_ids.empty() && r.run_ids.size() <= 4;
	}
	inline bool Bound(const ManagedParentInputReceipt& r)
	{
		return Valid(r) && !r.accepted_turn_id.empty() && RequestId(r.native_request_id);
	}
	inline bool ValidCollection(const std::vector<ManagedParentInputReceipt>& receipts)
	{
		std::unordered_set<std::string> inputs;
		std::unordered_map<std::string, std::unordered_set<std::string>> origins;
		for (const auto& receipt : receipts)
		{
			if (!Valid(receipt) || !inputs.insert(receipt.id).second) return false;
			auto& runs = origins[receipt.root_id + "\n" + receipt.origin_id];
			std::unordered_set<std::string> batch;
			for (const auto& id : receipt.run_ids)
				if (!Token(id) || !batch.insert(id).second || !runs.insert(id).second) return false;
			if (runs.size() > 4) return false;
		}
		return true;
	}
	inline bool Scope(const ManagedParentInputReceipt& r, const ChatSession& owner,
	    std::string_view native_thread, std::string_view workspace)
	{
		return Valid(r) && r.root_id == owner.id && r.provider_id == owner.provider_id &&
		    r.host_id == owner.execution_host_id && r.workspace == owner.workspace_directory &&
		    r.workspace == workspace && r.native_thread == native_thread && r.native_thread == owner.native_session_id;
	}
	inline const ManagedParentInputReceipt* FindBound(const ChatSession& owner, std::string_view native_thread,
	    std::string_view workspace, std::string_view turn, std::string_view raw)
	{
		if (!Token(turn)) return nullptr;
		const ManagedParentInputReceipt* found = nullptr;
		for (const auto& receipt : owner.managed_parent_input_receipts)
			if (Bound(receipt) && Scope(receipt, owner, native_thread, workspace) && receipt.accepted_turn_id == turn && receipt.payload == raw)
			{
				if (found) return nullptr; // Ambiguous receipt provenance never removes a User projection.
				found = &receipt;
			}
		return found;
	}
}
