#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

class IPlatformProcessService;

namespace uam::usage
{
	struct AccountCredentialResult
	{
		// Private memory only: callers must never log or persist this value.
		std::string credential;
		std::string error;
		bool account_unavailable = false;
	};

	/// Reads an existing login from this computer's credential store: the macOS Keychain, Windows Credential Manager
	/// or the Linux Secret Service. Automatic reads never show a prompt; allow_prompt is only for an explicit user refresh.
	AccountCredentialResult ReadAccountCredential(std::string_view service, std::string_view account, bool allow_prompt = false,
	    std::stop_token stop = {}, std::chrono::milliseconds timeout = std::chrono::seconds(120));

	// Helper-process seams behind ReadAccountCredential, kept public for deterministic tests.
	AccountCredentialResult ReadMacAccountCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, bool allow_prompt = false, std::stop_token stop = {},
	    std::chrono::milliseconds timeout = std::chrono::seconds(120));
	AccountCredentialResult ReadSecretServiceCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, bool allow_prompt = false, std::stop_token stop = {},
	    std::chrono::milliseconds timeout = std::chrono::seconds(120));
}
