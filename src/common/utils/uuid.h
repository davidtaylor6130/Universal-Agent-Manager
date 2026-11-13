#pragma once

#include "common/utils/string_utils.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace uam::uuid
{
	/// <summary>Checks the canonical 8-4-4-4-12 hexadecimal form without accepting URNs or padding.</summary>
	inline bool IsCanonicalUuid(std::string_view value)
	{
		if (value.size() != 36)
		{
			return false;
		}
		for (std::size_t index = 0; index < value.size(); ++index)
		{
			if (index == 8 || index == 13 || index == 18 || index == 23)
			{
				if (value[index] != '-') return false;
			}
			else if (!uam::strings::IsAsciiHexDigit(static_cast<unsigned char>(value[index])))
			{
				return false;
			}
		}
		return true;
	}

	/// <summary>Formats 16 random bytes as a lowercase version-4 UUID, setting the version and variant bits.</summary>
	inline std::string FormatUuidV4(std::array<std::uint8_t, 16> bytes)
	{
		bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
		bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
		constexpr char kHexDigits[] = "0123456789abcdef";
		std::string uuid;
		uuid.reserve(36);
		for (std::size_t index = 0; index < bytes.size(); ++index)
		{
			if (index == 4 || index == 6 || index == 8 || index == 10) uuid.push_back('-');
			uuid.push_back(kHexDigits[bytes[index] >> 4]);
			uuid.push_back(kHexDigits[bytes[index] & 0x0f]);
		}
		return uuid;
	}
}
