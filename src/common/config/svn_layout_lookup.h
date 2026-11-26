#pragma once

#include "common/models/app_models.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

/// <summary>Finds the SVN repository layout saved for a repository. Kept free of JSON so the isolated SVN branch tests
/// build it without the settings parser.</summary>
namespace uam::svn_layout
{
	/// <summary>The layout saved for a repository, matched by root URL or UUID with the settings editor's rule (surrounding
	/// spaces, trailing slashes and letter case ignored); nullptr means the standard `trunk`/`branches`/`tags` layout applies.</summary>
	inline const SvnRepositoryLayout* Find(const std::vector<SvnRepositoryLayout>& layouts, std::string_view root, std::string_view uuid)
	{
		const auto key = [](std::string_view value)
		{
			while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) value.remove_prefix(1);
			while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
			while (value.size() > 1 && value.back() == '/') value.remove_suffix(1);
			std::string folded(value);
			for (char& c : folded) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			return folded;
		};
		const std::string folded_root = key(root), folded_uuid = key(uuid);
		const auto found = std::ranges::find_if(layouts, [&](const SvnRepositoryLayout& layout)
		{
			const std::string repository = key(layout.repository);
			return !repository.empty() && (repository == folded_root || repository == folded_uuid);
		});
		return found == layouts.end() ? nullptr : &*found;
	}
} // namespace uam::svn_layout
