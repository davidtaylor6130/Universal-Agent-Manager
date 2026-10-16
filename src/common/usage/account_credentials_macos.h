#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

class IPlatformProcessService;

namespace uam::usage
{
	struct MacAccountCredentialResult
	{
		// Private memory only: callers must never log or persist this value.
		std::string credential;
		std::string error;
		bool account_unavailable = false;
	};

	MacAccountCredentialResult ReadMacAccountCredential(std::string_view service, std::string_view account,
	    std::stop_token stop = {}, std::chrono::milliseconds timeout = std::chrono::seconds(120));
	MacAccountCredentialResult ReadMacAccountCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, std::stop_token stop = {},
	    std::chrono::milliseconds timeout = std::chrono::seconds(120));
}
