#pragma once

#include <nlohmann/json.hpp>

#include "common/provider/codex/cli/codex_thread_id.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace uam::codex
{
	struct SessionIndexEntry
	{
		std::string id;
		std::string updated_at;
	};

	inline std::filesystem::path CodexHomePath()
	{
		if (const char* codex_home = std::getenv("CODEX_HOME"))
		{
			if (std::string(codex_home).size() > 0)
			{
				return std::filesystem::path(codex_home);
			}
		}
#if defined(_WIN32)
		if (const char* user_profile = std::getenv("USERPROFILE"))
		{
			return std::filesystem::path(user_profile) / ".codex";
		}
#endif
		if (const char* home = std::getenv("HOME"))
		{
			return std::filesystem::path(home) / ".codex";
		}
		return std::filesystem::current_path() / ".codex";
	}

	inline std::vector<SessionIndexEntry> ReadSessionIndex(const std::filesystem::path& codex_home = CodexHomePath())
	{
		std::vector<SessionIndexEntry> entries;
		std::ifstream in(codex_home / "session_index.jsonl", std::ios::binary);
		if (!in.good())
		{
			return entries;
		}

		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty())
			{
				continue;
			}
			try
			{
				const nlohmann::json parsed = nlohmann::json::parse(line);
				SessionIndexEntry entry;
				entry.id = parsed.value("id", "");
				entry.updated_at = parsed.value("updated_at", "");
					if (IsValidThreadId(entry.id))
					{
						entries.push_back(std::move(entry));
					}
			}
			catch (...)
			{
			}
		}
		return entries;
	}

	inline std::vector<std::string> ReadSessionIndexIds(const std::filesystem::path& codex_home = CodexHomePath())
	{
		std::vector<std::string> ids;
		for (const SessionIndexEntry& entry : ReadSessionIndex(codex_home))
		{
			ids.push_back(entry.id);
		}
		return ids;
	}

	inline bool PathsMatch(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
	{
		std::error_code lhs_ec;
		std::error_code rhs_ec;
		const std::filesystem::path lhs_canonical = std::filesystem::weakly_canonical(lhs, lhs_ec);
		const std::filesystem::path rhs_canonical = std::filesystem::weakly_canonical(rhs, rhs_ec);
		const std::filesystem::path lhs_compare = lhs_ec ? lhs.lexically_normal() : lhs_canonical.lexically_normal();
		const std::filesystem::path rhs_compare = rhs_ec ? rhs.lexically_normal() : rhs_canonical.lexically_normal();
#if defined(_WIN32) || defined(__APPLE__) || defined(__MACH__)
		std::string lhs_text = lhs_compare.generic_string();
		std::string rhs_text = rhs_compare.generic_string();
		std::transform(lhs_text.begin(), lhs_text.end(), lhs_text.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		std::transform(rhs_text.begin(), rhs_text.end(), rhs_text.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return lhs_text == rhs_text;
#else
		return lhs_compare == rhs_compare;
#endif
	}

	inline std::optional<std::filesystem::path> FindRolloutFileForSession(const std::string& session_id, const std::filesystem::path& codex_home = CodexHomePath())
	{
		if (session_id.empty())
		{
			return std::nullopt;
		}

		const std::filesystem::path sessions_root = codex_home / "sessions";
		if (!std::filesystem::exists(sessions_root))
		{
			return std::nullopt;
		}

		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(sessions_root, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!it->is_regular_file(ec))
			{
				continue;
			}
			const std::string filename = it->path().filename().string();
			if (filename.find(session_id) != std::string::npos)
			{
				return it->path();
			}
		}
		return std::nullopt;
	}

	inline bool RolloutCwdMatches(const std::string& session_id, const std::filesystem::path& cwd, const std::filesystem::path& codex_home = CodexHomePath())
	{
		const std::optional<std::filesystem::path> rollout = FindRolloutFileForSession(session_id, codex_home);
		if (!rollout.has_value())
		{
			return false;
		}

		std::ifstream in(rollout.value(), std::ios::binary);
		if (!in.good())
		{
			return false;
		}

		std::string line;
		for (int i = 0; i < 12 && std::getline(in, line); ++i)
		{
			try
			{
				const nlohmann::json parsed = nlohmann::json::parse(line);
				if (parsed.value("type", "") != "session_meta")
				{
					continue;
				}
				const nlohmann::json payload = parsed.value("payload", nlohmann::json::object());
				if (payload.value("id", "") != session_id)
				{
					continue;
				}
				const std::string recorded_cwd = payload.value("cwd", "");
				return !recorded_cwd.empty() && PathsMatch(recorded_cwd, cwd);
			}
			catch (...)
			{
			}
		}
		return false;
	}

	inline std::string PickNewSessionId(const std::vector<std::string>& ids_before, const std::filesystem::path& cwd, const std::filesystem::path& codex_home = CodexHomePath())
	{
		const std::unordered_set<std::string> before(ids_before.begin(), ids_before.end());
		std::vector<SessionIndexEntry> entries = ReadSessionIndex(codex_home);
		for (auto it = entries.rbegin(); it != entries.rend(); ++it)
		{
			if (before.find(it->id) != before.end())
			{
				continue;
			}
			if (RolloutCwdMatches(it->id, cwd, codex_home))
			{
				return it->id;
			}
		}
		return "";
	}
} // namespace uam::codex
