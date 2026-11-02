#include "common/usage/account_credentials.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace uam::usage
{
	namespace
	{
		constexpr std::size_t kCredentialLimit = 64 * 1024;
		constexpr std::size_t kOutputLimit = kCredentialLimit * 6 + 1024;
		// No Apple events or UI automation. kSecUseAuthenticationUIFail does not stop login-keychain ACL dialogs,
		// so automatic reads also disable Keychain interaction for this helper process.
		// The fixed system host lets cancellation reap a slow Keychain request without
		// leaving an uninterruptible Security.framework call in the desktop process.
		constexpr std::string_view kCredentialReadScript = R"JXA(ObjC.import('Foundation');
ObjC.import('Security');
ObjC.bindFunction('SecItemCopyMatching', ['int', ['id', 'id*']]);
function run(argv)
{
    function object(value) { return ObjC.castRefToObject(value); }
    const prompt = argv[2] === 'prompt';
    if (!prompt)
    {
        ObjC.bindFunction('SecKeychainSetUserInteractionAllowed', ['int', ['bool']]);
        if ($.SecKeychainSetUserInteractionAllowed(false) !== 0) return JSON.stringify({ status: -25308 });
    }
    const query = $.NSMutableDictionary.alloc.init;
    query.setObjectForKey(object($.kSecClassGenericPassword), object($.kSecClass));
    query.setObjectForKey($(argv[0]), object($.kSecAttrService));
    query.setObjectForKey($(argv[1]), object($.kSecAttrAccount));
    query.setObjectForKey($.NSNumber.numberWithBool(true), object($.kSecReturnData));
    query.setObjectForKey(object($.kSecMatchLimitOne), object($.kSecMatchLimit));
    if (!prompt) query.setObjectForKey(object($.kSecUseAuthenticationUIFail), object($.kSecUseAuthenticationUI));
    const result = Ref();
    const status = $.SecItemCopyMatching(query, result);
    if (status !== 0) return JSON.stringify({ status: status });
    const credential = ObjC.unwrap($.NSString.alloc.initWithDataEncoding(result[0], $.NSUTF8StringEncoding));
    if (typeof credential !== 'string' || credential.length > 65536) return JSON.stringify({ status: -1 });
    return JSON.stringify({ status: 0, credential: credential });
})JXA";
		AccountCredentialResult Failure(std::string message, bool unavailable = false)
		{
			AccountCredentialResult result; result.error = std::move(message); result.account_unavailable = unavailable; return result;
		}

		bool ValidIdentifiers(std::string_view service, std::string_view account)
		{
			for (const auto name : {service, account})
				if (name.empty() || name.size() > 1024 || std::any_of(name.begin(), name.end(), [](unsigned char character) { return character < 32 || character == 127; }))
					return false;
			return true;
		}

		enum class HelperOutcome { Exited, StartFailed, ReadFailed, Oversized, Canceled, TimedOut };
		struct HelperRun { HelperOutcome outcome = HelperOutcome::StartFailed; std::string output; int exit_code = -1; };

		// Runs one bounded credential helper. Its stderr can contain diagnostics, so it is drained and never exposed or logged.
		HelperRun RunCredentialHelper(const IPlatformProcessService& process_service, const std::vector<std::string>& arguments,
		    std::stop_token stop, std::chrono::milliseconds timeout)
		{
			HelperRun run;
			const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, std::chrono::milliseconds(120000));
			platform::StdioProcessPlatformFields process;
			if (!process_service.StartStdioProcess(process, {}, arguments, nullptr)) return run;
			struct Guard
			{
				const IPlatformProcessService& service; platform::StdioProcessPlatformFields& process;
				~Guard() { service.StopStdioProcess(process, true); service.CloseStdioProcessHandles(process); }
			} guard{process_service, process};
			process_service.CloseStdioProcessInput(process);
			std::array<char, 8192> bytes{};
			const auto fail = [&run](HelperOutcome outcome) { std::fill(run.output.begin(), run.output.end(), '\0'); run.output.clear(); run.outcome = outcome; return run; };
			while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				// Observe exit before the final read: output written before exit is already in the pipe.
				const bool exit_observed = process_service.PollStdioProcessExited(process, &run.exit_code);
				const auto read = process_service.ReadStdioProcessStdout(process, bytes.data(), bytes.size(), nullptr);
				if (read == -1) return fail(HelperOutcome::ReadFailed);
				if (read > 0)
				{
					if (run.output.size() + static_cast<std::size_t>(read) > kOutputLimit) return fail(HelperOutcome::Oversized);
					run.output.append(bytes.data(), static_cast<std::size_t>(read));
				}
				(void)process_service.ReadStdioProcessStderr(process, bytes.data(), bytes.size(), nullptr);
				if (read <= 0 && exit_observed) { run.outcome = HelperOutcome::Exited; return run; }
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return fail(stop.stop_requested() ? HelperOutcome::Canceled : HelperOutcome::TimedOut);
		}

		// Platform-neutral wording for every store except the macOS Keychain.
		constexpr const char* kStoreCanceled = "Credential store access was canceled.";
		constexpr const char* kStoreTimedOut = "Credential store access timed out; unlock this computer's keyring and refresh.";
		constexpr const char* kStoreInvalidIdentifiers = "Credential store identifiers are invalid.";
		constexpr const char* kStoreUnreadable = "The existing login could not be read from this computer's credential store.";
		constexpr const char* kStoreInvalid = "This computer's credential store returned an invalid credential.";

