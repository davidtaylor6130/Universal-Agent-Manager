#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace uam::hashing
{
	inline constexpr std::uint64_t kFnv1a64OffsetBasis = 1469598103934665603ULL;
	inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;
	inline constexpr char kLowerHexDigits[] = "0123456789abcdef";

	inline void UpdateFnv1a64(std::uint64_t& hash, const unsigned char* data, std::size_t length)
	{
		for (std::size_t i = 0; i < length; ++i)
		{
			hash ^= static_cast<std::uint64_t>(data[i]);
			hash *= kFnv1a64Prime;
		}
	}

	inline void UpdateFnv1a64(std::uint64_t& hash, std::string_view text)
	{
		UpdateFnv1a64(hash, reinterpret_cast<const unsigned char*>(text.data()), text.size());
	}

	inline void UpdateFnv1a64WithSeparator(std::uint64_t& hash, std::string_view text, unsigned char separator = 0xFF)
	{
		UpdateFnv1a64(hash, text);
		UpdateFnv1a64(hash, &separator, 1);
	}

	inline std::uint64_t Fnv1a64(std::string_view text)
	{
		std::uint64_t hash = kFnv1a64OffsetBasis;
		UpdateFnv1a64(hash, text);
		return hash;
	}

	inline std::string Hex64(std::uint64_t value)
	{
		if (value == 0)
		{
			return "0";
		}

		std::string hex;
		hex.reserve(16);
		bool seen_non_zero = false;
		for (int shift = 60; shift >= 0; shift -= 4)
		{
			const char digit = kLowerHexDigits[(value >> shift) & 0x0f];
			if (digit != '0' || seen_non_zero)
			{
				hex.push_back(digit);
				seen_non_zero = true;
			}
		}
		return hex;
	}

	inline std::string Hex64Padded(std::uint64_t value)
	{
		std::string hex(16, '0');
		for (int index = 15, shift = 0; index >= 0; --index, shift += 4)
		{
			hex[static_cast<std::size_t>(index)] = kLowerHexDigits[(value >> shift) & 0x0f];
		}
		return hex;
	}
} // namespace uam::hashing
