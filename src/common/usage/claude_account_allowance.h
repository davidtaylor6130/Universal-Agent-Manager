#pragma once

#include "common/usage/account_allowance_fetcher.h"

namespace uam::usage
{
	AccountAllowanceFetchResult NormalizeClaudeAccountAllowance(const nlohmann::json& response, std::int64_t now_ms);
	AccountAllowanceFetchResult FetchClaudeAccountAllowance(const std::filesystem::path& working_directory, std::stop_token stop = {});
	// Synthetic credentials and a process fixture exercise the transport without reading account storage.
	AccountAllowanceFetchResult FetchClaudeAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    const std::filesystem::path& working_directory, std::string token, std::stop_token stop = {},
	    std::chrono::milliseconds timeout = std::chrono::seconds(10));
}