#if defined(_WIN32)
		AccountCredentialResult ReadWindowsCredential(std::string_view service, std::string_view account, std::stop_token stop)
		{
			if (stop.stop_requested()) return Failure(kStoreCanceled);
			if (!ValidIdentifiers(service, account)) return Failure(kStoreInvalidIdentifiers);
			// go-keyring stores a generic credential named "service:account" whose blob is the UTF-8 secret; CredRead never shows UI.
			// ponytail: ANSI target name is fine for the ASCII identifiers callers pass; switch to CredReadW for non-ASCII names.
			const std::string target = std::string(service) + ":" + std::string(account);
			PCREDENTIALA stored = nullptr;
			if (!CredReadA(target.c_str(), CRED_TYPE_GENERIC, 0, &stored))
				return GetLastError() == ERROR_NOT_FOUND ? Failure("No existing login was found in this computer's credential store.", true) : Failure(kStoreUnreadable);
			AccountCredentialResult result;
			if (stored->CredentialBlob && stored->CredentialBlobSize > 0 && stored->CredentialBlobSize <= kCredentialLimit)
				result.credential.assign(reinterpret_cast<const char*>(stored->CredentialBlob), stored->CredentialBlobSize);
			if (stored->CredentialBlob) SecureZeroMemory(stored->CredentialBlob, stored->CredentialBlobSize);
			CredFree(stored);
			if (result.credential.empty()) return Failure(kStoreInvalid);
			return result;
		}
