#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

	inline constexpr const char* kEncodedLineValuePrefix = "@uam-escaped:";
	inline constexpr auto kLineValueEscapableChars = std::to_array<char>({
	    '\\',
	    '\n',
	    '\r',
	    '\t',
	    ',',
	    ';',
	});

	enum class UnknownLineEscapePolicy
	{
		PreserveBackslash,
		DropBackslash,
	};

	inline constexpr std::size_t EncodedLineValuePrefixLength()
	{
		return std::char_traits<char>::length(kEncodedLineValuePrefix);
	}

	inline bool IsLineValueEscapableChar(char ch)
	{
		return uam::ranges::Contains(kLineValueEscapableChars, ch);
	}

	inline bool NeedsLineValueEscaping(std::string_view value)
	{
		for (const char ch : value)
		{
			if (IsLineValueEscapableChar(ch))
			{
				return true;
			}
		}

		return false;
	}

	inline std::size_t EscapedLineValueBodySize(std::string_view value)
	{
		std::size_t size = 0;
		for (const char ch : value)
		{
			size += IsLineValueEscapableChar(ch) ? 2 : 1;
		}
		return size;
	}

	inline void AppendEscapedLineValueBody(std::string& encoded, std::string_view value)
	{
		for (const char ch : value)
		{
			switch (ch)
			{
			case '\\':
				encoded.append("\\\\");
				break;
			case '\n':
				encoded.append("\\n");
				break;
			case '\r':
				encoded.append("\\r");
				break;
			case '\t':
				encoded.append("\\t");
				break;
			case ',':
				encoded.append("\\c");
				break;
			case ';':
				encoded.append("\\s");
				break;
			default:
				encoded.push_back(ch);
				break;
			}
		}
	}

	inline std::string EscapeLineValueBody(std::string_view value)
	{
		std::string encoded;
		encoded.reserve(EscapedLineValueBodySize(value));
		AppendEscapedLineValueBody(encoded, value);

		return encoded;
	}

	inline std::string UnescapeLineValueBody(std::string_view value, UnknownLineEscapePolicy unknown_escape_policy = UnknownLineEscapePolicy::PreserveBackslash)
	{
		std::string decoded;
		decoded.reserve(value.size());

		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const char ch = value[i];

			if (ch != '\\' || i + 1 >= value.size())
			{
				decoded.push_back(ch);
				continue;
			}

			const char next = value[++i];

			switch (next)
			{
			case 'n':
				decoded.push_back('\n');
				break;
			case 'r':
				decoded.push_back('\r');
				break;
			case 't':
				decoded.push_back('\t');
				break;
			case '\\':
				decoded.push_back('\\');
				break;
			case 'c':
				decoded.push_back(',');
				break;
			case 's':
				decoded.push_back(';');
				break;
			default:
				if (unknown_escape_policy == UnknownLineEscapePolicy::PreserveBackslash)
				{
					decoded.push_back('\\');
				}
				decoded.push_back(next);
				break;
			}
		}

		return decoded;
	}

	inline std::string EncodeLineValue(std::string_view value)
	{
		if (!NeedsLineValueEscaping(value))
		{
			return std::string(value);
		}

		std::string encoded;
		encoded.reserve(EncodedLineValuePrefixLength() + EscapedLineValueBodySize(value));
		encoded.append(kEncodedLineValuePrefix);
		AppendEscapedLineValueBody(encoded, value);
		return encoded;
	}

	inline std::string DecodeLineValue(std::string_view value)
	{
		if (!uam::strings::StartsWith(value, kEncodedLineValuePrefix))
		{
			return std::string(value);
		}

		const std::string_view encoded = value.substr(EncodedLineValuePrefixLength());
		return UnescapeLineValueBody(encoded);
	}

	inline std::vector<std::string_view> SplitLineValueFields(std::string_view value, char delimiter)
	{
		std::vector<std::string_view> fields;
		fields.reserve(static_cast<std::size_t>(std::ranges::count(value, delimiter)) + 1);

		std::size_t start = 0;
		while (start <= value.size())
		{
			const std::size_t delimiter_at = value.find(delimiter, start);
			if (delimiter_at == std::string_view::npos)
			{
				fields.push_back(value.substr(start));
				break;
			}

			fields.push_back(value.substr(start, delimiter_at - start));
			start = delimiter_at + 1;
		}
		return fields;
	}

	inline std::string DecodedLineFieldOr(const std::vector<std::string_view>& fields, std::size_t index, std::string_view fallback)
	{
		return index < fields.size() ? DecodeLineValue(fields[index]) : std::string(fallback);
	}

	inline std::string EncodeLineValueFields(std::initializer_list<std::string_view> fields, std::string_view delimiter)
	{
		std::vector<std::string> encoded_fields;
		encoded_fields.reserve(fields.size());
		for (const std::string_view field : fields)
		{
			encoded_fields.push_back(EncodeLineValue(field));
		}
		return uam::strings::Join(encoded_fields, delimiter);
	}

} // namespace uam
