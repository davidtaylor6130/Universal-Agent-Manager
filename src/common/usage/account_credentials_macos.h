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

	/// Automatic reads never show Keychain dialogs; allow_prompt is only for an explicit user refresh.
	MacAccountCredentialResult ReadMacAccountCredential(std::string_view service, std::string_view account, bool allow_prompt = false,
	    std::stop_token stop = {}, std::chrono::milliseconds timeout = std::chrono::seconds(120));
	MacAccountCredentialResult ReadMacAccountCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, bool allow_prompt = false, std::stop_token stop = {},
	    std::chrono::milliseconds timeout = std::chrono::seconds(120));
}
