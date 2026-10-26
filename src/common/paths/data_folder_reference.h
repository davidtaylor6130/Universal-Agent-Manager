#pragma once

#include "common/paths/path_utils.h"
#include <string>
#include <string_view>

namespace uam::paths
{
	// Admission only. Conservative opaque-reference detection grants no rewrite or IO authority.
	inline bool EmbedsDataFolderReference(std::string_view value, const std::filesystem::path& root)
	{
		const auto spelling = [](std::string_view text)
		{
			std::string result;
			for (std::size_t i = 0; i < text.size(); ++i)
			{
				unsigned char byte = static_cast<unsigned char>(text[i]);
				if (byte == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '\t'))
					continue;
				if (byte == '\\')
					byte = '/';
				if (byte >= 'A' && byte <= 'Z')
					byte += 'a' - 'A';
				if (byte == '/' && !result.empty() && result.back() == '/')
					continue;
				result.push_back(static_cast<char>(byte));
			}
			return result;
		};
		const auto text = spelling(value), prefix = spelling(Utf8PathString(root));
		if (prefix.empty())
			return true;
		for (std::size_t position = text.find(prefix); position != std::string::npos; position = text.find(prefix, position + 1))
		{
			const auto end = position + prefix.size();
			if (end == text.size() || text[end] == '/' || std::string_view("\"' ,;)}]\n\r\t").find(text[end]) != std::string_view::npos)
				return true;
		}
		return false;
	}
} // namespace uam::paths
