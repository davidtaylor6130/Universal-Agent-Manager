#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace uam::strings
{
	inline constexpr auto kAsciiWhitespaceChars = std::to_array<unsigned char>({
	    ' ',
	    '\t',
	    '\r',
	    '\n',
	    '\f',
	    '\v',
	});

	inline bool IsAsciiSpace(unsigned char ch)
	{
		return std::ranges::find(kAsciiWhitespaceChars, ch) != kAsciiWhitespaceChars.end();
	}

	inline bool IsAsciiAlpha(unsigned char ch)
	{
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
	}

	inline bool IsAsciiDigit(unsigned char ch)
	{
		return ch >= '0' && ch <= '9';
	}

	inline bool IsAsciiAlnum(unsigned char ch)
	{
		return IsAsciiAlpha(ch) || IsAsciiDigit(ch);
	}

	inline bool IsAsciiHexDigit(unsigned char ch)
	{
		return IsAsciiDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
	}

	inline bool AllAsciiDigits(std::string_view value)
	{
		return !value.empty() && std::ranges::all_of(value, [](char ch) { return IsAsciiDigit(static_cast<unsigned char>(ch)); });
	}

	inline int HexDigitValue(unsigned char ch)
	{
		if (IsAsciiDigit(ch))
		{
			return ch - '0';
		}
		if (ch >= 'a' && ch <= 'f')
		{
			return 10 + (ch - 'a');
		}
		if (ch >= 'A' && ch <= 'F')
		{
			return 10 + (ch - 'A');
		}
		return -1;
	}

	inline char ToLowerAsciiChar(unsigned char ch)
	{
		return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
	}

	inline bool StartsWith(std::string_view value, std::string_view prefix)
	{
		return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
	}

	inline bool StartsWithIgnoreCase(std::string_view value, std::string_view prefix)
	{
		if (value.size() < prefix.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < prefix.size(); ++i)
		{
			if (ToLowerAsciiChar(static_cast<unsigned char>(value[i])) != ToLowerAsciiChar(static_cast<unsigned char>(prefix[i])))
			{
				return false;
			}
		}

		return true;
	}

	inline std::string_view ViewOrEmpty(const char* value)
	{
		return value == nullptr ? std::string_view() : std::string_view(value);
	}

	inline std::string_view TrimAsciiView(std::string_view value)
	{
		std::size_t start = 0;
		while (start < value.size() && IsAsciiSpace(static_cast<unsigned char>(value[start])))
		{
			++start;
		}

		std::size_t end = value.size();
		while (end > start && IsAsciiSpace(static_cast<unsigned char>(value[end - 1])))
		{
			--end;
		}

		return value.substr(start, end - start);
	}

	inline std::string Trim(std::string_view value)
	{
		return std::string(TrimAsciiView(value));
	}

	inline bool IsBlank(std::string_view value)
	{
		return TrimAsciiView(value).empty();
	}

	inline std::string TrimOrFallback(std::string_view value, std::string_view fallback)
	{
		const std::string_view trimmed = TrimAsciiView(value);
		return trimmed.empty() ? std::string(fallback) : std::string(trimmed);
	}

	inline std::string TrimAndElide(std::string_view value, std::size_t max_chars)
	{
		const std::string_view trimmed = TrimAsciiView(value);
		if (max_chars == 0 || trimmed.size() <= max_chars)
		{
			return std::string(trimmed);
		}
		if (max_chars <= 3)
		{
			return std::string(trimmed.substr(0, max_chars));
		}

		std::string out;
		out.reserve(max_chars);
		out.append(trimmed.substr(0, max_chars - 3));
		out.append("...");
		return out;
	}

	inline std::string NonEmptyOrFallback(std::string_view value, std::string_view fallback)
	{
		return value.empty() ? std::string(fallback) : std::string(value);
	}

	template <typename Range> inline std::string Join(const Range& values, std::string_view delimiter)
	{
		std::size_t piece_count = 0;
		std::size_t joined_size = 0;
		for (const auto& value : values)
		{
			joined_size += std::string_view(value).size();
			++piece_count;
		}
		if (piece_count > 1)
		{
			joined_size += delimiter.size() * (piece_count - 1);
		}

		std::string joined;
		joined.reserve(joined_size);
		std::size_t index = 0;
		for (const auto& value : values)
		{
			const std::string_view piece(value);
			if (index > 0)
			{
				joined.append(delimiter);
			}
			joined.append(piece);
			++index;
		}
		return joined;
	}

	template <typename Range> inline std::string JoinNonEmpty(const Range& values, std::string_view delimiter)
	{
		std::size_t piece_count = 0;
		std::size_t joined_size = 0;
		for (const auto& value : values)
		{
			const std::string_view piece(value);
			if (piece.empty())
			{
				continue;
			}
			joined_size += piece.size();
			++piece_count;
		}
		if (piece_count > 1)
		{
			joined_size += delimiter.size() * (piece_count - 1);
		}

		std::string joined;
		joined.reserve(joined_size);
		for (const auto& value : values)
		{
			const std::string_view piece(value);
			if (piece.empty())
			{
				continue;
			}
			if (!joined.empty())
			{
				joined.append(delimiter);
			}
			joined.append(piece);
		}
		return joined;
	}

	inline std::string ToLowerAscii(std::string value)
	{
		std::ranges::transform(value, value.begin(), [](unsigned char ch) { return ToLowerAsciiChar(ch); });
		return value;
	}

	inline std::string ToLowerAscii(std::string_view value)
	{
		std::string lowered;
		lowered.reserve(value.size());
		for (const char ch : value)
		{
			lowered.push_back(ToLowerAsciiChar(static_cast<unsigned char>(ch)));
		}
		return lowered;
	}

	inline std::string TrimAndLowerAscii(std::string_view value)
	{
		return ToLowerAscii(TrimAsciiView(value));
	}

	inline std::string SafeLine(std::string_view value, std::size_t max_chars, bool replace_line_feeds = false)
	{
		std::string safe_value = Trim(value);
		std::ranges::replace(safe_value, '\r', ' ');
		if (replace_line_feeds)
		{
			std::ranges::replace(safe_value, '\n', ' ');
		}
		if (safe_value.size() > max_chars)
		{
			safe_value = safe_value.substr(0, max_chars);
		}
		return safe_value;
	}

	inline void TrimTrailingChar(std::string& value, char trailing_char)
	{
		while (!value.empty() && value.back() == trailing_char)
		{
			value.pop_back();
		}
	}

	inline std::string AsciiSlug(std::string_view value, std::size_t max_chars, std::string_view fallback)
	{
		if (max_chars == 0)
		{
			return std::string(fallback);
		}

		std::string out;
		out.reserve(std::min(value.size(), max_chars));
		bool previous_dash = false;
		for (const char raw_ch : value)
		{
			const unsigned char ch = static_cast<unsigned char>(ToLowerAsciiChar(static_cast<unsigned char>(raw_ch)));
			if (IsAsciiAlpha(ch) || IsAsciiDigit(ch))
			{
				out.push_back(static_cast<char>(ch));
				previous_dash = false;
			}
			else if (!previous_dash && !out.empty())
			{
				out.push_back('-');
				previous_dash = true;
			}
			if (out.size() >= max_chars)
			{
				break;
			}
		}
		TrimTrailingChar(out, '-');
		return out.empty() ? std::string(fallback) : out;
	}

	inline std::string NormalizeComparableKey(std::string_view value)
	{
		std::string out;
		out.reserve(value.size());
		for (const char raw_ch : value)
		{
			const unsigned char ch = static_cast<unsigned char>(ToLowerAsciiChar(static_cast<unsigned char>(raw_ch)));
			if (IsAsciiAlnum(ch))
			{
				out.push_back(static_cast<char>(ch));
			}
			else if (!out.empty() && out.back() != ' ')
			{
				out.push_back(' ');
			}
		}
		TrimTrailingChar(out, ' ');
		return out;
	}

	inline bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < lhs.size(); ++i)
		{
			const auto lhs_ch = static_cast<unsigned char>(lhs[i]);
			const auto rhs_ch = static_cast<unsigned char>(rhs[i]);
			if (ToLowerAsciiChar(lhs_ch) != ToLowerAsciiChar(rhs_ch))
			{
				return false;
			}
		}

		return true;
	}

	inline bool TrimmedEqualsIgnoreCase(std::string_view lhs, std::string_view rhs)
	{
		return EqualsIgnoreCase(TrimAsciiView(lhs), TrimAsciiView(rhs));
	}

	inline bool TrimmedEquals(std::string_view lhs, std::string_view rhs)
	{
		return TrimAsciiView(lhs) == TrimAsciiView(rhs);
	}

	inline bool TrimmedEqualsNonEmpty(std::string_view lhs, std::string_view rhs)
	{
		const std::string_view left = TrimAsciiView(lhs);
		return !left.empty() && left == TrimAsciiView(rhs);
	}

	inline bool Contains(std::string_view haystack, char needle)
	{
		return haystack.find(needle) != std::string_view::npos;
	}

	inline bool Contains(std::string_view haystack, std::string_view needle)
	{
		return !needle.empty() && haystack.find(needle) != std::string_view::npos;
	}

	inline bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
	{
		if (needle.empty())
		{
			return false;
		}

		const auto match = std::ranges::search(haystack, needle, {}, ToLowerAsciiChar, ToLowerAsciiChar);
		return !match.empty();
	}

	template <typename Range> inline bool ContainsAny(std::string_view haystack, const Range& needles)
	{
		return std::ranges::any_of(needles, [haystack](std::string_view needle) { return Contains(haystack, needle); });
	}

	inline bool ContainsAny(std::string_view haystack, std::initializer_list<std::string_view> needles)
	{
		return ContainsAny<std::initializer_list<std::string_view>>(haystack, needles);
	}

	template <typename Range> inline bool ContainsAnyCaseInsensitive(std::string_view haystack, const Range& needles)
	{
		return std::ranges::any_of(needles, [haystack](std::string_view needle) { return !needle.empty() && ContainsCaseInsensitive(haystack, needle); });
	}

	inline bool ContainsAnyCaseInsensitive(std::string_view haystack, std::initializer_list<std::string_view> needles)
	{
		return ContainsAnyCaseInsensitive<std::initializer_list<std::string_view>>(haystack, needles);
	}

	template <typename Range> inline std::optional<std::string_view> FindEqualIgnoreCase(const Range& values, std::string_view value)
	{
		const auto found = std::ranges::find_if(values, [value](std::string_view candidate) { return EqualsIgnoreCase(value, candidate); });
		if (found == std::ranges::end(values))
		{
			return std::nullopt;
		}
		return std::string_view(*found);
	}

	template <typename Range> inline bool ContainsEqualIgnoreCase(const Range& values, std::string_view value)
	{
		return FindEqualIgnoreCase(values, value).has_value();
	}
} // namespace uam::strings
