#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_ids.h"
#include "common/utils/history_timestamp_order.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <regex>
#include <string>
#include <string_view>

namespace uam::chat_import
{
	namespace detail
	{
		inline std::string LowerAscii(std::string_view text)
		{
			std::string lowered(text);
			std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lowered;
		}

		/// <summary>Forward slashes, no trailing separator; Windows-style paths (drive letter or UNC) also lower-cased.</summary>
		inline std::string ComparablePath(std::string_view path)
		{
			std::string text(path);
			std::ranges::replace(text, '\\', '/');
			while (text.size() > 1 && text.back() == '/' && !(text.size() == 3 && text[1] == ':')) text.pop_back();
			const bool windows = (text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':') || text.starts_with("//");
			return windows ? LowerAscii(text) : text;
		}
	}

	/// <summary>
	/// True when the user's Chat Data import filters exclude this native session: its provider title starts with an
	/// excluded prefix, ignoring case, or its workspace or worktree is an excluded folder or inside one, on any host.
	/// Matching is on whole folder names, so "C:/work" does not exclude "C:/workshop".
	/// </summary>
	inline bool NativeImportExcludedBySettings(const AppSettings& settings, const ChatSession& chat)
	{
		if (!settings.native_import_excluded_title_prefixes.empty())
		{
			const std::string title = detail::LowerAscii(chat.title);
			const std::size_t start = title.find_first_not_of(" \t\r\n");
			const std::string_view trimmed = start == std::string::npos ? std::string_view{} : std::string_view(title).substr(start);
			for (const std::string& prefix : settings.native_import_excluded_title_prefixes)
				if (!prefix.empty() && trimmed.starts_with(detail::LowerAscii(prefix))) return true;
		}
		for (const std::string* path : {&chat.workspace_directory, &chat.workspace_worktree_directory})
		{
			if (path->empty()) continue;
			const std::string workspace = detail::ComparablePath(*path);
			for (const std::string& directory : settings.native_import_excluded_directories)
			{
				const std::string folder = detail::ComparablePath(directory);
				// A bare root (including "\\" collapsing to "/") would exclude every session; ignore it.
				if (folder.empty() || folder == "/") continue;
				if (workspace == folder) return true;
				const std::string parent = folder.ends_with('/') ? folder : folder + '/';
				if (workspace.starts_with(parent)) return true;
			}
		}
		return false;
	}

	/// <summary>
	/// True when startup import should leave this session for explicit discovery: its last recorded activity is
	/// before the user's startup age limit (exactly the limit is still offered). A missing or unreadable time never hides a session.
	/// </summary>
	inline bool NativeImportOlderThanStartupLimit(const AppSettings& settings, const ChatSession& chat, std::time_t now)
	{
		if (settings.native_import_startup_max_age_days <= 0) return false;
		const std::time_t cutoff = now - static_cast<std::time_t>(settings.native_import_startup_max_age_days) * 86400;
		const std::string cutoff_text = uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(cutoff), uam::time::kIsoUtcTimestampFormat);
		const std::string& activity = chat.updated_at.empty() ? chat.created_at : chat.updated_at;
		return uam::time::CompareHistoryTimestamps(activity, cutoff_text).order == uam::time::HistoryTimestampOrder::Before;
	}

	/// <summary>
	/// True when startup import should leave this session for explicit discovery because it still has its provider's
	/// generated default name. Only OpenCode generates one; the pattern is the one OpenCode uses to recognise a generated
	/// title, so any other title, including "New session" typed by a user, is kept. Other providers name sessions from
	/// their content, and an OpenCode session with no stored title is named from its first message during discovery.
	/// </summary>
	inline bool NativeImportHasProviderDefaultTitle(const ChatSession& chat)
	{
		static const std::regex open_code_default(R"((New session|Child session) - \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
		return chat.provider_id == provider_ids::kOpenCodeCli && std::regex_match(chat.title, open_code_default);
	}
}