#endif
	}

	AccountCredentialResult ReadAccountCredential(std::string_view service, std::string_view account, bool allow_prompt,
	    std::stop_token stop, std::chrono::milliseconds timeout)
	{
#if defined(__APPLE__)
		return ReadMacAccountCredentialWithProcessService(PlatformServicesFactory::Instance().process_service, service, account, allow_prompt, stop, timeout);
#elif defined(_WIN32)
		(void)allow_prompt; (void)timeout;
		return ReadWindowsCredential(service, account, stop);
#else
		return ReadSecretServiceCredentialWithProcessService(PlatformServicesFactory::Instance().process_service, service, account, allow_prompt, stop, timeout);
#endif
	}

	AccountCredentialResult ReadSecretServiceCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, bool allow_prompt, std::stop_token stop, std::chrono::milliseconds timeout)
	{
		if (stop.stop_requested()) return Failure(kStoreCanceled);
		if (timeout.count() <= 0) return Failure(kStoreTimedOut);
		if (!ValidIdentifiers(service, account)) return Failure(kStoreInvalidIdentifiers);
		// go-keyring's Secret Service items carry "service" and "username" attributes. Without --unlock, search
		// returns a locked item's attributes but not its secret and never shows an unlock prompt.
		std::vector<std::string> arguments{"secret-tool", "search"};
		if (allow_prompt) arguments.push_back("--unlock");
		arguments.insert(arguments.end(), {"service", std::string(service), "username", std::string(account)});
		auto run = RunCredentialHelper(process_service, arguments, stop, timeout);
		constexpr const char* kStoreAbsent = "No Secret Service credential store is available on this computer; install secret-tool (libsecret) and a keyring to read the existing login.";
		switch (run.outcome)
		{
		case HelperOutcome::StartFailed: return Failure(kStoreAbsent, true);
		case HelperOutcome::ReadFailed: case HelperOutcome::Oversized: return Failure(kStoreUnreadable);
		case HelperOutcome::Canceled: return Failure(kStoreCanceled);
		case HelperOutcome::TimedOut: return Failure(kStoreTimedOut);
		case HelperOutcome::Exited: break;
		}
		if (run.exit_code == 127) return Failure(kStoreAbsent, true);
		// secret-tool exits nonzero both when nothing matches and when no Secret Service answers.
		if (run.exit_code != 0) return Failure("No existing login was found in this computer's credential store, or no Secret Service is running.", true);
		// ponytail: reads a one-line secret; go-keyring callers store compact JSON. Parse multi-line output if a store needs it.
		std::size_t begin = run.output.starts_with("secret = ") ? 0 : run.output.find("\nsecret = ");
		AccountCredentialResult result;
		if (begin != std::string::npos)
		{
			begin += run.output[begin] == '\n' ? 10 : 9;
			const std::size_t end = run.output.find('\n', begin);
			result.credential = run.output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		}
		std::fill(run.output.begin(), run.output.end(), '\0');
		if (begin == std::string::npos)
			return Failure(allow_prompt ? "This computer's credential store did not unlock the existing login." : "The existing login is in a locked credential store; unlock this computer's keyring or choose Refresh allowances.");
		if (result.credential.empty() || result.credential.size() > kCredentialLimit) { std::fill(result.credential.begin(), result.credential.end(), '\0'); return Failure(kStoreInvalid); }
		return result;
	}

	AccountCredentialResult ReadMacAccountCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, bool allow_prompt, std::stop_token stop, std::chrono::milliseconds timeout)
	{
		if (stop.stop_requested()) return Failure("macOS account access was canceled.");
		if (timeout.count() <= 0) return Failure("macOS account access timed out; unlock your existing login keychain and refresh.");
		if (!ValidIdentifiers(service, account)) return Failure("macOS account storage identifiers are invalid.");
		const std::vector<std::string> arguments{"/usr/bin/osascript", "-l", "JavaScript", "-e", std::string(kCredentialReadScript), std::string(service), std::string(account), allow_prompt ? "prompt" : "silent"};
		auto run = RunCredentialHelper(process_service, arguments, stop, timeout);
		switch (run.outcome)
		{
		case HelperOutcome::StartFailed: return Failure("macOS account storage could not be opened.");
		case HelperOutcome::ReadFailed: return Failure("macOS account storage response could not be read.");
		case HelperOutcome::Oversized: return Failure("macOS account storage response exceeded its size limit.");
		case HelperOutcome::Canceled: return Failure("macOS account access was canceled.");
		case HelperOutcome::TimedOut: return Failure("macOS account access timed out; unlock your existing login keychain and refresh.");
		case HelperOutcome::Exited: break;
		}
		if (run.exit_code != 0) return Failure("macOS could not read the existing account without a permission prompt.");
		const auto response = nlohmann::json::parse(run.output, nullptr, false); std::fill(run.output.begin(), run.output.end(), '\0');
		if (!response.is_object() || !response.contains("status") || !response["status"].is_number_integer()) return Failure("macOS account storage response is invalid.");
		if (response["status"] == -25300) return Failure("No existing account credential was found in the login keychain.", true);
		if (!allow_prompt && (response["status"] == -25293 || response["status"] == -25308))
			return Failure("macOS needs permission or an unlocked keychain to read the existing login; choose Refresh allowances on this Mac to allow it.");
		if (response["status"] != 0) return Failure("macOS did not allow access to the existing login keychain.");
		if (!response.contains("credential") || !response["credential"].is_string() || response["credential"].get_ref<const std::string&>().empty() || response["credential"].get_ref<const std::string&>().size() > kCredentialLimit)
			return Failure("macOS account storage returned an invalid credential.");
		AccountCredentialResult result; result.credential = response["credential"].get<std::string>(); return result;
	}
}
