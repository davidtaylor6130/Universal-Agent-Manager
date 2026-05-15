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
		for (const char ch : value)
		{
#if defined(_WIN32)
			size += (ch == '"' || ch == '%') ? 2 : 1;
#else
			size += ch == '\'' ? kPosixEscapedSingleQuote.size() : 1;
#endif
		}
		return size;
	}

	inline void AppendEscapedArg(std::string& escaped, std::string_view value)
	{
#if defined(_WIN32)
		escaped.push_back('"');

		for (const char ch : value)
		{
			if (ch == '"')
			{
				escaped += "\"\"";
			}
			else if (ch == '%')
			{
				escaped += "%%";
			}
			else if (ch == '\r' || ch == '\n')
			{
				escaped.push_back(' ');
			}
			else
			{
				escaped.push_back(ch);
			}
		}

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
