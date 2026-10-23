#include "common/chat/branch_native_assets.h"

#include "common/chat/chat_ids.h"
#include "common/chat/chat_repository.h"
#include "common/chat/message_attachment_sources.h"
#include "common/chat/native_attachment_assets.h"
#include "common/utils/base64.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace uam
{
	namespace
	{
		bool Fail(std::string& error, const char* message)
		{
			error = message;
			return false;
		}
		bool Retained(const std::optional<std::vector<std::string>>& ids, const MessageAttachment& attachment)
		{
			return !ids || std::ranges::find(*ids, attachment.id) != ids->end();
		}
		bool ValidSkillSelection(const Message& source, const std::optional<std::vector<std::string>>& retained)
		{
			if (!retained || retained->empty())
				return true;
			if (retained->size() > 128 || source.markdown_store_files.size() != source.markdown_store_prompt_blocks.size())
				return false;
			std::set<std::string> selected;
			for (const auto& file : *retained)
				if (file.empty() || file.size() > 16384 || !selected.insert(file).second || std::ranges::count(source.markdown_store_files, file) != 1)
					return false;
			for (std::size_t index = 0; index < source.markdown_store_files.size(); ++index)
				if (selected.contains(source.markdown_store_files[index]) && source.markdown_store_prompt_blocks[index].empty())
					return false;
			return true;
		}
	} // namespace

	bool BranchPrefixHasOwnedOriginals(const ChatSession& source, int message_index)
	{
		if (!source.messages_loaded || message_index < 0 || static_cast<std::size_t>(message_index) >= source.messages.size())
			return false;
		for (int row = 0; row <= message_index; ++row)
			for (const auto& attachment : source.messages[static_cast<std::size_t>(row)].attachments)
				if (attachment.source == AttachmentSource::OwnedNativeAsset)
					return true;
		return false;
	}

	bool BranchRequestAttachmentsEquivalent(const std::vector<MessageAttachment>& source, const std::vector<MessageAttachment>& branch, const std::string& source_owner, const std::string& branch_owner)
	{
		if (source.size() != branch.size())
			return false;
		for (std::size_t index = 0; index < source.size(); ++index)
		{
			const auto& left = source[index];
			const auto& right = branch[index];
			if (left.source != AttachmentSource::OwnedNativeAsset && right.source != AttachmentSource::OwnedNativeAsset)
			{
				if (left != right)
					return false;
				continue;
			}
			if (left.source != AttachmentSource::OwnedNativeAsset || right.source != AttachmentSource::OwnedNativeAsset || !attachment_sources::IsPersistable(left, source_owner) || !attachment_sources::IsPersistable(right, branch_owner))
				return false;
			auto normalized = left;
			normalized.native_asset_key = right.native_asset_key;
			if (normalized != right)
				return false;
		}
		return true;
	}

	PreparedBranchOriginals::~PreparedBranchOriginals() = default;

	std::unique_ptr<PreparedBranchOriginals> PreparedBranchOriginals::Prepare(const std::filesystem::path& root, const ChatSession& source, const std::string& final_branch_id, int message_index, const std::optional<std::vector<std::string>>& retained_attachment_ids, std::string& error, std::stop_token cancel, const std::optional<std::vector<std::string>>& retained_markdown_store_files)
	try
	{
		error.clear();
		if (cancel.stop_requested() || !root.is_absolute() || !chat_ids::IsSafeStorageChatId(source.id) || !chat_ids::IsSafeStorageChatId(final_branch_id) || source.id == final_branch_id || !source.messages_loaded || message_index < 0 || static_cast<std::size_t>(message_index) >= source.messages.size() || source.messages[static_cast<std::size_t>(message_index)].role != MessageRole::User || ChatRepository::ChatStorageRevision(root, final_branch_id) != std::optional<std::string>("missing"))
		{
			Fail(error, "The branch original owner or selected source is unavailable.");
			return {};
		}
		if (!ValidSkillSelection(source.messages[static_cast<std::size_t>(message_index)], retained_markdown_store_files))
		{
			Fail(error, "Branch skill selection or captured prompt snapshots changed.");
			return {};
		}
		if (retained_attachment_ids)
		{
			std::set<std::string> seen;
			for (const auto& id : *retained_attachment_ids)
				if (id.empty() || !seen.insert(id).second || std::ranges::count(source.messages[static_cast<std::size_t>(message_index)].attachments, id, &MessageAttachment::id) != 1)
				{
					Fail(error, "Branch attachment selection changed.");
					return {};
				}
		}
		auto result = std::unique_ptr<PreparedBranchOriginals>(new PreparedBranchOriginals());
		result->m_root = root;
		result->m_source_owner = source.id;
		result->m_final_owner = final_branch_id;
		result->m_message_index = message_index;
		result->m_retained = retained_attachment_ids;
		result->m_retained_skills = retained_markdown_store_files;
		for (int row = 0; row <= message_index; ++row)
		{
			const auto& message = source.messages[static_cast<std::size_t>(row)];
			result->m_skill_prefix.push_back({message.markdown_store_files, message.markdown_store_prompt_blocks});
		}
		const auto budget = std::make_shared<native_assets::PayloadBudget>();
		std::vector<native_assets::Payload> payloads;
		std::map<std::pair<std::string, std::string>, std::size_t> references;
		std::vector<std::size_t> associations;
		for (int row = 0; row <= message_index; ++row)
		{
			const auto& attachments = source.messages[static_cast<std::size_t>(row)].attachments;
			std::size_t branch_part = 0;
			for (std::size_t part = 0; part < attachments.size(); ++part)
			{
				const auto& attachment = attachments[part];
				if (row == message_index && !Retained(retained_attachment_ids, attachment))
					continue;
				const auto target_part = branch_part++;
				if (attachment.source != AttachmentSource::OwnedNativeAsset)
					continue;
				if (cancel.stop_requested() || !attachment_sources::IsPersistable(attachment, source.id) || result->m_parts.size() >= native_assets::kMaxAssetCount)
				{
					Fail(error, "Branch original references changed or exceed the operation bound.");
					return {};
				}
				// The persisted key identifies bytes, not a part's MIME declaration.
				const auto payload_identity = std::make_pair(attachment.native_asset_key, attachment.mime_type);
				const auto found = references.find(payload_identity);
				std::size_t reference = 0;
				if (found != references.end())
				{
					reference = found->second;
					const auto& payload = payloads[reference];
					if (payload->MimeType() != attachment.mime_type || payload->Digest() != attachment.content_digest || payload->DecodedSize() != attachment.size_bytes)
					{
						Fail(error, "Repeated branch original declarations disagree.");
						return {};
					}
				}
				else
				{
					if (budget->UsedCount() >= native_assets::kMaxAssetCount || attachment.size_bytes > native_assets::kMaxBatchBytes - budget->UsedBytes())
					{
						Fail(error, "Branch originals exceed the operation budget.");
						return {};
					}
					const auto original = native_assets::ReadOwned(root, source.id, attachment.native_asset_key, native_assets::kMaxOriginalBytes, cancel);
					if (!original.ok || original.bytes.size() != attachment.size_bytes)
					{
						Fail(error, "A retained branch original is unavailable or invalid.");
						return {};
					}
					auto payload = native_assets::MakeInlinePayload(base64::Encode(original.bytes), attachment.mime_type, budget, error, cancel);
					if (!payload || payload->Digest() != attachment.content_digest || payload->DecodedSize() != attachment.size_bytes)
					{
						Fail(error, "A retained branch original does not match its declaration.");
						return {};
					}
					reference = payloads.size();
					references.emplace(payload_identity, reference);
					payloads.push_back(std::move(payload));
				}
				result->m_parts.push_back({static_cast<std::size_t>(row), part, target_part, attachment, {}});
				associations.push_back(reference);
			}
		}
		if (cancel.stop_requested())
		{
			Fail(error, "Branch original preparation was cancelled.");
			return {};
		}
		if (!payloads.empty())
		{
			result->m_batch = native_assets::PreparedBatch::Prepare(root, final_branch_id, payloads, error, cancel);
			if (!result->m_batch)
				return {};
			for (std::size_t index = 0; index < result->m_parts.size(); ++index)
				result->m_parts[index].replacement_key = result->m_batch->Assets().at(associations[index]).key;
			if (cancel.stop_requested())
			{
				Fail(error, "Branch original preparation was cancelled.");
				return {};
			}
			if (!result->m_batch->MarkPublicationAttempted(error))
				return {};
		}
		if (cancel.stop_requested())
		{
			Fail(error, "Branch original preparation was cancelled.");
			return {};
		}
		result->m_ready = true;
		return result;
	}
	catch (...)
	{
		Fail(error, "Branch original preparation failed; originals were not acknowledged.");
		return {};
	}

	bool PreparedBranchOriginals::MatchesRequest(const std::filesystem::path& root, const std::string& source_id, int message_index, const std::optional<std::vector<std::string>>& retained_attachment_ids, const std::optional<std::vector<std::string>>& retained_markdown_store_files) const
	{
		return m_ready && !m_consumed && root == m_root && source_id == m_source_owner && message_index == m_message_index && retained_attachment_ids == m_retained && retained_markdown_store_files == m_retained_skills;
	}

	bool PreparedBranchOriginals::MatchesSource(const ChatSession& source) const
	{
		if (!m_ready || source.id != m_source_owner || !source.messages_loaded || m_message_index < 0 || static_cast<std::size_t>(m_message_index) >= source.messages.size())
			return false;
		for (std::size_t row = 0; row < m_skill_prefix.size(); ++row)
			if (source.messages[row].markdown_store_files != m_skill_prefix[row].files || source.messages[row].markdown_store_prompt_blocks != m_skill_prefix[row].blocks)
				return false;
		std::size_t count = 0;
		for (int row = 0; row <= m_message_index; ++row)
			for (const auto& attachment : source.messages[static_cast<std::size_t>(row)].attachments)
				if ((row != m_message_index || Retained(m_retained, attachment)) && attachment.source == AttachmentSource::OwnedNativeAsset)
					++count;
		if (count != m_parts.size())
			return false;
		for (const auto& part : m_parts)
		{
			const auto& attachments = source.messages[part.row].attachments;
			if (part.source_part >= attachments.size() || attachments[part.source_part] != part.expected)
				return false;
		}
		return true;
	}

	bool PreparedBranchOriginals::ApplyTo(ChatSession& candidate, std::string& error)
	{
		if (!m_ready || m_consumed || candidate.id != m_final_owner || candidate.parent_chat_id != m_source_owner || candidate.branch_from_message_index != m_message_index || candidate.messages.size() != static_cast<std::size_t>(m_message_index + 1))
			return Fail(error, "The prepared branch original owner changed.");
		for (std::size_t row = 0; row < m_skill_prefix.size(); ++row)
		{
			const auto& original = m_skill_prefix[row];
			const auto& message = candidate.messages[row];
			if (static_cast<int>(row) != m_message_index || !m_retained_skills)
			{
				if (message.markdown_store_files != original.files || message.markdown_store_prompt_blocks != original.blocks)
					return Fail(error, "The branch skill snapshots changed.");
				continue;
			}
			std::size_t selected = 0;
			for (std::size_t index = 0; index < original.files.size(); ++index)
				if (std::ranges::find(*m_retained_skills, original.files[index]) != m_retained_skills->end())
				{
					if (selected >= message.markdown_store_files.size() || selected >= message.markdown_store_prompt_blocks.size() || message.markdown_store_files[selected] != original.files[index] || message.markdown_store_prompt_blocks[selected] != original.blocks[index])
						return Fail(error, "The selected branch skill snapshots changed.");
					++selected;
				}
			if (selected != message.markdown_store_files.size() || selected != message.markdown_store_prompt_blocks.size())
				return Fail(error, "The selected branch skill snapshots changed.");
		}
		std::size_t count = 0;
		for (const auto& message : candidate.messages)
			for (const auto& attachment : message.attachments)
				if (attachment.source == AttachmentSource::OwnedNativeAsset)
					++count;
		if (count != m_parts.size())
			return Fail(error, "The retained branch originals changed.");
		for (const auto& part : m_parts)
		{
			const auto& attachments = candidate.messages[part.row].attachments;
			if (part.branch_part >= attachments.size() || attachments[part.branch_part] != part.expected)
				return Fail(error, "The retained branch original association changed.");
			auto rebound = part.expected;
			rebound.native_asset_key = part.replacement_key;
			if (!attachment_sources::IsPersistable(rebound, m_final_owner))
				return Fail(error, "The prepared branch original reference is invalid.");
		}
		for (const auto& part : m_parts)
			candidate.messages[part.row].attachments[part.branch_part].native_asset_key = part.replacement_key;
		m_consumed = true;
		return true;
	}

	bool PreparedBranchOriginals::AcknowledgeOriginals(std::string& error)
	{
		error.clear();
		if (!m_primary_acknowledged)
			return Fail(error, "Branch originals have no acknowledged primary publication.");
		return !m_batch || m_batch->MarkPublished(error);
	}
} // namespace uam
