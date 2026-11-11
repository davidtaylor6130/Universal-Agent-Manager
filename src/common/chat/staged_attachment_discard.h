#pragma once

#include "common/models/app_models.h"
#include "common/state/app_state.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace uam::staged_attachment_discard
{
	inline std::string Portable(std::string path)
	{
		std::ranges::replace(path, '\\', '/');
		return path;
	}

	/// True only for `.UAM/attachments/<chat id>/<file>`: a copy this chat staged, never a directory reference or any other path.
	inline bool IsStagedCopyPath(const std::string& path, const std::string& chat_id)
	{
		const std::string portable = Portable(path);
		const std::string root = ".UAM/attachments/" + chat_id + "/";
		return !chat_id.empty() && portable.size() > root.size() && portable.starts_with(root) &&
		       portable.find('/', root.size()) == std::string::npos && portable.find('\0') == std::string::npos &&
		       portable.substr(root.size()) != ".." && portable.substr(root.size()) != ".";
	}

	/// Copies staged in this run that no send has named yet. Only these may be discarded with their chip: a path
	/// leaves the set the moment any send request names it (even one later refused or still preparing), and after a
	/// restart the set is empty, so a doubtful copy is always kept rather than deleted.
	enum class Unsent { Stage, Submit, TakeForDiscard };
	inline bool ExchangeUnsent(const std::string& path, const Unsent change)
	{
		static std::mutex mutex;
		static std::set<std::string> unsent;
		const std::scoped_lock lock(mutex);
		if (change == Unsent::Stage) return unsent.insert(Portable(path)).second;
		return unsent.erase(Portable(path)) > 0;
	}

	/// True when any chat has sent, queued or recorded the copy, so removing an unsent chip must leave it on disk.
	inline bool IsReferenced(const uam::AppState& app, const std::string& path)
	{
		const std::string portable = Portable(path);
		const auto same = [&portable](const std::string& other) { return Portable(other) == portable; };
		const auto in_attachments = [&same](const std::vector<MessageAttachment>& attachments)
		{
			return std::ranges::any_of(attachments, [&same](const MessageAttachment& attachment) { return same(attachment.path); });
		};
		const auto in_chat = [&](const ChatSession& chat)
		{
			return std::ranges::any_of(chat.linked_files, same) ||
			       std::ranges::any_of(chat.messages, [&](const auto& message) { return in_attachments(message.attachments); }) ||
			       std::ranges::any_of(chat.acp_queued_prompts, [&](const auto& prompt) { return in_attachments(prompt.attachments); });
		};
		if (std::ranges::any_of(app.chats, in_chat) || std::ranges::any_of(app.excluded_provider_chats, in_chat))
		{
			return true;
		}
		return std::ranges::any_of(app.acp_sessions, [&](const auto& session)
		{
			return session != nullptr && std::ranges::any_of(session->queued_user_prompts, [&](const auto& prompt) { return in_attachments(prompt.attachments); });
		});
	}
} // namespace uam::staged_attachment_discard
