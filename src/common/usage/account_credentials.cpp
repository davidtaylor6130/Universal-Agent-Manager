#include "common/usage/account_credentials.h"
#include "common/platform/credential_store.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace uam::usage
{
	namespace
	{
		constexpr std::size_t kCredentialLimit = 64 * 1024;
		constexpr std::size_t kOutputLimit = kCredentialLimit * 6 + 1024;

		// Volatile writes so the compiler cannot drop the wipe as a dead store before the buffer is freed.
		void Wipe(std::string& value)
		{
			volatile char* bytes = value.data();
			for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
			value.clear();
		}

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
		struct HelperRun
		{
			HelperOutcome outcome = HelperOutcome::StartFailed; std::string output; int exit_code = -1;
			// Helper output can hold the secret: every copy is wiped however its reader returns.
			~HelperRun() { Wipe(output); }
		};

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
			const auto fail = [&run](HelperOutcome outcome) { return HelperRun{outcome, {}, run.exit_code}; };
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
		constexpr const char* kStoreMissing = "No existing login was found in this computer's credential store.";
		constexpr const char* kStoreUnreadable = "The existing login could not be read from this computer's credential store.";
		constexpr const char* kStoreInvalid = "This computer's credential store returned an invalid credential.";

#if defined(_WIN32)
		AccountCredentialResult ReadWindowsCredential(std::string_view service, std::string_view account, std::stop_token stop)
		{
			if (stop.stop_requested()) return Failure(kStoreCanceled);
			if (!ValidIdentifiers(service, account)) return Failure(kStoreInvalidIdentifiers);
			// go-keyring stores a generic credential named "service:account" whose blob is the UTF-8 secret.
			auto stored = platform::ReadWindowsGenericCredential(std::string(service) + ":" + std::string(account), kCredentialLimit);
			if (stored.status == platform::StoredCredentialStatus::NotFound) return Failure(kStoreMissing, true);
			if (stored.status != platform::StoredCredentialStatus::Found) return Failure(kStoreUnreadable);
			AccountCredentialResult result; result.credential = std::move(stored.secret);
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
		auto run = RunCredentialHelper(process_service, platform::SecretServiceSearchArguments(service, account, allow_prompt), stop, timeout);
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
		// secret-tool exits nonzero when no Secret Service answers, and exits 0 with no output when nothing matches.
		if (run.exit_code != 0) return Failure("The existing login could not be read because no Secret Service credential store is running on this computer.", true);
		// Each matching item starts with a "[<id>]" header line; its attribute lines go to stderr, which is never kept.
		if (!run.output.starts_with('[') && run.output.find("\n[") == std::string::npos) return Failure(kStoreMissing, true);
		// ponytail: reads a one-line secret; go-keyring callers store compact JSON. Parse multi-line output if a store needs it.
		std::size_t begin = run.output.starts_with("secret = ") ? 0 : run.output.find("\nsecret = ");
		// An item without its secret is locked: automatic reads never unlock, so only Refresh can.
		if (begin == std::string::npos)
			return Failure(allow_prompt ? "This computer's credential store did not unlock the existing login." : "The existing login is in a locked credential store; unlock this computer's keyring or choose Refresh allowances.");
		begin += run.output[begin] == '\n' ? 10 : 9;
		const std::size_t end = run.output.find('\n', begin);
		AccountCredentialResult result; result.credential = run.output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		if (result.credential.empty() || result.credential.size() > kCredentialLimit) { Wipe(result.credential); return Failure(kStoreInvalid); }
		return result;
	}

	AccountCredentialResult ReadMacAccountCredentialWithProcessService(const IPlatformProcessService& process_service,
	    std::string_view service, std::string_view account, bool allow_prompt, std::stop_token stop, std::chrono::milliseconds timeout)
	{
		if (stop.stop_requested()) return Failure("macOS account access was canceled.");
		if (timeout.count() <= 0) return Failure("macOS account access timed out; unlock your existing login keychain and refresh.");
		if (!ValidIdentifiers(service, account)) return Failure("macOS account storage identifiers are invalid.");
		auto run = RunCredentialHelper(process_service, platform::MacKeychainReadArguments(service, account, allow_prompt), stop, timeout);
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
		const auto response = nlohmann::json::parse(run.output, nullptr, false);
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
