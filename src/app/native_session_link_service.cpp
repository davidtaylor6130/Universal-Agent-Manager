#include "native_session_link_service.h"

#include "app/application_core_helpers.h"
#include "common/paths/app_paths.h"
#include "common/provider/codex/cli/codex_thread_id.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace
{
	constexpr long long kLinkTimeToleranceMs = 2LL * 60LL * 1000LL;
	constexpr long long kAmbiguousTimeGapMs = 5000LL;

	bool MessagesEquivalentForNativeLinking(const Message& local_message, const Message& native_message)
	{
		return local_message.role == native_message.role && Trim(local_message.content) == Trim(native_message.content);
	}

	bool IsMessagePrefixForNativeLinking(const std::vector<Message>& local_messages, const std::vector<Message>& native_messages)
	{
		if (local_messages.empty() || local_messages.size() > native_messages.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < local_messages.size(); ++i)
		{
			if (!MessagesEquivalentForNativeLinking(local_messages[i], native_messages[i]))
			{
				return false;
			}
		}

		return true;
	}

	bool MessagesExactlyMatchForNativeLinking(const std::vector<Message>& local_messages, const std::vector<Message>& native_messages)
	{
		return local_messages.size() == native_messages.size() && IsMessagePrefixForNativeLinking(local_messages, native_messages);
	}

	std::string FirstUserMessageTextForNativeLinking(const ChatSession& chat)
	{
		for (const Message& message : chat.messages)
		{
			if (message.role == MessageRole::User)
			{
				return Trim(message.content);
			}
		}

		return "";
	}

	std::optional<long long> ParseDraftTimestampMs(const std::string& chat_id)
	{
		if (chat_id.rfind("chat-", 0) != 0)
		{
			return std::nullopt;
		}

		const std::size_t start = 5;
		const std::size_t end = chat_id.find('-', start);

		if (end == std::string::npos || end <= start)
		{
			return std::nullopt;
		}

		const std::string epoch_ms = chat_id.substr(start, end - start);
		char* parse_end = nullptr;
		const long long parsed = std::strtoll(epoch_ms.c_str(), &parse_end, 10);

		if (parse_end == nullptr || *parse_end != '\0')
		{
			return std::nullopt;
		}

		return parsed;
	}

	std::optional<long long> ParseLocalTimestampMs(const std::string& timestamp)
	{
		if (timestamp.empty())
		{
			return std::nullopt;
		}

		std::tm tm{};
		std::istringstream in(timestamp);
		in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

		if (in.fail())
		{
			return std::nullopt;
		}

		tm.tm_isdst = -1;
		const std::time_t parsed = std::mktime(&tm);

		if (parsed == static_cast<std::time_t>(-1))
		{
			return std::nullopt;
		}

		return static_cast<long long>(parsed) * 1000LL;
	}

	std::time_t TimegmPortable(std::tm* tm)
	{
#if defined(_WIN32)
		return _mkgmtime(tm);
#else
		return timegm(tm);
#endif
	}

	std::optional<long long> ParseZuluTimestampMs(const std::string& timestamp)
	{
		if (timestamp.size() < 20 || timestamp[10] != 'T')
		{
			return std::nullopt;
		}

		std::string core = timestamp.substr(0, 19);
		std::tm tm{};
		std::istringstream in(core);
		in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

		if (in.fail())
		{
			return std::nullopt;
		}

		long long millis = 0;
		std::size_t pos = 19;

		if (pos < timestamp.size() && timestamp[pos] == '.')
		{
			++pos;
			std::size_t ms_start = pos;

			while (pos < timestamp.size() && std::isdigit(static_cast<unsigned char>(timestamp[pos])) != 0)
			{
				++pos;
			}

			std::string fractional = timestamp.substr(ms_start, pos - ms_start);

			if (!fractional.empty())
			{
				while (fractional.size() < 3)
				{
					fractional.push_back('0');
				}

				if (fractional.size() > 3)
				{
					fractional = fractional.substr(0, 3);
				}

				millis = std::strtoll(fractional.c_str(), nullptr, 10);
			}
		}

		if (pos >= timestamp.size() || timestamp[pos] != 'Z')
		{
			return std::nullopt;
		}

		const std::time_t parsed = TimegmPortable(&tm);

		if (parsed == static_cast<std::time_t>(-1))
		{
			return std::nullopt;
		}

		return static_cast<long long>(parsed) * 1000LL + millis;
	}

	std::optional<long long> TimestampForDraftLinking(const ChatSession& chat)
	{
		if (const auto draft_time = ParseDraftTimestampMs(chat.id); draft_time.has_value())
		{
			return draft_time;
		}

		if (const auto local_time = ParseLocalTimestampMs(chat.created_at); local_time.has_value())
		{
			return local_time;
		}

		return ParseZuluTimestampMs(chat.created_at);
	}

	struct MatchCandidate
	{
		std::string session_id;
		int message_rank = -1;
		long long time_diff_ms = std::numeric_limits<long long>::max();
	};

	bool IsCandidateBetterForNativeLinking(const MatchCandidate& lhs, const MatchCandidate& rhs)
	{
		if (lhs.message_rank != rhs.message_rank)
		{
			return lhs.message_rank > rhs.message_rank;
		}

		if (lhs.time_diff_ms != rhs.time_diff_ms)
		{
			return lhs.time_diff_ms < rhs.time_diff_ms;
		}

		return lhs.session_id < rhs.session_id;
	}

	bool IsWorkspaceCompatibleForNativeLinking(const ChatSession& local_chat, const ChatSession& native_chat)
	{
		if (Trim(local_chat.workspace_directory).empty() || Trim(native_chat.workspace_directory).empty())
		{
			return true;
		}

		return FolderDirectoryMatches(local_chat.workspace_directory, native_chat.workspace_directory);
	}

} // namespace

