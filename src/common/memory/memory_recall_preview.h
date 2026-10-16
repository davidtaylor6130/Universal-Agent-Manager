#pragma once

#include "common/utils/string_utils.h"

#include <array>
#include <istream>
#include <string>

namespace uam::memory
{
	inline bool IsSafeDetailReference(std::string_view path)
	{
		return !path.empty() && uam::strings::SafeLine(path, path.size() + 1, true) == path &&
		    std::ranges::none_of(path, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
	}

	/// <summary>Reads a bounded fact preview without folding later evidence sections into the prompt.</summary>
	inline std::string ReadFactPreview(std::istream& file)
	{
		constexpr std::string_view marker = "## Memory";
		std::array<char, 8192> buffer{};
		std::string overlap;
		std::streamoff offset = 0;
		std::streamoff body_offset = 0;
		while (file)
		{
			file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::size_t count = static_cast<std::size_t>(file.gcount());
			const std::string chunk = overlap + std::string(buffer.data(), count);
			const std::size_t found = chunk.find(marker);
			if (found != std::string::npos)
			{
				body_offset = offset - static_cast<std::streamoff>(overlap.size()) + static_cast<std::streamoff>(found + marker.size());
				break;
			}
			offset += static_cast<std::streamoff>(count);
			overlap = chunk.substr(chunk.size() - std::min(chunk.size(), marker.size() - 1));
		}
		if (file.bad()) return {};
		file.clear();
		file.seekg(body_offset);
		std::string preview;
		bool line_start = true;
		const auto append = [&preview](char ch)
		{
			if (preview.empty() && uam::strings::IsAsciiSpace(static_cast<unsigned char>(ch))) return true;
			if (preview.size() == 321)
			{
				if (uam::strings::IsAsciiSpace(static_cast<unsigned char>(ch))) return true;
				preview += 'x';
				return false;
			}
			preview += ch;
			return preview.size() != 321 || uam::strings::IsAsciiSpace(static_cast<unsigned char>(ch));
		};
		char ch = 0;
		while (file.get(ch))
		{
			if (line_start && ch == '#' && file.peek() == '#')
			{
				file.get(ch);
				const int next = file.peek();
				if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == std::char_traits<char>::eof()) break;
				if (!append('#') || !append('#')) break;
				line_start = false;
				continue;
			}
			if (ch == '\n') line_start = true;
			else if (ch != ' ' && ch != '\t' && ch != '\r') line_start = false;
			if (!append(ch)) break;
		}
		return file.bad() ? std::string{} : uam::strings::SafeLine(preview, 320, true);
	}
}
