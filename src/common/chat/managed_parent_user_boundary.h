#pragma once

#include "common/models/app_models.h"
#include "common/platform/platform_incremental_sha256.h"

#include <nlohmann/json.hpp>

#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace uam::managed_parent_history
{
	/// <summary>
	/// The one list of attachment fields the boundary digest covers, in digest order. It works on a MessageAttachment
	/// and on a UserBoundaryAttachment alike (same member names), so capture, comparison and the digest JSON all read it.
	/// </summary>
	template <class Attachment> auto BoundaryAttachmentFields(Attachment& item)
	{
		return std::tie(item.id, item.name, item.kind, item.mime_type, item.path, item.size_bytes, item.source, item.source_uri,
		    item.content_digest, item.origin_execution_host_id, item.native_part_key, item.native_tool_call_id,
		    item.native_source_field, item.native_asset_key);
	}

	/// <summary>The one list of user-row fields the digest covers ahead of the row's attachments, in digest order.</summary>
	template <class Row> auto BoundaryRowFields(Row& row)
	{
		return std::tie(row.created_at, row.content, row.markdown_store_files, row.markdown_store_prompt_blocks);
	}

	struct UserBoundaryAttachment
	{
		std::string id, name, kind, mime_type, path;
		std::uintmax_t size_bytes = 0;
		AttachmentSource source = AttachmentSource::LegacyPath;
		std::string source_uri, content_digest, origin_execution_host_id, native_part_key, native_tool_call_id, native_source_field, native_asset_key;
	};

	/// <summary>One user row as the managed-parent origin boundary digest covers it.</summary>
	struct UserBoundaryRow
	{
		std::string created_at, content;
		std::vector<std::string> markdown_store_files, markdown_store_prompt_blocks;
		std::vector<UserBoundaryAttachment> attachments;
	};

	inline UserBoundaryRow CaptureBoundaryRow(const Message& row)
	{
		UserBoundaryRow out;
		BoundaryRowFields(out) = BoundaryRowFields(row);
		out.attachments.resize(row.attachments.size());
		for (std::size_t index = 0; index < row.attachments.size(); ++index)
			BoundaryAttachmentFields(out.attachments[index]) = BoundaryAttachmentFields(row.attachments[index]);
		return out;
	}

	/// <summary>Whether a live user row has exactly the captured row's digested fields; compares in place, copies nothing.</summary>
	inline bool SameBoundaryRow(const Message& row, const UserBoundaryRow& captured)
	{
		if (row.attachments.size() != captured.attachments.size() || BoundaryRowFields(row) != BoundaryRowFields(captured)) return false;
		for (std::size_t index = 0; index < row.attachments.size(); ++index)
			if (BoundaryAttachmentFields(row.attachments[index]) != BoundaryAttachmentFields(captured.attachments[index])) return false;
		return true;
	}

	namespace detail
	{
		template <class Value> nlohmann::json BoundaryJson(const Value& value)
		{
			if constexpr (std::is_same_v<Value, AttachmentSource>) return static_cast<int>(value);
			else return value;
		}
		template <class Tuple> void AppendBoundaryJson(nlohmann::json& out, const Tuple& fields)
		{
			std::apply([&](const auto&... value) { (out.push_back(BoundaryJson(value)), ...); }, fields);
		}
	}

	/// <summary>The digest's JSON for one row: the byte layout origin_user_digest has always been taken over.</summary>
	inline nlohmann::json BoundaryRowJson(const UserBoundaryRow& row)
	{
		nlohmann::json attachments = nlohmann::json::array();
		for (const UserBoundaryAttachment& item : row.attachments)
		{
			nlohmann::json value = nlohmann::json::array();
			detail::AppendBoundaryJson(value, BoundaryAttachmentFields(item));
			attachments.push_back(std::move(value));
		}
		nlohmann::json value = nlohmann::json::array();
		detail::AppendBoundaryJson(value, BoundaryRowFields(row));
		value.push_back(std::move(attachments));
		return value;
	}

	/// <summary>True when a user row may count towards the boundary at all; the digest refuses the whole boundary otherwise.</summary>
	inline bool BoundaryUserRowUsable(const Message& row)
	{
		return !row.created_at.empty() && !row.acp_prompt_not_sent && !row.acp_prompt_discarded && !row.interrupted;
	}

	/// <summary>
	/// SHA-256 over every user row up to now, empty when the boundary is unusable (not loaded, no user row, an unsent,
	/// discarded or interrupted user row, over 1 MiB or 4,096 rows). Taken once when a continuation lease is created;
	/// rows_out receives the rows it covers so later polls compare them instead of hashing again.
	/// </summary>
	inline std::string UserBoundaryDigest(const ChatSession& chat, std::vector<UserBoundaryRow>* rows_out = nullptr)
	{
		if (!chat.messages_loaded || chat.messages.empty()) return {};
		nlohmann::json rows = nlohmann::json::array();
		std::vector<UserBoundaryRow> captured;
		std::size_t bytes = 0;
		for (const Message& row : chat.messages)
		{
			if (row.role != MessageRole::User) continue;
			if (!BoundaryUserRowUsable(row)) return {};
			UserBoundaryRow boundary = CaptureBoundaryRow(row);
			nlohmann::json value = BoundaryRowJson(boundary);
			bytes += value.dump().size();
			if (bytes > 1024 * 1024 || rows.size() >= 4096) return {};
			rows.push_back(std::move(value));
			captured.push_back(std::move(boundary));
		}
		if (rows.empty()) return {};
		platform::IncrementalSha256 hash;
		std::string digest = hash.Add(rows.dump()) ? hash.Finish() : std::string{};
		if (rows_out && !digest.empty()) *rows_out = std::move(captured);
		return digest;
	}

	/// <summary>
	/// Whether the chat's user rows are exactly the captured ones, in order, which is when UserBoundaryDigest would
	/// return the digest taken with them: the captured rows already passed its usability and size limits, so equal rows
	/// pass them again. One pass over the transcript comparing fields in place; nothing is serialised or copied.
	/// </summary>
	inline bool UserBoundaryMatches(const ChatSession& chat, const std::vector<UserBoundaryRow>& captured)
	{
		if (captured.empty() || !chat.messages_loaded || chat.messages.empty()) return false;
		std::size_t next = 0;
		for (const Message& row : chat.messages)
		{
			if (row.role != MessageRole::User) continue;
			if (!BoundaryUserRowUsable(row) || next == captured.size() || !SameBoundaryRow(row, captured[next++])) return false;
		}
		return next == captured.size();
	}
} // namespace uam::managed_parent_history
