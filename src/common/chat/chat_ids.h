#pragma once

#include "common/utils/time_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>

namespace uam::chat_ids
{
	namespace detail
	{
		inline constexpr char kLowerHexDigits[] = "0123456789abcdef";
		inline constexpr std::string_view kChatIdPrefix = "chat";
		inline constexpr std::string_view kFolderIdPrefix = "folder";
		inline constexpr std::string_view kLocalDraftChatIdPrefix = "chat-";
		inline constexpr std::size_t kNewChatSuffixDigits = 6;
		inline constexpr std::size_t kNewFolderSuffixDigits = 8;

		inline std::string RandomLowerHexSuffix(std::size_t digit_count)
		{
			std::mt19937 rng(std::random_device{}());
			std::uniform_int_distribution<int> hex_digit(0, 15);

			std::string suffix;
			suffix.reserve(digit_count);
			for (std::size_t i = 0; i < digit_count; ++i)
			{
				suffix.push_back(kLowerHexDigits[hex_digit(rng)]);
			}
			return suffix;
		}

		inline std::string TimestampedHexId(std::string_view prefix, std::size_t suffix_digits)
		{
			const std::string timestamp = std::to_string(uam::time::SystemEpochMillisecondsNow());
			const std::string suffix = RandomLowerHexSuffix(suffix_digits);

			std::string id;
			id.reserve(prefix.size() + 1 + timestamp.size() + 1 + suffix.size());
			id.append(prefix);
			id.push_back('-');
			id.append(timestamp);
			id.push_back('-');
			id.append(suffix);
			return id;
		}
	} // namespace detail

	inline bool IsSafeStorageChatId(std::string_view chat_id)
	{
		if (chat_id.empty() || uam::strings::TrimAsciiView(chat_id) != chat_id || chat_id == "." || chat_id == "..")
		{
			return false;
		}

		const bool has_ascii_space = std::ranges::any_of(chat_id, [](char ch) {
			return uam::strings::IsAsciiSpace(static_cast<unsigned char>(ch));
		});
		const bool has_path_separator = uam::strings::Contains(chat_id, '/') || uam::strings::Contains(chat_id, '\\');
		const bool has_parent_marker = uam::strings::Contains(chat_id, "..");
		return !has_ascii_space && !has_path_separator && !has_parent_marker;
	}

	inline std::string NewChatId()
	{
		return detail::TimestampedHexId(detail::kChatIdPrefix, detail::kNewChatSuffixDigits);
	}

	inline std::string NewFolderId(std::string_view uuid)
	{
		const std::string normalized_uuid = uam::strings::Trim(uuid);
		if (normalized_uuid.empty())
		{
			return detail::TimestampedHexId(detail::kFolderIdPrefix, detail::kNewFolderSuffixDigits);
		}

		std::string id;
		id.reserve(detail::kFolderIdPrefix.size() + 1 + normalized_uuid.size());
		id.append(detail::kFolderIdPrefix);
		id.push_back('-');
		id.append(normalized_uuid);
		return id;
	}

	inline bool IsLocalDraftChatId(std::string_view chat_id)
	{
		return uam::strings::StartsWith(chat_id, detail::kLocalDraftChatIdPrefix);
	}
} // namespace uam::chat_ids
