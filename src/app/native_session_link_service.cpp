#include "native_session_link_service.h"

#include "common/paths/app_paths.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/provider_ids.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{
	constexpr long long kLinkTimeToleranceMs = 2LL * 60LL * 1000LL;
	constexpr long long kAmbiguousTimeGapMs = 5000LL;
	constexpr long long kUnknownTimeDiffMs = std::numeric_limits<long long>::max();
	constexpr std::string_view kLocalDraftChatIdPrefix = "chat-";
	constexpr int kNoMessageMatchRank = -1;
	constexpr int kTimestampOnlyMatchRank = 0;
	constexpr int kFirstUserMessageMatchRank = 1;
	constexpr int kMessagePrefixMatchRank = 2;
	constexpr int kExactMessagesMatchRank = 3;

	bool MessagesEquivalentForNativeLinking(const Message& local_message, const Message& native_message)
	{
		return local_message.role == native_message.role && uam::strings::Trim(local_message.content) == uam::strings::Trim(native_message.content);
	}

	bool IsLocalDraftChatIdValue(std::string_view chat_id)
	{
		return uam::strings::StartsWith(uam::strings::TrimAsciiView(chat_id), kLocalDraftChatIdPrefix);
	}

	std::string NormalizeNativeSessionId(std::string_view session_id)
	{
		return uam::strings::Trim(session_id);
	}

	std::unordered_set<std::string> NormalizeNativeSessionIdSet(const std::unordered_set<std::string>& values)
	{
		std::unordered_set<std::string> normalized_values;
		normalized_values.reserve(values.size());
		for (const std::string& value : values)
		{
			const std::string normalized_value = NormalizeNativeSessionId(value);
			if (!normalized_value.empty())
			{
				normalized_values.insert(normalized_value);
			}
		}
		return normalized_values;
	}

	std::optional<std::string> RealNativeSessionIdForLinking(const ChatSession& chat)
	{
		const std::string native_session_id = NormalizeNativeSessionId(chat.native_session_id);
		if (native_session_id.empty() || IsLocalDraftChatIdValue(native_session_id))
		{
			return std::nullopt;
		}

		if (uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli) && !uam::codex::IsValidThreadId(native_session_id))
		{
			return std::nullopt;
		}

		return native_session_id;
	}

	bool IsMessagePrefixForNativeLinking(const std::vector<Message>& local_messages, const std::vector<Message>& native_messages)
	{
		if (local_messages.empty() || local_messages.size() > native_messages.size())
		{
			return false;
		}

		const auto native_prefix = std::ranges::subrange(native_messages.begin(), native_messages.begin() + static_cast<std::ptrdiff_t>(local_messages.size()));
		return std::ranges::equal(local_messages, native_prefix, MessagesEquivalentForNativeLinking);
	}

	bool MessagesExactlyMatchForNativeLinking(const std::vector<Message>& local_messages, const std::vector<Message>& native_messages)
	{
		return local_messages.size() == native_messages.size() && IsMessagePrefixForNativeLinking(local_messages, native_messages);
	}

	std::string FirstUserMessageTextForNativeLinking(const ChatSession& chat)
	{
		const auto found = std::ranges::find_if(chat.messages, [](const Message& message) {
			return message.role == MessageRole::User;
		});
		if (found != chat.messages.end())
		{
			return uam::strings::Trim(found->content);
		}

		return "";
	}

	std::optional<long long> ParseDraftTimestampMs(std::string_view chat_id)
	{
		const std::string_view draft_id = uam::strings::TrimAsciiView(chat_id);
		if (!IsLocalDraftChatIdValue(draft_id))
		{
			return std::nullopt;
		}

		const std::size_t start = kLocalDraftChatIdPrefix.size();
		const std::size_t end = draft_id.find('-', start);

		if (end == std::string::npos || end <= start)
		{
			return std::nullopt;
		}

		return uam::parse::NonNegativeLongLongStrict(draft_id.substr(start, end - start));
	}

	std::optional<std::tm> ParseTimestampCore(const std::string& value, const char* format)
	{
		std::tm tm{};
		std::istringstream in(value);
		in >> std::get_time(&tm, format);

		if (in.fail())
		{
			return std::nullopt;
		}

		return tm;
	}

	std::optional<long long> ParseLocalTimestampMs(const std::string& timestamp)
	{
		const std::string trimmed = uam::strings::Trim(timestamp);
		constexpr std::string_view kTimestampShape = "YYYY-MM-DD HH:MM:SS";
		constexpr const char* kTimestampFormat = "%Y-%m-%d %H:%M:%S";
		if (trimmed.empty() || trimmed.size() != kTimestampShape.size())
		{
			return std::nullopt;
		}

		std::optional<std::tm> tm = ParseTimestampCore(trimmed, kTimestampFormat);
		if (!tm)
		{
			return std::nullopt;
		}

		tm->tm_isdst = -1;
		const std::time_t parsed = std::mktime(&*tm);

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

	std::optional<long long> ParseZuluTimestampMs(std::string_view timestamp)
	{
		const std::string trimmed = uam::strings::Trim(timestamp);
		constexpr const char* kTimestampFormat = "%Y-%m-%dT%H:%M:%S";
		if (trimmed.size() < 20 || trimmed[10] != 'T')
		{
			return std::nullopt;
		}

		const std::string core = trimmed.substr(0, 19);
		std::optional<std::tm> tm = ParseTimestampCore(core, kTimestampFormat);
		if (!tm)
		{
			return std::nullopt;
		}

		long long millis = 0;
		std::size_t pos = 19;

		if (pos < trimmed.size() && trimmed[pos] == '.')
		{
			++pos;
			std::size_t ms_start = pos;

			while (pos < trimmed.size() && uam::strings::IsAsciiDigit(static_cast<unsigned char>(trimmed[pos])))
			{
				++pos;
			}

			std::string fractional = trimmed.substr(ms_start, pos - ms_start);

			if (fractional.empty())
			{
				return std::nullopt;
			}

			while (fractional.size() < 3)
			{
				fractional.push_back('0');
			}

			if (fractional.size() > 3)
			{
				fractional = fractional.substr(0, 3);
			}

			const std::optional<long long> parsed_millis = uam::parse::NonNegativeLongLongStrict(fractional);
			if (!parsed_millis)
			{
				return std::nullopt;
			}

			millis = *parsed_millis;
		}

		if (pos >= trimmed.size() || trimmed[pos] != 'Z' || pos + 1 != trimmed.size())
		{
			return std::nullopt;
		}

		const std::time_t parsed = TimegmPortable(&*tm);

		if (parsed == static_cast<std::time_t>(-1))
		{
			return std::nullopt;
		}

		return static_cast<long long>(parsed) * 1000LL + millis;
	}

	std::optional<long long> TimestampForDraftLinking(const ChatSession& chat)
	{
		if (const auto draft_time = ParseDraftTimestampMs(chat.id))
		{
			return draft_time;
		}

		if (const auto local_time = ParseLocalTimestampMs(chat.created_at))
		{
			return local_time;
		}

		return ParseZuluTimestampMs(chat.created_at);
	}

	struct MatchCandidate
	{
		std::string session_id;
		int message_rank = kNoMessageMatchRank;
		long long time_diff_ms = kUnknownTimeDiffMs;
	};

	bool HasKnownTimeDiff(const MatchCandidate& candidate)
	{
		return candidate.time_diff_ms != kUnknownTimeDiffMs;
	}

	bool IsWithinLinkTimeTolerance(const MatchCandidate& candidate)
	{
		return candidate.time_diff_ms <= kLinkTimeToleranceMs;
	}

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
		const std::string local_workspace = uam::strings::Trim(local_chat.workspace_directory);
		const std::string native_workspace = uam::strings::Trim(native_chat.workspace_directory);
		if (local_workspace.empty() || native_workspace.empty())
		{
			return true;
		}

		return FolderDirectoryMatches(local_workspace, native_workspace);
	}

	long long AbsoluteTimestampDiffMs(const long long lhs, const long long rhs)
	{
		return lhs > rhs ? lhs - rhs : rhs - lhs;
	}

} // namespace

