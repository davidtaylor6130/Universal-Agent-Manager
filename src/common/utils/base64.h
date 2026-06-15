#pragma once

#include "common/utils/string_utils.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace uam::base64
{
	inline constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	inline int DecodeValue(char ch)
	{
		if (ch >= 'A' && ch <= 'Z')
		{
			return ch - 'A';
		}
		if (ch >= 'a' && ch <= 'z')
		{
			return ch - 'a' + 26;
		}
		if (ch >= '0' && ch <= '9')
		{
			return ch - '0' + 52;
		}
		if (ch == '+')
		{
			return 62;
		}
		if (ch == '/')
		{
			return 63;
		}
		return -1;
	}

	inline bool FailDecode(std::string& out)
	{
		out.clear();
		return false;
	}

	inline bool HasZeroUnusedTrailingBits(std::size_t data_chars, int last_decoded)
	{
		const std::size_t remainder = data_chars % 4;
		if (remainder == 2)
		{
			return (last_decoded & 0x0F) == 0;
		}
		if (remainder == 3)
		{
			return (last_decoded & 0x03) == 0;
		}
		return true;
	}

	inline std::string Encode(std::string_view input)
	{
		std::string out;
		out.reserve(((input.size() + 2) / 3) * 4);

		std::size_t i = 0;
		while (i + 3 <= input.size())
		{
			const unsigned char first = static_cast<unsigned char>(input[i]);
			const unsigned char second = static_cast<unsigned char>(input[i + 1]);
			const unsigned char third = static_cast<unsigned char>(input[i + 2]);
			out += kAlphabet[(first >> 2) & 0x3F];
			out += kAlphabet[((first & 0x03) << 4) | ((second >> 4) & 0x0F)];
			out += kAlphabet[((second & 0x0F) << 2) | ((third >> 6) & 0x03)];
			out += kAlphabet[third & 0x3F];
			i += 3;
		}

		const std::size_t remaining = input.size() - i;
		if (remaining == 1)
		{
			const unsigned char first = static_cast<unsigned char>(input[i]);
			out += kAlphabet[(first >> 2) & 0x3F];
			out += kAlphabet[(first & 0x03) << 4];
			out += "==";
		}
		else if (remaining == 2)
		{
			const unsigned char first = static_cast<unsigned char>(input[i]);
			const unsigned char second = static_cast<unsigned char>(input[i + 1]);
			out += kAlphabet[(first >> 2) & 0x3F];
			out += kAlphabet[((first & 0x03) << 4) | ((second >> 4) & 0x0F)];
			out += kAlphabet[(second & 0x0F) << 2];
			out += '=';
		}

		return out;
	}

	inline bool Decode(std::string_view input, std::string& out)
	{
		out.clear();

		int value = 0;
		int bits = -8;
		bool saw_padding = false;
		std::size_t data_chars = 0;
		std::size_t padding_chars = 0;
		int last_decoded = 0;
		for (const char ch : input)
		{
			if (uam::strings::IsAsciiSpace(static_cast<unsigned char>(ch)))
			{
				continue;
			}
			if (ch == '=')
			{
				saw_padding = true;
				++padding_chars;
				if (padding_chars > 2)
				{
					return FailDecode(out);
				}
				continue;
			}
			if (saw_padding)
			{
				return FailDecode(out);
			}

			const int decoded = DecodeValue(ch);
			if (decoded < 0)
			{
				return FailDecode(out);
			}

			++data_chars;
			last_decoded = decoded;
			value = (value << 6) + decoded;
			bits += 6;
			if (bits >= 0)
			{
				out.push_back(static_cast<char>((value >> bits) & 0xFF));
				bits -= 8;
			}
		}

		if (data_chars % 4 == 1 || (padding_chars > 0 && (data_chars + padding_chars) % 4 != 0))
		{
			return FailDecode(out);
		}

		if (!HasZeroUnusedTrailingBits(data_chars, last_decoded))
		{
			return FailDecode(out);
		}

		return true;
	}
} // namespace uam::base64
