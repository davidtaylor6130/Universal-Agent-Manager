#pragma once
#include "common/models/app_models.h"
#include "common/utils/hash_utils.h"
#include <algorithm>

namespace uam::prompt_recovery
{
	inline constexpr std::size_t kMaxRecords = 32;
	inline constexpr std::size_t kMaxBytes = 2U * 1024U * 1024U;
	inline void AddWitness(std::uint64_t& hash, std::string_view value)
	{
		hashing::UpdateFnv1a64WithSeparator(hash, std::to_string(value.size()));
		hashing::UpdateFnv1a64(hash, value);
	}
	// Stable length-delimited authored bytes, independent of std::hash/platform word size.
	inline std::string RowWitness(const Message& row)
	{
		std::uint64_t hash = hashing::kFnv1a64OffsetBasis;
		for (const std::string* value : {&row.content, &row.created_at, &row.provider}) AddWitness(hash, *value);
		AddWitness(hash, std::to_string(static_cast<int>(row.role)));
		AddWitness(hash, row.priority_steer ? "1" : "0");
		AddWitness(hash, std::to_string(row.markdown_store_files.size()));
		for (const auto& value : row.markdown_store_files) AddWitness(hash, value);
		AddWitness(hash, std::to_string(row.markdown_store_prompt_blocks.size()));
		for (const auto& value : row.markdown_store_prompt_blocks) AddWitness(hash, value);
		AddWitness(hash, std::to_string(row.attachments.size()));
		for (const auto& a : row.attachments)
		{
			for (const std::string* value : {&a.id, &a.name, &a.kind, &a.mime_type, &a.path, &a.preview_key, &a.preview_origin_path,
			         &a.source_uri, &a.origin_execution_host_id, &a.native_part_key, &a.native_tool_call_id, &a.native_source_field,
			         &a.native_asset_key, &a.content_digest}) AddWitness(hash, *value);
			AddWitness(hash, std::to_string(a.size_bytes));
			AddWitness(hash, a.copied ? "1" : "0");
			AddWitness(hash, std::to_string(static_cast<int>(a.source)));
		}
		return hashing::Hex64Padded(hash);
	}
	inline std::size_t PolicyBytes(const AcpUnsentPromptRecovery& value)
	{
		std::size_t bytes = 0;
		for (const std::string* field : {&value.chat_id, &value.provider_id, &value.user_message_created_at, &value.row_witness, &value.admission_id,
		         &value.uam_agent_id, &value.uam_agent_definition_hash, &value.uam_agent_definition_snapshot, &value.uam_agent_instructions,
		         &value.uam_agent_workspace_access, &value.uam_agent_execution_capability}) bytes += field->size();
		for (const auto& field : value.uam_agent_skills) bytes += field.size();
		for (const auto& field : value.uam_agent_delegates) bytes += field.size();
		return bytes;
	}
	inline bool Bounded(const AcpUnsentPromptRecovery& value)
	{
		const auto short_string = [](const std::string& s) { return s.size() <= 256; };
		const auto ids = [](const auto& list)
		{
			return list.size() <= 128 && std::all_of(list.begin(), list.end(), [](const auto& id) { return !id.empty() && id.size() <= 64; });
		};
		return short_string(value.chat_id) && short_string(value.provider_id) && short_string(value.user_message_created_at) &&
		    short_string(value.row_witness) && short_string(value.admission_id) && value.uam_agent_id.size() <= 64 &&
		    short_string(value.uam_agent_definition_hash) && value.uam_agent_definition_snapshot.size() <= 256U * 1024U &&
		    value.uam_agent_instructions.size() <= 192U * 1024U && short_string(value.uam_agent_workspace_access) &&
		    short_string(value.uam_agent_execution_capability) && ids(value.uam_agent_skills) && ids(value.uam_agent_delegates);
	}
	inline bool Bounded(const std::vector<AcpUnsentPromptRecovery>& values)
	{
		if (values.size() > kMaxRecords) return false;
		std::size_t bytes = 0;
		for (const auto& value : values)
		{
			if (!Bounded(value)) return false;
			const auto size = PolicyBytes(value);
			if (size > kMaxBytes - bytes) return false;
			bytes += size;
		}
		return true;
	}
	inline std::string ReceiptWitness(const Message& row, const AcpUnsentPromptRecovery& value)
	{
		std::uint64_t hash = hashing::kFnv1a64OffsetBasis;
		AddWitness(hash, RowWitness(row));
		for (const std::string* part : {&value.chat_id, &value.provider_id, &value.user_message_created_at, &value.admission_id}) AddWitness(hash, *part);
		AddWitness(hash, std::to_string(value.user_message_index));
		AddWitness(hash, std::to_string(value.admission_revision));
		return hashing::Hex64Padded(hash);
	}
	inline bool BindingMatches(const ChatSession& chat, const AcpUnsentPromptRecovery& value)
	{
		if (value.version != 1 || !Bounded(value) || value.chat_id.empty() || value.chat_id != chat.id ||
		    value.provider_id != chat.provider_id || value.admission_id.empty() || value.admission_revision < 1 ||
		    !chat.messages_loaded || value.user_message_index < 0 ||
		    static_cast<std::size_t>(value.user_message_index) >= chat.messages.size()) return false;
		const Message& row = chat.messages[static_cast<std::size_t>(value.user_message_index)];
		return row.role == MessageRole::User && (row.provider.empty() || row.provider == chat.provider_id) &&
		    !value.user_message_created_at.empty() && value.user_message_created_at == row.created_at &&
		    value.row_witness == ReceiptWitness(row, value);
	}
	// Validate the pre-mutation carrier before an owned operation may rebind or retire it.
	inline bool BindingsMatch(const ChatSession& chat)
	{
		if (!Bounded(chat.acp_unsent_prompt_recoveries)) return false;
		std::vector<int> indices;
		for (const auto& value : chat.acp_unsent_prompt_recoveries)
		{
			if (!BindingMatches(chat, value) || std::ranges::find(indices, value.user_message_index) != indices.end()) return false;
			indices.push_back(value.user_message_index);
		}
		return true;
	}
	inline bool HasCapturedPolicy(const AcpQueuedUserPromptState& prompt)
	{
		return !prompt.uam_agent_definition_hash.empty() || !prompt.uam_agent_definition_snapshot.empty() || !prompt.uam_agent_instructions.empty();
	}
	inline void CopyPolicy(const AcpUnsentPromptRecovery& source, AcpQueuedUserPromptState& target)
	{
		target.uam_agent_id = source.uam_agent_id;
		target.uam_agent_definition_hash = source.uam_agent_definition_hash;
		target.uam_agent_definition_snapshot = source.uam_agent_definition_snapshot;
		target.uam_agent_instructions = source.uam_agent_instructions;
		target.uam_agent_skills = source.uam_agent_skills;
		target.uam_agent_delegates = source.uam_agent_delegates;
		target.uam_agent_workspace_access = source.uam_agent_workspace_access;
		target.uam_agent_execution_capability = source.uam_agent_execution_capability;
	}
	inline AcpUnsentPromptRecovery Capture(const ChatSession& chat, int index, const AcpQueuedUserPromptState& prompt)
	{
		AcpUnsentPromptRecovery value;
		value.chat_id = chat.id; value.provider_id = chat.provider_id;
		value.user_message_index = index;
		const Message& row = chat.messages[static_cast<std::size_t>(index)];
		value.user_message_created_at = row.created_at;
		value.admission_id = prompt.id; value.admission_revision = prompt.revision;
		value.row_witness = ReceiptWitness(row, value);
		value.uam_agent_id = prompt.uam_agent_id;
		value.uam_agent_definition_hash = prompt.uam_agent_definition_hash;
		value.uam_agent_definition_snapshot = prompt.uam_agent_definition_snapshot;
		value.uam_agent_instructions = prompt.uam_agent_instructions;
		value.uam_agent_skills = prompt.uam_agent_skills;
		value.uam_agent_delegates = prompt.uam_agent_delegates;
		value.uam_agent_workspace_access = prompt.uam_agent_workspace_access;
		value.uam_agent_execution_capability = prompt.uam_agent_execution_capability;
		return value;
	}
}
