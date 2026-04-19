#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

inline std::string StripTerminalControlSequencesForLifecycle(const std::string_view input)
{
	std::string output;
	output.reserve(input.size());

	for (std::size_t i = 0; i < input.size(); ++i)
	{
		const unsigned char ch = static_cast<unsigned char>(input[i]);

		if (ch == 0x1B)
		{
			if (i + 1 >= input.size())
			{
				break;
			}

			const unsigned char next = static_cast<unsigned char>(input[i + 1]);

			if (next == '[')
			{
				i += 2;
				while (i < input.size())
				{
					const unsigned char seq = static_cast<unsigned char>(input[i]);
					if (seq >= 0x40 && seq <= 0x7E)
					{
						break;
					}
					++i;
				}
				continue;
			}

			if (next == ']')
			{
				i += 2;
				while (i < input.size())
				{
					const unsigned char seq = static_cast<unsigned char>(input[i]);
					if (seq == 0x07)
					{
						break;
					}
					if (seq == 0x1B && i + 1 < input.size() && input[i + 1] == '\\')
					{
						++i;
						break;
					}
					++i;
				}
				continue;
			}

			++i;
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
			continue;
		}

		if (ch == '\n' || ch == '\t' || ch >= 0x20)
		{
			output.push_back(static_cast<char>(ch));
		}
	}

	return output;
}

inline std::string TrimAsciiCopy(std::string value)
{
	const auto is_space = [](const unsigned char ch)
	{
		return std::isspace(ch) != 0;
	};

	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](const char ch)
	{
		return !is_space(static_cast<unsigned char>(ch));
	}));

	value.erase(std::find_if(value.rbegin(), value.rend(), [&](const char ch)
	{
		return !is_space(static_cast<unsigned char>(ch));
	}).base(), value.end());

	return value;
}

inline std::string NormalizeGeminiPromptLine(std::string line)
{
	line = TrimAsciiCopy(std::move(line));

	while (!line.empty())
	{
		const unsigned char ch = static_cast<unsigned char>(line.front());
		if (ch == '|' || ch == '>' || ch < 0x80)
		{
			break;
		}
		line.erase(line.begin());
		line = TrimAsciiCopy(std::move(line));
	}

	while (!line.empty())
	{
		const unsigned char ch = static_cast<unsigned char>(line.back());
		if (ch < 0x80)
		{
			break;
		}
		line.pop_back();
		line = TrimAsciiCopy(std::move(line));
	}

	const auto box_prefix = line.find('>');
	if (box_prefix != std::string::npos)
	{
		line = TrimAsciiCopy(line.substr(box_prefix));
	}

	return line;
}

inline bool GeminiCliRecentOutputIndicatesInputPrompt(const std::string_view recent_output)
{
	constexpr std::size_t kPromptScanLimit = 8192;
	const std::size_t start = recent_output.size() > kPromptScanLimit ? recent_output.size() - kPromptScanLimit : 0;
	const std::string stripped = StripTerminalControlSequencesForLifecycle(recent_output.substr(start));

	if (stripped.empty())
	{
		return false;
	}

	std::vector<std::string> lines;
	std::string current;
	for (const char ch : stripped)
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

	int inspected = 0;
	for (auto it = lines.rbegin(); it != lines.rend() && inspected < 6; ++it)
	{
		std::string line = NormalizeGeminiPromptLine(*it);
		if (line.empty())
		{
			continue;
		}

		++inspected;
		if (line == ">" || line == "> " || line == ">_")
		{
			return true;
		}

		if (line.find("Type your message") != std::string::npos && line.find('>') != std::string::npos)
		{
			return true;
		}

		if (line.find("? for shortcuts") != std::string::npos && line.find('>') != std::string::npos)
		{
			return true;
		}
	}

	return false;
}

inline bool CodexCliRecentOutputIndicatesInputPrompt(const std::string_view recent_output)
{
	constexpr std::size_t kPromptScanLimit = 8192;
	const std::size_t start = recent_output.size() > kPromptScanLimit ? recent_output.size() - kPromptScanLimit : 0;
	const std::string stripped = StripTerminalControlSequencesForLifecycle(recent_output.substr(start));
	if (stripped.empty())
	{
		return false;
	}

	if (stripped.find("\xE2\x80\xBA") != std::string::npos || stripped.find("> ") != std::string::npos)
	{
		if (stripped.find("Send") != std::string::npos || stripped.find("message") != std::string::npos || stripped.find("for shortcuts") != std::string::npos)
		{
			return true;
		}
	}

	return false;
}
