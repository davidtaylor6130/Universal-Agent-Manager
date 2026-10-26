#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace uam::chat_storage
{
	// Admission only: never normalize or rewrite a primary/journal, infer nondelivery,
	// release a held queue, or transfer a remote/managed execution capability.
	inline bool StoppedFolderTransferAdmissible(const nlohmann::json& record, std::string& error)
	{
		const auto refuse = [&error]()
		{
			error = "Saved execution ownership is unresolved. Settle prepared input, managed work and remote recovery in the current folder before transferring it.";
			return false;
		};
		if (!record.is_object())
			return refuse();
		if (record.contains("remote_pending_requests") && (!record["remote_pending_requests"].is_array() || !record["remote_pending_requests"].empty()))
			return refuse();
		if (record.contains("remote_interaction_responses") && (!record["remote_interaction_responses"].is_array() || !record["remote_interaction_responses"].empty()))
			return refuse();
		// The repository reconstructs pending responses from these legacy fields only
		// when the modern array is absent. A partial or malformed fallback stays held.
		if (!record.contains("remote_interaction_responses"))
			for (const auto* field : {"remote_interaction_response_request_id", "remote_interaction_response_json"})
				if (record.contains(field) && (!record[field].is_string() || !record[field].get_ref<const std::string&>().empty()))
					return refuse();
		for (const auto* field : {"remote_active_turn_id", "remote_prompt_delivery_session_id", "remote_prompt_delivery_id", "remote_prompt_delivery_payload"})
			if (record.contains(field) && (!record[field].is_string() || !record[field].get_ref<const std::string&>().empty()))
				return refuse();
		for (const auto* field : {"remote_process_exists", "remote_stop_cleanup_pending", "remote_restart_pending", "remote_source_exit_pending", "remote_turn_reconnect_pending", "remote_manual_compaction_pending"})
		{
			if (record.contains(field) && (!record[field].is_boolean() || record[field].get<bool>()))
				return refuse();
		}
		if (record.contains("acp_dispatched_queued_prompt_count") && (!record["acp_dispatched_queued_prompt_count"].is_number_integer() || record["acp_dispatched_queued_prompt_count"] != 0))
			return refuse();
		if (record.contains("acp_queued_prompts"))
		{
			if (!record["acp_queued_prompts"].is_array())
				return refuse();
			for (const auto& prompt : record["acp_queued_prompts"])
			{
				if (!prompt.is_object())
					return refuse();
				if (prompt.contains("prepared_for_delivery") && (!prompt["prepared_for_delivery"].is_boolean() || prompt["prepared_for_delivery"].get<bool>()))
					return refuse();
				// The owning decoder treats any marker presence as unresolved, including
				// malformed/null identity. Ordinary unmanaged serialization omits the key.
				if (prompt.contains("managed_turn"))
					return refuse();
			}
		}
		return true;
	}
} // namespace uam::chat_storage
