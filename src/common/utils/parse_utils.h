#pragma once

#include "common/utils/string_utils.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace uam::parse
{
	inline constexpr auto kTruthyBoolTokens = std::to_array<std::string_view>({"1", "true", "on", "yes"});
	inline constexpr auto kFalsyBoolTokens = std::to_array<std::string_view>({"0", "false", "off", "no"});

	template <typename Number> inline std::optional<Number> FromCharsStrict(std::string_view value)
	{
		value = uam::strings::TrimAsciiView(value);
		if (value.empty())
		{
			return std::nullopt;
		}

		Number parsed{};
		const char* begin = value.data();
		const char* end = value.data() + value.size();
		const auto result = std::from_chars(begin, end, parsed);
		if (result.ec != std::errc{} || result.ptr != end)
		{
			return std::nullopt;
		}

		return parsed;
	}

	inline std::optional<int> IntStrict(std::string_view value)
	{
		return FromCharsStrict<int>(value);
	}

	inline int IntOr(std::string_view value, int fallback)
	{
		const std::optional<int> parsed = IntStrict(value);
		return parsed.value_or(fallback);
	}

	inline std::optional<long long> NonNegativeLongLongStrict(std::string_view value)
	{
		value = uam::strings::TrimAsciiView(value);
		return uam::strings::AllAsciiDigits(value) ? FromCharsStrict<long long>(value) : std::nullopt;
	}

	inline std::optional<int> NonNegativeIntStrict(std::string_view value)
	{
		const std::optional<long long> parsed = NonNegativeLongLongStrict(value);
		if (!parsed || *parsed > std::numeric_limits<int>::max())
		{
			return std::nullopt;
		}

		return static_cast<int>(*parsed);
	}

	template <typename Float> inline std::optional<Float> FiniteFromCharsStrict(std::string_view value)
	{
		const std::optional<Float> parsed = FromCharsStrict<Float>(value);
		if (!parsed || !std::isfinite(*parsed))
		{
			return std::nullopt;
		}

		return *parsed;
	}

	inline std::optional<float> FloatStrict(std::string_view value)
	{
		return FiniteFromCharsStrict<float>(value);
	}

	inline std::optional<double> DoubleStrict(std::string_view value)
	{
		return FiniteFromCharsStrict<double>(value);
	}

	inline float FloatOr(std::string_view value, float fallback)
	{
		const std::optional<float> parsed = FloatStrict(value);
		return parsed.value_or(fallback);
	}

	inline double DoubleOr(std::string_view value, double fallback)
	{
		const std::optional<double> parsed = DoubleStrict(value);
		return parsed.value_or(fallback);
	}

	inline std::optional<bool> BoolStrict(std::string_view value)
	{
		value = uam::strings::TrimAsciiView(value);
		if (value.empty())
		{
			return std::nullopt;
		}

		if (uam::strings::ContainsEqualIgnoreCase(kTruthyBoolTokens, value))
		{
			return true;
		}
		if (uam::strings::ContainsEqualIgnoreCase(kFalsyBoolTokens, value))
		{
			return false;
		}

		return std::nullopt;
	}

	inline bool BoolOr(std::string_view value, bool fallback)
	{
		const std::optional<bool> parsed = BoolStrict(value);
		return parsed.value_or(fallback);
	}
} // namespace uam::parse
