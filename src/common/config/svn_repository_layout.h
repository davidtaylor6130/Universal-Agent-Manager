#pragma once

#include "common/models/app_models.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

/// <summary>Per-repository SVN layouts: which folders hold the main line, branches and tags when a repository does
/// not use `trunk`/`branches`/`tags`. The interface uses them to name what a path is; no action reads them.</summary>
namespace uam::svn_layout
{
	inline constexpr std::size_t kMaxLayouts = 64;
	inline constexpr std::size_t kMaxFieldBytes = 512;

	/// A repository-relative folder: no leading or trailing slash, no empty, `.` or `..` segment, no control characters.
	inline bool IsFolder(std::string_view value)
	{
		if (value.empty() || value.size() > kMaxFieldBytes || value.front() == '/' || value.back() == '/') return false;
		for (std::size_t start = 0; start <= value.size();)
		{
			const std::size_t end = std::min(value.find('/', start), value.size());
			const std::string_view segment = value.substr(start, end - start);
			if (segment.empty() || segment == "." || segment == "..") return false;
			start = end + 1;
		}
		return std::ranges::none_of(value, [](const char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F || c == '\\'; });
	}

	inline nlohmann::json ToJson(const std::vector<SvnRepositoryLayout>& layouts)
	{
		nlohmann::json value = nlohmann::json::array();
		for (const SvnRepositoryLayout& layout : layouts)
			value.push_back({{"repository", layout.repository}, {"trunk", layout.trunk}, {"branches", layout.branches}, {"tags", layout.tags}});
		return value;
	}

	/// <summary>Strict: every entry must be complete and repositories unique, or nothing is accepted and `layouts` is left alone.</summary>
	inline bool Parse(const nlohmann::json& value, std::vector<SvnRepositoryLayout>& layouts)
	{
		if (!value.is_array() || value.size() > kMaxLayouts) return false;
		std::vector<SvnRepositoryLayout> parsed;
		for (const nlohmann::json& item : value)
		{
			if (!item.is_object()) return false;
			SvnRepositoryLayout layout;
			for (const auto& [key, field] : {std::pair{"repository", &layout.repository}, std::pair{"trunk", &layout.trunk}, std::pair{"branches", &layout.branches}, std::pair{"tags", &layout.tags}})
			{
				const auto found = item.find(key);
				if (found == item.end() || !found->is_string()) return false;
				*field = uam::strings::Trim(found->get<std::string>());
			}
			while (layout.repository.size() > 1 && layout.repository.back() == '/') layout.repository.pop_back();
			const bool plain = std::ranges::none_of(layout.repository, [](const char c) { return static_cast<unsigned char>(c) <= 0x20 || c == 0x7F; });
			if (layout.repository.empty() || layout.repository.size() > kMaxFieldBytes || !plain || !IsFolder(layout.trunk) || !IsFolder(layout.branches) || !IsFolder(layout.tags)) return false;
			// Same rule as the settings editor: a repository differing only in letter case is the same repository.
			const auto folded = [](std::string value) { for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return value; };
			if (std::ranges::any_of(parsed, [&](const SvnRepositoryLayout& other) { return folded(other.repository) == folded(layout.repository); })) return false;
			parsed.push_back(std::move(layout));
		}
		layouts = std::move(parsed);
		return true;
	}
} // namespace uam::svn_layout
