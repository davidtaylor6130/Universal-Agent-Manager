#include "common/runtime/terminal/terminal_idle_classifier.h"

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <string>
#include <string_view>
#include <vector>

namespace uam
{

std::size_t CountTerminalLineBreaks(std::string_view value)
{
	std::size_t count = 0;
	for (const char ch : value)
	{
		if (ch == '\n')
		{
			++count;
		}
	}

	return count;
}

std::vector<std::string> SplitTerminalLines(std::string_view value)
{
	std::vector<std::string> lines;
	lines.reserve(CountTerminalLineBreaks(value) + 1);

	std::string current;
	current.reserve(value.size());
	for (const char ch : value)
	{
		if (ch == '\n')
		{
			lines.push_back(current);
			current.clear();
		}
		else
		{
			current.push_back(ch);
		}
	}

	lines.push_back(current);
	return lines;
}

bool SkipTerminalEscapeSequence(std::string_view input, std::size_t& index)
{
	if (index + 1 >= input.size())
	{
		index = input.size();
		return false;
	}

	const unsigned char next = static_cast<unsigned char>(input[index + 1]);

	if (next == '[')
	{
		index += 2;
		while (index < input.size())
		{
			const unsigned char seq = static_cast<unsigned char>(input[index]);
			if (seq >= 0x40 && seq <= 0x7E)
			{
				break;
			}
			++index;
		}
		return true;
	}

	if (next == ']')
	{
		index += 2;
		while (index < input.size())
		{
			const unsigned char seq = static_cast<unsigned char>(input[index]);
			if (seq == 0x07)
			{
				break;
			}
			if (seq == 0x1B && index + 1 < input.size() && input[index + 1] == '\\')
			{
				++index;
				break;
			}
			++index;
		}
		return true;
	}

	++index;
	return true;
}

std::string StripTerminalControlSequencesForLifecycle(std::string_view input)
{
	std::string output;
	output.reserve(input.size());

	for (std::size_t i = 0; i < input.size(); ++i)
	{
		const unsigned char ch = static_cast<unsigned char>(input[i]);

		if (ch == 0x1B)
		{
			if (!SkipTerminalEscapeSequence(input, i))
			{
				break;
			}
			continue;
		}

		if (ch == '\b' || ch == 0x7F)
		{
			if (!output.empty())
			{
				output.pop_back();
			}
			continue;
		}

		if (ch == '\r')
		{
			output.push_back('\n');
			if (i + 1 < input.size() && input[i + 1] == '\n')
			{
				++i;
			}
			continue;
		}

		if (ch == '\n' || ch == '\t' || ch >= 0x20)
		{
			output.push_back(static_cast<char>(ch));
		}
	}

	return output;
}

std::string NormalizeGeminiPromptLine(std::string_view line)
{
	std::string normalized = uam::strings::Trim(line);

	while (!normalized.empty())
	{
		const unsigned char ch = static_cast<unsigned char>(normalized.front());
		if (ch == '|' || ch == '>' || ch < 0x80)
		{
			break;
		}
		normalized.erase(normalized.begin());
		normalized = uam::strings::Trim(normalized);
	}

	while (!normalized.empty())
	{
		const unsigned char ch = static_cast<unsigned char>(normalized.back());
		if (ch < 0x80)
		{
			break;
		}
		normalized.pop_back();
		normalized = uam::strings::Trim(normalized);
	}

	const auto box_prefix = normalized.find('>');
	if (box_prefix != std::string::npos)
	{
		normalized = uam::strings::Trim(std::string_view(normalized).substr(box_prefix));
	}

	return normalized;
}

std::string RecentTerminalPromptScanText(std::string_view recent_output)
{
	const std::size_t start = recent_output.size() > kTerminalPromptScanLimit ? recent_output.size() - kTerminalPromptScanLimit : 0;
	return StripTerminalControlSequencesForLifecycle(recent_output.substr(start));
}

bool GeminiCliRecentOutputIndicatesInputPrompt(std::string_view recent_output)
{
	const std::string stripped = RecentTerminalPromptScanText(recent_output);

	if (stripped.empty())
	{
		return false;
	}

	const std::vector<std::string> lines = SplitTerminalLines(stripped);

	int inspected = 0;
	for (auto it = lines.rbegin(); it != lines.rend() && inspected < kGeminiPromptRecentLineLimit; ++it)
	{
		std::string line = NormalizeGeminiPromptLine(*it);
		if (line.empty())
		{
			continue;
		}

		++inspected;
		if (uam::ranges::Contains(kGeminiExactPromptLines, std::string_view(line)))
		{
			return true;
		}

		if (uam::strings::Contains(line, '>') && uam::strings::ContainsAny(line, kGeminiPromptCueTexts))
		{
			return true;
		}
	}

	return false;
}

bool CodexCliRecentOutputIndicatesInputPrompt(std::string_view recent_output)
{
	const std::string stripped = RecentTerminalPromptScanText(recent_output);
	if (stripped.empty())
	{
		return false;
	}

	return uam::strings::ContainsAny(stripped, kCodexPromptMarkers) && uam::strings::ContainsAny(stripped, kCodexPromptCueTexts);
}

bool FallbackCliRecentOutputIndicatesInputPrompt(std::string_view recent_output)
{
	return GeminiCliRecentOutputIndicatesInputPrompt(recent_output);
}

} // namespace uam
