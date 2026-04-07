#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ollama_engine::internal::determanistic_hash
{

	/// <summary>Computes a deterministic 64-bit FNV-1a hash.</summary>
	/// <param name="pSContent">Input bytes to hash.</param>
	/// <returns>FNV-1a hash value.</returns>
	std::uint64_t Fnv1a64(std::string_view pSContent);

	/// <summary>Converts a 64-bit hash to fixed-width lowercase hex.</summary>
	/// <param name="piHash">Hash value.</param>
	/// <returns>16-char hex string.</returns>
	std::string ToHex(std::uint64_t piHash);

	/// <summary>Hashes text and returns lowercase hex digest.</summary>
	/// <param name="pSContent">Input bytes to hash.</param>
	/// <returns>16-char hex string.</returns>
	std::string HashTextHex(std::string_view pSContent);

} // namespace ollama_engine::internal::determanistic_hash
