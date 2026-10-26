#include "common/usage/account_credentials_macos.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <thread>

namespace uam::usage
{
	namespace
	{
		constexpr std::size_t kCredentialLimit = 64 * 1024;
		constexpr std::size_t kOutputLimit = kCredentialLimit * 6 + 1024;
		// No Apple events or UI automation: only a no-prompt Security.framework read.
		// The fixed system host lets cancellation reap a slow Keychain request without
		// leaving an uninterruptible Security.framework call in the desktop process.
		constexpr std::string_view kCredentialReadScript = R"JXA(ObjC.import('Foundation');
ObjC.import('Security');
ObjC.bindFunction('SecItemCopyMatching', ['int', ['id', 'id*']]);
function run(argv)
{
    function object(value) { return ObjC.castRefToObject(value); }
    const query = $.NSMutableDictionary.alloc.init;
    query.setObjectForKey(object($.kSecClassGenericPassword), object($.kSecClass));
    query.setObjectForKey($(argv[0]), object($.kSecAttrService));
    query.setObjectForKey($(argv[1]), object($.kSecAttrAccount));
    query.setObjectForKey($.NSNumber.numberWithBool(true), object($.kSecReturnData));
    query.setObjectForKey(object($.kSecMatchLimitOne), object($.kSecMatchLimit));
    query.setObjectForKey(object($.kSecUseAuthenticationUIFail), object($.kSecUseAuthenticationUI));
    const result = Ref();
    const status = $.SecItemCopyMatching(query, result);
    if (status !== 0) return JSON.stringify({ status: status });
    const credential = ObjC.unwrap($.NSString.alloc.initWithDataEncoding(result[0], $.NSUTF8StringEncoding));
    if (typeof credential !== 'string' || credential.length > 65536) return JSON.stringify({ status: -1 });
    return JSON.stringify({ status: 0, credential: credential });
})JXA";
		MacAccountCredentialResult Failure(std::string message, bool unavailable = false)
		{
			MacAccountCredentialResult result; result.error = std::move(message); result.account_unavailable = unavailable; return result;
		}
	}

	MacAccountCredentialResult ReadMacAccountCredential(std::string_view service, std::string_view account,
	    std::stop_token stop, std::chrono::milliseconds timeout)
	{
#if defined(__APPLE__)
		return ReadMacAccountCredentialWithProcessService(PlatformServicesFactory::Instance().process_service, service, account, stop, timeout);
#else
		(void)service; (void)account; (void)stop; (void)timeout;
		return Failure("macOS account storage is unavailable on this platform.");
#endif
	}

	MacAccountCredentialResult ReadMacAccountCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, std::stop_token stop, std::chrono::milliseconds timeout)
	{
		if (stop.stop_requested()) return Failure("macOS account access was canceled.");
		if (timeout.count() <= 0) return Failure("macOS account access timed out; unlock your existing login keychain and refresh.");
		for (const auto name : {service, account})
			if (name.empty() || name.size() > 1024 || std::any_of(name.begin(), name.end(), [](unsigned char character) { return character < 32 || character == 127; }))
				return Failure("macOS account storage identifiers are invalid.");
		const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, std::chrono::milliseconds(120000));
		const std::vector<std::string> arguments{"/usr/bin/osascript", "-l", "JavaScript", "-e", std::string(kCredentialReadScript), std::string(service), std::string(account)};
		platform::StdioProcessPlatformFields process;
		if (!process_service.StartStdioProcess(process, {}, arguments, nullptr)) return Failure("macOS account storage could not be opened.");
		struct Guard
		{
			const IPlatformProcessService& service; platform::StdioProcessPlatformFields& process;
			~Guard() { service.StopStdioProcess(process, true); service.CloseStdioProcessHandles(process); }
		} guard{process_service, process};
		process_service.CloseStdioProcessInput(process);
		std::string output; std::array<char, 8192> bytes{}; int exit_code = -1; bool exited = false;
		while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
		{
			// Observe exit before the final read: output written before exit is already in the pipe.
			const bool exit_observed = process_service.PollStdioProcessExited(process, &exit_code);
			const auto read = process_service.ReadStdioProcessStdout(process, bytes.data(), bytes.size(), nullptr);
			if (read == -1) return Failure("macOS account storage response could not be read.");
			if (read > 0)
			{
				if (output.size() + static_cast<std::size_t>(read) > kOutputLimit) return Failure("macOS account storage response exceeded its size limit.");
				output.append(bytes.data(), static_cast<std::size_t>(read));
			}
			// This stream can contain interpreter diagnostics, so it is never exposed or logged.
			(void)process_service.ReadStdioProcessStderr(process, bytes.data(), bytes.size(), nullptr);
			if (read <= 0 && exit_observed) { exited = true; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (stop.stop_requested()) return Failure("macOS account access was canceled.");
		if (!exited) return Failure("macOS account access timed out; unlock your existing login keychain and refresh.");
		if (exit_code != 0) return Failure("macOS could not read the existing account without a permission prompt.");
		const auto response = nlohmann::json::parse(output, nullptr, false); std::fill(output.begin(), output.end(), '\0');
		if (!response.is_object() || !response.contains("status") || !response["status"].is_number_integer()) return Failure("macOS account storage response is invalid.");
		if (response["status"] == -25300) return Failure("No existing account credential was found in the login keychain.", true);
		if (response["status"] != 0) return Failure("macOS did not allow access to the existing login keychain.");
		if (!response.contains("credential") || !response["credential"].is_string() || response["credential"].get_ref<const std::string&>().empty() || response["credential"].get_ref<const std::string&>().size() > kCredentialLimit)
			return Failure("macOS account storage returned an invalid credential.");
		MacAccountCredentialResult result; result.credential = response["credential"].get<std::string>(); return result;
	}
}
