#pragma once

#include "common/utils/string_utils.h"

#include <cstddef>
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
}
