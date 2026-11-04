#pragma once

#include "common/usage/account_allowance_fetcher.h"

namespace uam::usage
{
	AccountAllowanceFetchResult NormalizeGeminiAccountAllowance(const nlohmann::json& response, std::int64_t now_ms);
	AccountAllowanceFetchResult NormalizeAntigravityAccountAllowance(const nlohmann::json& response, std::int64_t now_ms);
	AccountAllowanceFetchResult FetchGeminiAccountAllowance(const std::filesystem::path& working_directory, std::stop_token stop = {});
	AccountAllowanceFetchResult FetchAntigravityAccountAllowance(const std::filesystem::path& working_directory, std::stop_token stop = {}, bool allow_prompt = false);

	// Synthetic credentials keep protocol regression tests independent of account storage.
	AccountAllowanceFetchResult FetchGeminiAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    const std::filesystem::path& working_directory, nlohmann::json credentials, std::stop_token stop = {},
	    std::chrono::milliseconds timeout = std::chrono::seconds(10));
	AccountAllowanceFetchResult FetchAntigravityAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    const std::filesystem::path& working_directory, nlohmann::json token_profile, std::stop_token stop = {},
	    std::chrono::milliseconds timeout = std::chrono::seconds(10));
}
