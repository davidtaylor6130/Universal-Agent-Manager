#pragma once

#include <nlohmann/json.hpp>

#include "common/paths/path_utils.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uam::codex
{
	inline constexpr const char* kSessionIndexFilename = "session_index.jsonl";
	inline constexpr int kMaxRolloutPreludeLines = 12;

	struct SessionIndexEntry
	{
		std::string id;
		std::string updated_at;
	};

	inline std::filesystem::path CodexHomePath()
	{
		if (const std::optional<std::filesystem::path> codex_home = uam::env::GetTrimmedPath("CODEX_HOME"))
		{
			return *codex_home;
		}
#if defined(_WIN32)
		if (const std::optional<std::filesystem::path> user_profile = uam::env::GetTrimmedPath("USERPROFILE"))
		{
			return *user_profile / ".codex";
		}
#endif
		if (const std::optional<std::filesystem::path> home = uam::env::GetTrimmedPath("HOME"))
		{
			return *home / ".codex";
		}
		return uam::paths::CurrentPathOrDot() / ".codex";
	}

	inline std::optional<SessionIndexEntry> ParseSessionIndexLine(std::string_view line)
	{
		line = uam::strings::TrimAsciiView(line);
		if (line.empty())
		{
			return std::nullopt;
		}

		try
		{
			const nlohmann::json parsed = nlohmann::json::parse(line.begin(), line.end());
			SessionIndexEntry entry;
			entry.id = ValidThreadIdOrEmpty(uam::nlohmann_json::StringViewOrEmpty(parsed, "id"));
			entry.updated_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(parsed, "updated_at")};
			if (!entry.id.empty())
			{
				return entry;
			}
		}
		catch (const nlohmann::json::exception&)
		{
			// Skip malformed JSONL records; the next valid line can still identify a session.
		}

		return std::nullopt;
	}

	inline bool SessionIndexEntrySortsBefore(const SessionIndexEntry& lhs, const SessionIndexEntry& rhs)
	{
		if (lhs.updated_at.empty() != rhs.updated_at.empty())
		{
			return lhs.updated_at.empty();
		}

		if (lhs.updated_at == rhs.updated_at)
		{
			return false;
		}

		return lhs.updated_at < rhs.updated_at;
	}

	inline void SortSessionIndexOldestToNewest(std::vector<SessionIndexEntry>& entries)
	{
		std::ranges::stable_sort(entries, SessionIndexEntrySortsBefore);
	}

	inline std::vector<SessionIndexEntry> ReadSessionIndex(const std::filesystem::path& codex_home = CodexHomePath())
	{
		std::vector<SessionIndexEntry> entries;
		uam::io::ForEachTextFileLine(codex_home / kSessionIndexFilename,
		                             [&entries](const std::string& line)
		                             {
			                             if (!line.empty())
			                             {
				                             if (std::optional<SessionIndexEntry> entry = ParseSessionIndexLine(line))
				                             {
					                             entries.push_back(std::move(*entry));
				                             }
			                             }
			                             return true;
		                             });
		return entries;
	}

	inline std::vector<std::string> ReadSessionIndexIds(const std::filesystem::path& codex_home = CodexHomePath())
	{
		std::vector<std::string> ids;
		const std::vector<SessionIndexEntry> entries = ReadSessionIndex(codex_home);
		ids.reserve(entries.size());

		for (const SessionIndexEntry& entry : entries)
		{
			ids.push_back(entry.id);
		}
		return ids;
	}

	inline bool PathsMatch(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
	{
		const std::filesystem::path lhs_compare = uam::paths::NormalizeExistingPath(lhs);
		const std::filesystem::path rhs_compare = uam::paths::NormalizeExistingPath(rhs);
#if defined(_WIN32) || defined(__APPLE__) || defined(__MACH__)
		const std::string lhs_text = uam::strings::ToLowerAscii(uam::paths::PortablePathString(lhs_compare));
		const std::string rhs_text = uam::strings::ToLowerAscii(uam::paths::PortablePathString(rhs_compare));
		return lhs_text == rhs_text;
#else
		return lhs_compare == rhs_compare;
#endif
	}

	inline bool RolloutFileNameMatchesSession(const std::filesystem::path& rollout_file, std::string_view session_id)
	{
		const std::string valid_session_id = ValidThreadIdOrEmpty(session_id);
		return !valid_session_id.empty() && uam::strings::Contains(rollout_file.filename().string(), valid_session_id);
	}

	inline std::optional<std::filesystem::path> FindRolloutFileForSession(std::string_view session_id, const std::filesystem::path& codex_home = CodexHomePath())
	{
		const std::string valid_session_id = ValidThreadIdOrEmpty(session_id);
		if (valid_session_id.empty())
		{
			return std::nullopt;
		}

		const std::filesystem::path sessions_root = codex_home / "sessions";
		if (!uam::paths::PathExistsNoThrow(sessions_root))
		{
			return std::nullopt;
		}

		std::error_code iterator_error;
		constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
		for (std::filesystem::recursive_directory_iterator it(sessions_root, options, iterator_error), end; !iterator_error && it != end; it.increment(iterator_error))
		{
			if (!uam::paths::IsRegularFileEntryNoThrow(*it))
			{
				continue;
			}
			if (RolloutFileNameMatchesSession(it->path(), valid_session_id))
			{
				return it->path();
			}
		}
		return std::nullopt;
	}

	inline bool RolloutCwdMatches(std::string_view session_id, const std::filesystem::path& cwd, const std::filesystem::path& codex_home = CodexHomePath())
	{
		const std::string valid_session_id = ValidThreadIdOrEmpty(session_id);
		if (valid_session_id.empty())
		{
			return false;
		}

		const std::optional<std::filesystem::path> rollout = FindRolloutFileForSession(valid_session_id, codex_home);
		if (!rollout)
		{
			return false;
		}

		bool matched = false;
		int line_count = 0;
		uam::io::ForEachTextFileLine(*rollout,
		                             [&matched, &line_count, &valid_session_id, &cwd](const std::string& line)
		                             {
			                             if (line_count >= kMaxRolloutPreludeLines)
			                             {
				                             return false;
			                             }
			                             ++line_count;
			                             try
			                             {
				                             const nlohmann::json parsed = nlohmann::json::parse(line);
				                             if (uam::nlohmann_json::TrimmedStringViewOrEmpty(parsed, "type") != "session_meta")
				                             {
					                             return true;
				                             }
				                             const nlohmann::json payload = parsed.value("payload", nlohmann::json::object());
				                             if (ValidThreadIdOrEmpty(uam::nlohmann_json::StringViewOrEmpty(payload, "id")) != valid_session_id)
				                             {
					                             return true;
				                             }
				                             const std::string recorded_cwd{uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "cwd")};
				                             matched = !recorded_cwd.empty() && PathsMatch(recorded_cwd, cwd);
				                             return false;
			                             }
			                             catch (const nlohmann::json::exception&)
			                             {
				                             // Ignore malformed rollout prelude lines and keep looking for session metadata.
				                             return true;
			                             }
		                             });
		return matched;
	}

	inline std::string PickNewSessionId(const std::vector<std::string>& ids_before, const std::filesystem::path& cwd, const std::filesystem::path& codex_home = CodexHomePath())
	{
		const std::unordered_set<std::string> before(ids_before.begin(), ids_before.end());
		std::vector<SessionIndexEntry> entries = ReadSessionIndex(codex_home);
		SortSessionIndexOldestToNewest(entries);
		std::string discovered_id;

		for (const SessionIndexEntry& entry : entries)
		{
			if (before.contains(entry.id) || !RolloutCwdMatches(entry.id, cwd, codex_home))
			{
				continue;
			}
			if (!discovered_id.empty())
			{
				return "";
			}
			discovered_id = entry.id;
		}
		return discovered_id;
	}
} // namespace uam::codex