bool NativeSessionLinkService::IsLocalDraftChatId(const std::string& chat_id) const
{
	return chat_id.rfind("chat-", 0) == 0;
}

	bool NativeSessionLinkService::HasRealNativeSessionId(const ChatSession& chat) const
	{
		const std::string native_session_id = Trim(chat.native_session_id);
		if (native_session_id.empty() || IsLocalDraftChatId(native_session_id))
		{
			return false;
		}
		if (Trim(chat.provider_id) == "codex-cli")
		{
			return uam::codex::IsValidThreadId(native_session_id);
		}
		return true;
	}

std::optional<std::string> NativeSessionLinkService::MatchNativeSessionIdForLocalDraft(const ChatSession& local_chat,
                                                                                      const std::vector<ChatSession>& native_chats,
                                                                                      const std::unordered_set<std::string>& blocked_ids) const
{
	if (!IsLocalDraftChatId(local_chat.id))
	{
		return std::nullopt;
	}

	const std::string local_first_user = FirstUserMessageTextForNativeLinking(local_chat);
	const bool has_local_messages = !local_chat.messages.empty();
	const std::optional<long long> local_time_ms = TimestampForDraftLinking(local_chat);
	std::vector<MatchCandidate> candidates;

	for (const ChatSession& native_chat : native_chats)
	{
		if (!HasRealNativeSessionId(native_chat))
		{
			continue;
		}

		if (blocked_ids.find(native_chat.native_session_id) != blocked_ids.end())
		{
			continue;
		}

		if (!IsWorkspaceCompatibleForNativeLinking(local_chat, native_chat))
		{
			continue;
		}

		MatchCandidate candidate;
		candidate.session_id = native_chat.native_session_id;

		if (has_local_messages)
		{
			const std::string native_first_user = FirstUserMessageTextForNativeLinking(native_chat);

			if (!local_first_user.empty())
			{
				if (native_first_user.empty() || native_first_user != local_first_user)
				{
					continue;
				}

				candidate.message_rank = 1;
			}

			if (local_chat.messages.size() >= 2)
			{
				if (MessagesExactlyMatchForNativeLinking(local_chat.messages, native_chat.messages))
				{
					candidate.message_rank = 3;
				}
				else if (IsMessagePrefixForNativeLinking(local_chat.messages, native_chat.messages))
				{
					candidate.message_rank = 2;
				}
				else
				{
					continue;
				}
			}
		}
		else
		{
			candidate.message_rank = 0;
		}

		if (candidate.message_rank < 0)
		{
			continue;
		}

		if (local_time_ms.has_value())
		{
			if (const auto native_time_ms = TimestampForDraftLinking(native_chat); native_time_ms.has_value())
			{
				candidate.time_diff_ms = std::llabs(local_time_ms.value() - native_time_ms.value());
			}
		}

		candidates.push_back(std::move(candidate));
	}

	if (candidates.empty())
	{
		return std::nullopt;
	}

	std::sort(candidates.begin(), candidates.end(), IsCandidateBetterForNativeLinking);
	const MatchCandidate& best = candidates.front();

	if (best.message_rank == 0)
	{
		if (best.time_diff_ms == std::numeric_limits<long long>::max() || best.time_diff_ms > kLinkTimeToleranceMs)
		{
			return std::nullopt;
		}

		int candidates_within_tolerance = 0;

		for (const MatchCandidate& candidate : candidates)
		{
			if (candidate.time_diff_ms <= kLinkTimeToleranceMs)
			{
				++candidates_within_tolerance;
			}
		}

		return candidates_within_tolerance == 1 ? std::optional<std::string>(best.session_id) : std::nullopt;
	}

	if (candidates.size() == 1)
	{
		return best.session_id;
	}

	const MatchCandidate& second = candidates[1];

	if (best.message_rank > second.message_rank)
	{
		return best.session_id;
	}

	if (best.time_diff_ms != std::numeric_limits<long long>::max() &&
	    second.time_diff_ms != std::numeric_limits<long long>::max() &&
	    best.time_diff_ms + kAmbiguousTimeGapMs < second.time_diff_ms)
	{
		return best.session_id;
	}

	return std::nullopt;
}

