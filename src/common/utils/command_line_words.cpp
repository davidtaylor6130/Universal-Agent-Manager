#include "common/utils/command_line_words.h"

#include "common/utils/string_utils.h"

#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{
	bool IsQuoteChar(char ch)
	{
		return ch == '\'' || ch == '"';
	}

	void AppendCurrentWordIfPending(std::vector<std::string>& words, std::string& current, bool& word_started)
	{
		if (!word_started)
		{
			return;
		}

		words.push_back(std::move(current));
		current.clear();
		word_started = false;
	}

	std::vector<std::string> SplitShellWordsFallback(std::string_view value)
	{
		std::vector<std::string> words;
		std::string current;
		current.reserve(value.size());
		bool word_started = false;
		bool escaping = false;
		char quote = '\0';

		for (char ch : value)
		{
			if (escaping)
			{
				current.push_back(ch);
				word_started = true;
				escaping = false;
				continue;
			}

			if (quote == '\'')
			{
				if (ch == quote)
				{
					quote = '\0';
				}
				else
				{
					current.push_back(ch);
					word_started = true;
				}

				continue;
			}

			if (ch == '\\')
			{
				escaping = true;
				continue;
			}

			if (quote != '\0')
			{
				if (ch == quote)
				{
					quote = '\0';
				}
				else
				{
					current.push_back(ch);
					word_started = true;
				}

				continue;
			}

			if (IsQuoteChar(ch))
			{
				quote = ch;
				word_started = true;
				continue;
			}

			if (uam::strings::IsAsciiSpace(static_cast<unsigned char>(ch)))
			{
				AppendCurrentWordIfPending(words, current, word_started);
				continue;
			}

			current.push_back(ch);
			word_started = true;
		}

		if (escaping)
		{
			current.push_back('\\');
			word_started = true;
		}

		AppendCurrentWordIfPending(words, current, word_started);

		return words;
	}

#if defined(_WIN32)
	std::wstring WideFromString(std::string_view value)
	{
		if (value.empty())
		{
			return std::wstring();
		}

		auto convert = [&](const UINT code_page, const DWORD flags) -> std::wstring
		{
			const int wide_len = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);

			if (wide_len <= 0)
			{
				return std::wstring();
			}

			std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');

			if (MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), wide.data(), wide_len) <= 0)
			{
				return std::wstring();
			}

			return wide;
		};

		std::wstring wide = convert(CP_UTF8, MB_ERR_INVALID_CHARS);

		if (!wide.empty())
		{
			return wide;
		}

		return convert(CP_ACP, 0);
	}

	std::string StringFromWide(const std::wstring& value)
	{
		if (value.empty())
		{
			return std::string();
		}

		auto convert = [&](const UINT code_page) -> std::string
		{
			const int narrow_len = WideCharToMultiByte(code_page, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);

			if (narrow_len <= 0)
			{
				return std::string();
			}

			std::string narrow(static_cast<std::size_t>(narrow_len), '\0');

			if (WideCharToMultiByte(code_page, 0, value.data(), static_cast<int>(value.size()), narrow.data(), narrow_len, nullptr, nullptr) <= 0)
			{
				return std::string();
			}

			return narrow;
		};

		std::string narrow = convert(CP_UTF8);

		if (!narrow.empty())
		{
			return narrow;
		}

		return convert(CP_ACP);
	}

#endif

} // namespace

std::vector<std::string> uam::command_line::SplitWords(std::string_view value)
{
	if (value.empty())
	{
		return {};
	}

#if defined(_WIN32)
	const std::wstring wide = WideFromString(value);

	if (!wide.empty())
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(wide.c_str(), &argc);

		if (argv != nullptr)
		{
			std::vector<std::string> words;
			words.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));

			for (int i = 0; i < argc; ++i)
			{
				words.push_back(StringFromWide(argv[i] == nullptr ? L"" : std::wstring(argv[i])));
			}

			LocalFree(argv);
			return words;
		}
	}

#endif

	return SplitShellWordsFallback(value);
}
