#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uam::shell
{
#if !defined(_WIN32)
	inline constexpr std::string_view kPosixEscapedSingleQuote = "'\\''";
#endif

	inline std::size_t EscapedArgSize(std::string_view value)
	{
		std::size_t size = 2;
#if defined(_WIN32)
		std::size_t backslashes = 0;
		for (const char ch : value)
		{
			// Backslashes that precede an emitted quote are doubled; see AppendEscapedArg.
			if (ch == '\\') { ++backslashes; ++size; continue; }
			size += ch == '"' ? backslashes + 2 : ch == '%' ? backslashes + 4 : 1;
			backslashes = 0;
		}
		size += backslashes;
#else
		for (const char ch : value)
		{
			size += ch == '\'' ? kPosixEscapedSingleQuote.size() : 1;
		}
#endif
		return size;
	}

	inline void AppendEscapedArg(std::string& escaped, std::string_view value)
	{
#if defined(_WIN32)
		// Commands run through `cmd.exe /D /V:OFF /S /C` (no delayed `!` expansion), then the program's CRT argv parser.
		// cmd command-line mode never collapses %%, so a percent is emitted outside the
		// quotes as ^% where the caret blocks variable expansion and cmd then removes it.
		// The CRT treats backslashes before a quote as escapes, so those runs are doubled.
		escaped.push_back('"');

		std::size_t backslashes = 0;
		for (const char ch : value)
		{
			if (ch == '\\')
			{
				++backslashes;
				escaped.push_back(ch);
				continue;
			}
			if (ch == '"')
			{
				escaped.append(backslashes, '\\');
				escaped += "\"\"";
			}
			else if (ch == '%')
			{
				escaped.append(backslashes, '\\');
				escaped += "\"^%\"";
			}
			else if (ch == '\r' || ch == '\n')
			{
				escaped.push_back(' ');
			}
			else
			{
				escaped.push_back(ch);
			}
			backslashes = 0;
		}

		escaped.append(backslashes, '\\');
		escaped.push_back('"');
#else
		escaped.push_back('\'');

		for (const char ch : value)
		{
			if (ch == '\'')
			{
				escaped.append(kPosixEscapedSingleQuote);
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		escaped.push_back('\'');
#endif
	}

	inline std::string EscapeArg(std::string_view value)
	{
		std::string escaped;
		escaped.reserve(EscapedArgSize(value));
		AppendEscapedArg(escaped, value);
		return escaped;
	}

	inline std::string JoinEscapedArgs(const std::vector<std::string>& args)
	{
		std::size_t command_size = args.empty() ? 0 : args.size() - 1;
		for (const std::string& arg : args)
		{
			command_size += EscapedArgSize(arg);
		}

		std::string command;
		command.reserve(command_size);
		for (const std::string& arg : args)
		{
			if (!command.empty())
			{
				command.push_back(' ');
			}
			AppendEscapedArg(command, arg);
		}

		return command;
	}
} // namespace uam::shell
