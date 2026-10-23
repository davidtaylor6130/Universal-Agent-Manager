#include "common/chat/native_media_chat_save.h"

#include "common/chat/chat_ids.h"
#include "common/provider/native_agent_history.h"
#include "common/paths/path_utils.h"
#include "common/chat/chat_repository.h"
#include "common/chat/message_attachment_sources.h"
#include "common/chat/native_attachment_assets.h"
#include "core/native_import_content.h"

#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace uam
{
	namespace
	{
		using ContentIdentity = std::tuple<std::string, std::uintmax_t, std::string>;
		ContentIdentity ContentOf(const MessageAttachment& a) { return {a.content_digest, a.size_bytes, a.mime_type}; }
		bool Fail(std::string& error, const char* reason) { error = reason; return false; }
		bool PendingIsConsistent(const MessageAttachment& a, std::string_view host)
		{
			const auto& p = a.native_inline_payload;
			return a.source == AttachmentSource::NativeInlinePending && p &&
			    a.mime_type == p->MimeType() && a.content_digest == p->Digest() && a.size_bytes == p->DecodedSize() &&
			    a.path.empty() && !a.copied && a.preview_key.empty() && a.preview_origin_path.empty() &&
			    a.source_uri.empty() && a.native_asset_key.empty() && a.origin_execution_host_id == host &&
			    !a.id.empty() && !a.native_part_key.empty() &&
			    attachment_sources::BoundedText(a.id, 16384) && attachment_sources::BoundedText(a.name, 4096) &&
			    attachment_sources::BoundedText(a.native_part_key, 16384) &&
			    attachment_sources::BoundedText(a.native_tool_call_id, 1024) &&
			    attachment_sources::BoundedText(a.native_source_field, 16384) &&
			    attachment_sources::BoundedText(a.origin_execution_host_id, 1024);
		}
		void MakeOwned(MessageAttachment& a, const native_assets::AssetReference& reference)
		{
			a.source = AttachmentSource::OwnedNativeAsset;
			a.native_asset_key = reference.key;
			a.native_inline_payload.reset();
		}
	}

	PreparedNativeMediaChatSave::~PreparedNativeMediaChatSave() = default;

	std::unique_ptr<PreparedNativeMediaChatSave> PreparedNativeMediaChatSave::Prepare(
	    const std::filesystem::path& root, ChatSession detached, std::string_view revision,
	    std::string& error, std::stop_token cancel)
	{
		if (detached.native_session_id.empty())
		{ Fail(error, "Native media preparation requires an admitted loaded conversation and storage revision."); return {}; }
		return PrepareAdmitted(root, std::move(detached), revision, error, cancel);
	}

	std::unique_ptr<PreparedNativeMediaChatSave> PreparedNativeMediaChatSave::PreparePassive(
	    const std::filesystem::path& root, AdmittedLocalPassiveNativeHistory admitted,
	    const PassiveNativeOpenMetadata& metadata, std::string_view revision,
	    std::string& error, std::stop_token cancel)
	{
		const auto reference = native_agents::ParseHistoryId(admitted.m_reference);
		auto& chat = admitted.m_chat;
		if (!reference || reference->execution_host_id != "local" ||
		    !metadata.incarnation || admitted.m_source_id.empty() || admitted.m_source_birth.empty() ||
		    root != admitted.m_root || revision != admitted.m_primary ||
		    ChatRepository::ChatSummaryStorageRevision(root, admitted.m_reference) != admitted.m_summary ||
		    chat.id != admitted.m_reference || chat.provider_id != reference->provider_id ||
		    chat.parent_chat_id != reference->parent_session_id || chat.parent_chat_id != admitted.m_parent_native_id ||
		    !chat.imported_read_only || !chat.native_session_id.empty() || chat.execution_host_id != "local" ||
		    chat.workspace_directory != admitted.m_workspace || !paths::PathFromUtf8(chat.workspace_directory).is_absolute() ||
		    (metadata.display_parent_chat_id.empty() != metadata.display_parent_created_at.empty()) ||
		    ((!chat.subagent_parent_chat_id.empty() || !chat.subagent_parent_created_at.empty()) &&
		     (metadata.display_parent_chat_id != chat.subagent_parent_chat_id || metadata.display_parent_created_at != chat.subagent_parent_created_at)) ||
		    ((!metadata.display_parent_chat_id.empty() || !metadata.display_parent_created_at.empty()) &&
		     !(metadata.display_parent_chat_id == chat.subagent_parent_chat_id && metadata.display_parent_created_at == chat.subagent_parent_created_at) &&
		     !(metadata.display_parent_chat_id == admitted.m_source_id && metadata.display_parent_created_at == admitted.m_source_birth)))
		{ Fail(error, "The original images no longer match this child history. Reopen it and try again."); return {}; }
		chat.metadata_incarnation = metadata.incarnation;
		chat.subagent_parent_chat_id = metadata.display_parent_chat_id;
		chat.subagent_parent_created_at = metadata.display_parent_created_at;
		if (metadata.last_opened_at) chat.last_opened_at = *metadata.last_opened_at;
		return PrepareAdmitted(root, std::move(chat), revision, error, cancel);
	}

	std::unique_ptr<PreparedNativeMediaChatSave> PreparedNativeMediaChatSave::PrepareAdmitted(
	    const std::filesystem::path& root, ChatSession detached, std::string_view revision,
	    std::string& error, std::stop_token cancel)
	try
	{
		error.clear();
		std::error_code filesystem_error;
		if (cancel.stop_requested()) { Fail(error, "Native media preparation was cancelled."); return {}; }
		if (!root.is_absolute() || !std::filesystem::is_directory(root, filesystem_error) || filesystem_error ||
		    !chat_ids::IsSafeStorageChatId(detached.id) || detached.id.size() > 256 ||
		    !detached.messages_loaded || detached.provider_id.empty() ||
		    !HasImportedConversationContent(detached.messages) || revision.empty())
		{ Fail(error, "Native media preparation requires an admitted loaded conversation and storage revision."); return {}; }
		if (ChatRepository::ChatStorageRevision(root, detached.id) != revision)
		{ Fail(error, "Saved chat changed before native media preparation."); return {}; }
		auto result = std::unique_ptr<PreparedNativeMediaChatSave>(new PreparedNativeMediaChatSave());
		result->m_chat = std::move(detached);
		auto& chat = result->m_chat;
		const std::string_view host = chat.execution_host_id.empty() ? std::string_view("local") : chat.execution_host_id;
		std::vector<std::pair<std::size_t, std::size_t>> positions;
		std::size_t bytes = 0;
		std::map<ContentIdentity, native_assets::AssetReference> reusable;
		for (std::size_t row = 0; row < chat.messages.size(); ++row)
		{
			if (cancel.stop_requested()) { Fail(error, "Native media preparation was cancelled."); return {}; }
			for (std::size_t part = 0; part < chat.messages[row].attachments.size(); ++part)
			{
				const auto& a = chat.messages[row].attachments[part];
				if (a.source != AttachmentSource::NativeInlinePending)
				{
					if (!attachment_sources::IsPersistable(a, chat.id))
					{ Fail(error, "Native media source metadata is invalid."); return {}; }
					continue;
				}
				if (chat.workspace_directory.empty() || !PendingIsConsistent(a, host) ||
				    positions.size() >= native_assets::kMaxAssetCount ||
				    a.size_bytes > native_assets::kMaxOriginalBytes || a.size_bytes > native_assets::kMaxBatchBytes - bytes)
				{ Fail(error, "Pending native originals are inconsistent or exceed the operation bounds."); return {}; }
				bytes += static_cast<std::size_t>(a.size_bytes);
				positions.emplace_back(row, part);
				reusable.try_emplace(ContentOf(a));
			}
		}
		for (const auto& prompt : chat.acp_queued_prompts)
			for (const auto& a : prompt.attachments)
				if (!attachment_sources::IsPersistable(a, chat.id))
				{ Fail(error, "Queued native originals require separate input delivery admission."); return {}; }

		// Reuse only existing same-owner originals verified by the scoped byte
		// reader. Identical refreshes do not create another retained generation.
		if (!positions.empty() && revision != "missing")
		{
			const auto previous = ChatRepository::LoadLocalChat(root, chat.id, true, nullptr, false);
			if (previous)
				for (const auto& message : previous->messages)
					for (const auto& a : message.attachments)
					{
						if (cancel.stop_requested()) { Fail(error, "Native media preparation was cancelled."); return {}; }
						const auto found = reusable.find(ContentOf(a));
						if (a.source != AttachmentSource::OwnedNativeAsset || found == reusable.end() ||
						    !found->second.key.empty() || !attachment_sources::IsPersistable(a, chat.id)) continue;
						const auto original = native_assets::ReadOwned(root, chat.id, a.native_asset_key,
						    static_cast<std::size_t>(a.size_bytes), cancel);
						if (original.ok) found->second = {a.native_asset_key, a.mime_type, a.content_digest, static_cast<std::size_t>(a.size_bytes)};
					}
		}
		std::vector<native_assets::Payload> originals;
		std::vector<std::pair<std::size_t, std::size_t>> new_positions;
		for (const auto& [row, part] : positions)
		{
			auto& a = chat.messages[row].attachments[part];
			const auto& old = reusable.at(ContentOf(a));
			if (!old.key.empty()) MakeOwned(a, old);
			else { originals.push_back(a.native_inline_payload); new_positions.emplace_back(row, part); }
		}
		if (!originals.empty())
		{
			result->m_originals = native_assets::PreparedBatch::Prepare(root, chat.id, originals, error, cancel);
			if (!result->m_originals) return {};
			const auto& references = result->m_originals->Assets();
			if (references.size() != new_positions.size()) { Fail(error, "Native original association could not be prepared."); return {}; }
			for (std::size_t index = 0; index < references.size(); ++index)
			{
				const auto [row, part] = new_positions[index];
				MakeOwned(chat.messages[row].attachments[part], references[index]);
			}
		}
		if (cancel.stop_requested()) { Fail(error, "Native media preparation was cancelled."); return {}; }
		result->m_prepared = ChatRepository::PrepareChatSave(root, chat, revision);
		if (!result->m_prepared || result->m_prepared->primary_temp.empty())
		{ Fail(error, "The complete native conversation could not be prepared for its admitted owner."); return {}; }
		if (cancel.stop_requested()) { Fail(error, "Native media preparation was cancelled."); return {}; }
		if (result->m_originals && !result->m_originals->MarkPublicationAttempted(error)) return {};
		if (cancel.stop_requested()) { Fail(error, "Native media preparation was cancelled."); return {}; }
		return result;
	}
	catch (...)
	{
		Fail(error, "Native media chat preparation failed; saved history was not acknowledged.");
		return {};
	}

	bool PreparedNativeMediaChatSave::Publish(std::string& error, const std::function<bool()>& still_owned, bool sync_directory)
	{
		error.clear();
		if (m_publish_attempted || !m_prepared || m_prepared->primary_temp.empty())
			return Fail(error, "This native media publication has already been attempted or is unavailable.");
		m_publish_attempted = true;
		if (!ChatRepository::PublishPreparedChatSave(m_prepared, sync_directory, &m_chat, still_owned))
			return Fail(error, "Native media publication was rejected or was not acknowledged.");
		m_primary_acknowledged = true;
		return true;
	}

	bool PreparedNativeMediaChatSave::AcknowledgeOriginals(std::string& error)
	{
		error.clear();
		if (!m_primary_acknowledged) return Fail(error, "Native originals have no acknowledged primary publication.");
		return !m_originals || m_originals->MarkPublished(error);
	}

	std::optional<ChatSession> PreparedNativeMediaChatSave::TakeCommittedChat()
	{
		if (!m_primary_acknowledged || m_chat_taken) return std::nullopt;
		m_chat_taken = true;
		return std::move(m_chat);
	}
}
