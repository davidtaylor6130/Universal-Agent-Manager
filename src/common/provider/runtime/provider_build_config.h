#pragma once

namespace provider_build_config
{

	inline constexpr const char* FirstEnabledProviderId()
	{
		return "gemini-cli";
	}

	inline constexpr const char* DefaultVectorDbBackend()
	{
		return "none";
	}

	inline constexpr const char* DefaultHistoryAdapter()
	{
		return "gemini-cli-json";
	}

	inline constexpr const char* DefaultNativeHistoryProviderId()
	{
		return "gemini-cli";
	}

	inline constexpr bool HasNativeHistoryProvider()
	{
		return true;
	}

} // namespace provider_build_config