bool NativeSessionLinkService::IsLocalDraftChatId(const std::string& chat_id) const
{
	return IsLocalDraftChatIdValue(chat_id);
}

bool NativeSessionLinkService::HasRealNativeSessionId(const ChatSession& chat) const
{
	return RealNativeSessionIdForLinking(chat).has_value();
}

std::string NativeSessionLinkService::RealNativeSessionId(const ChatSession& chat) const
{
	return RealNativeSessionIdForLinking(chat).value_or("");
}

std::optional<std::string> NativeSessionLinkService::MatchNativeSessionIdForLocalDraft(const ChatSession& local_chat, const std::vector<ChatSession>& native_chats, const std::unordered_set<std::string>& blocked_ids) const
{
	if (!IsLocalDraftChatId(local_chat.id))
	{
		return std::nullopt;
	}

	const std::string local_first_user = FirstUserMessageTextForNativeLinking(local_chat);
	const bool has_local_messages = !local_chat.messages.empty();
	const std::optional<long long> local_time_ms = TimestampForDraftLinking(local_chat);
	const std::unordered_set<std::string> normalized_blocked_ids = NormalizeNativeSessionIdSet(blocked_ids);
	std::vector<MatchCandidate> candidates;
	candidates.reserve(native_chats.size());

	for (const ChatSession& native_chat : native_chats)
	{
		const std::optional<std::string> native_session_id = RealNativeSessionIdForLinking(native_chat);
		if (!native_session_id)
		{
			continue;
		}

		if (normalized_blocked_ids.contains(*native_session_id))
		{
			continue;
		}

		if (!IsWorkspaceCompatibleForNativeLinking(local_chat, native_chat))
		{
			continue;
		}

		MatchCandidate candidate;
		candidate.session_id = *native_session_id;

		if (has_local_messages)
		{
			const std::string native_first_user = FirstUserMessageTextForNativeLinking(native_chat);

			if (!local_first_user.empty())
			{
				if (native_first_user.empty() || native_first_user != local_first_user)
				{
					continue;
				}

				candidate.message_rank = kFirstUserMessageMatchRank;
			}

			if (local_chat.messages.size() >= 2)
			{
				if (MessagesExactlyMatchForNativeLinking(local_chat.messages, native_chat.messages))
				{
					candidate.message_rank = kExactMessagesMatchRank;
				}
				else if (IsMessagePrefixForNativeLinking(local_chat.messages, native_chat.messages))
				{
					candidate.message_rank = kMessagePrefixMatchRank;
				}
				else
				{
					continue;
				}
			}
		}
		else
		{
			candidate.message_rank = kTimestampOnlyMatchRank;
		}

		if (candidate.message_rank == kNoMessageMatchRank)
		{
			continue;
		}

		if (local_time_ms)
		{
			if (const auto native_time_ms = TimestampForDraftLinking(native_chat))
			{
				candidate.time_diff_ms = AbsoluteTimestampDiffMs(*local_time_ms, *native_time_ms);
			}
		}

		candidates.push_back(std::move(candidate));
	}

	if (candidates.empty())
	{
		return std::nullopt;
	}

	std::ranges::sort(candidates, IsCandidateBetterForNativeLinking);
	const MatchCandidate& best = candidates.front();

	if (best.message_rank == kTimestampOnlyMatchRank)
	{
		if (!IsWithinLinkTimeTolerance(best))
		{
			return std::nullopt;
		}

		const auto candidates_within_tolerance = std::ranges::count_if(candidates, IsWithinLinkTimeTolerance);

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

	if (HasKnownTimeDiff(best) && HasKnownTimeDiff(second) && best.time_diff_ms + kAmbiguousTimeGapMs < second.time_diff_ms)
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
	std::unordered_set<std::string> seen;
	seen.reserve(existing_ids.size() + loaded_chats.size());
	for (const std::string& existing_id : existing_ids)
	{
		const std::string normalized_existing_id = NormalizeNativeSessionId(existing_id);
		if (!normalized_existing_id.empty())
		{
			seen.insert(normalized_existing_id);
		}
	}

	std::vector<std::string> discovered;
	discovered.reserve(loaded_chats.size());

	for (const ChatSession& chat : loaded_chats)
	{
		const std::optional<std::string> native_session_id = RealNativeSessionIdForLinking(chat);
		if (native_session_id && seen.insert(*native_session_id).second)
		{
			discovered.push_back(*native_session_id);
		}
	}

	return discovered;
}

std::string NativeSessionLinkService::PickFirstUnblockedSessionId(const std::vector<std::string>& candidate_ids, const std::unordered_set<std::string>& blocked_ids) const
{
	const std::unordered_set<std::string> normalized_blocked_ids = NormalizeNativeSessionIdSet(blocked_ids);

	for (const std::string& candidate : candidate_ids)
	{
		const std::string normalized_candidate = NormalizeNativeSessionId(candidate);
		if (!normalized_candidate.empty() && !normalized_blocked_ids.contains(normalized_candidate))
		{
			return normalized_candidate;
		}
	}

	return "";
}

bool NativeSessionLinkService::SessionIdExistsInLoadedChats(const std::vector<ChatSession>& loaded_chats, const std::string& session_id) const
{
	const std::string requested_session_id = NormalizeNativeSessionId(session_id);
	if (requested_session_id.empty())
	{
		return false;
	}

	return std::ranges::any_of(loaded_chats, [&requested_session_id](const ChatSession& chat) {
		return RealNativeSessionIdForLinking(chat) == requested_session_id;
	});
}