std::optional<std::string> NativeSessionLinkService::InferNativeSessionIdForLocalDraft(const ChatSession& local_chat, const std::vector<ChatSession>& native_chats) const
{
	return MatchNativeSessionIdForLocalDraft(local_chat, native_chats);
}

std::vector<std::string> NativeSessionLinkService::CollectNewSessionIds(const std::vector<ChatSession>& loaded_chats, const std::vector<std::string>& existing_ids) const
{
	std::unordered_set<std::string> seen(existing_ids.begin(), existing_ids.end());
	std::vector<std::string> discovered;

	for (const ChatSession& chat : loaded_chats)
	{
		if (HasRealNativeSessionId(chat) && seen.find(chat.native_session_id) == seen.end())
		{
			discovered.push_back(chat.native_session_id);
		}
	}

	return discovered;
}

std::string NativeSessionLinkService::PickFirstUnblockedSessionId(const std::vector<std::string>& candidate_ids, const std::unordered_set<std::string>& blocked_ids) const
{
	for (const std::string& candidate : candidate_ids)
	{
		if (!candidate.empty() && blocked_ids.find(candidate) == blocked_ids.end())
		{
			return candidate;
		}
	}

	return "";
}

bool NativeSessionLinkService::SessionIdExistsInLoadedChats(const std::vector<ChatSession>& loaded_chats, const std::string& session_id) const
{
	if (session_id.empty())
	{
		return false;
	}

	for (const ChatSession& chat : loaded_chats)
	{
		if (!chat.native_session_id.empty() && chat.native_session_id == session_id)
		{
			return true;
		}
	}

	return false;
}
