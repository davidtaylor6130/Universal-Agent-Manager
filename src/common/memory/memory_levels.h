#pragma once

#include "common/utils/string_utils.h"

#include <string>
#include <string_view>

namespace uam::memory_levels
{
	inline constexpr const char* kDefault = "default";
	inline constexpr const char* kOff = "off";
	inline constexpr const char* kReadOnly = "readOnly";
	inline constexpr const char* kStrict = "strict";
	inline constexpr const char* kBalanced = "balanced";
	inline constexpr const char* kOpen = "open";

	inline std::string Normalize(std::string_view value, bool legacy_enabled = true)
	{
		if (!legacy_enabled)
		{
			return kOff;
		}
		const std::string normalized = uam::strings::ToLowerAscii(uam::strings::Trim(std::string(value)));
		if (normalized == "readonly") return kReadOnly;
		if (normalized == kOff || normalized == kStrict || normalized == kBalanced || normalized == kOpen)
		{
			return normalized;
		}
		return legacy_enabled ? kStrict : kOff;
	}

	/// <summary>Keep inheritance as a persisted selection; legacy booleans apply only to concrete levels.</summary>
	inline std::string NormalizeSelection(std::string_view value, bool legacy_enabled = true)
	{
		if (uam::strings::TrimAndLowerAscii(value) == kDefault) return kDefault;
		return Normalize(value, legacy_enabled);
	}

	inline bool IsEnabled(std::string_view value, bool legacy_enabled = true)
	{
		return Normalize(value, legacy_enabled) != kOff;
	}

	/// <summary>Recall may remain enabled while automatic and manual AI extraction are forbidden.</summary>
	inline bool AllowsCreation(std::string_view value, bool legacy_enabled = true)
	{
		const std::string normalized = Normalize(value, legacy_enabled);
		return normalized != kOff && normalized != kReadOnly;
	}
} // namespace uam::memory_levels
