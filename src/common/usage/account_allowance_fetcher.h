#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

class IPlatformProcessService;

namespace uam::usage
{
	struct AccountAllowanceFetchResult
	{
		bool account_unavailable = false;
		nlohmann::json limits = nullptr;
		nlohmann::json windows = nlohmann::json::array();
		std::string error;
		std::string primary_label;
		std::string secondary_label;
	};

	struct AccountAllowanceCommand
	{
		std::vector<std::string> command;
		std::string error;
	};
	// Keep independent account reads separate from interactive prompts, wrappers and account overrides.
	AccountAllowanceCommand ValidateAccountAllowanceCommand(std::string_view provider_id, const std::vector<std::string>& command);

	AccountAllowanceFetchResult NormalizeCodexAccountAllowance(const nlohmann::json& response, std::int64_t now_ms);
	AccountAllowanceFetchResult NormalizeCopilotAccountAllowance(const nlohmann::json& response, std::int64_t now_ms);
	AccountAllowanceFetchResult FetchCliAccountAllowance(std::string_view provider_id, const std::vector<std::string>& command,
	    const std::filesystem::path& working_directory, std::stop_token stop = {});

	// A bounded transport seam for deterministic process ownership and protocol tests.
	AccountAllowanceFetchResult FetchCliAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    std::string_view provider_id, const std::vector<std::string>& command, const std::filesystem::path& working_directory,
	    std::stop_token stop = {}, std::chrono::milliseconds timeout = std::chrono::seconds(10));
}
