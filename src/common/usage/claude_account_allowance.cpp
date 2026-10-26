#include "common/usage/claude_account_allowance.h"
#include "common/usage/account_credentials_macos.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/usage/transcript_usage.h"
#include "common/utils/io_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <thread>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace uam::usage
{
	namespace
	{
		constexpr std::size_t kCredentialLimit = 64 * 1024;
		constexpr std::size_t kResponseLimit = 128 * 1024;

		AccountAllowanceFetchResult Failure(std::string message, bool account_unavailable = false)
		{
			AccountAllowanceFetchResult result; result.error = std::move(message); result.account_unavailable = account_unavailable; return result;
		}

		nlohmann::json Window(const nlohmann::json& response, const char* key, int minutes)
		{
			const auto window = response.find(key);
			if (window == response.end() || !window->is_object()) return nullptr;
			const auto utilization = window->find("utilization");
			if (utilization == window->end() || !utilization->is_number()) return nullptr;
			// The OAuth usage endpoint reports percentages; rate_limit_event reports fractions.
			const double percent = utilization->get<double>();
			if (!std::isfinite(percent) || percent < 0 || percent > 100) return nullptr;
			nlohmann::json reset = nullptr;
			const auto timestamp = window->find("resets_at");
			if (timestamp != window->end() && timestamp->is_string() && timestamp->get_ref<const std::string&>().size() <= 128)
			{
				const auto milliseconds = TimestampMilliseconds(timestamp->get_ref<const std::string&>());
				if (milliseconds && *milliseconds > 0) reset = *milliseconds / 1000;
			}
			return {{"usedPercent", percent}, {"resetsAt", reset}, {"windowDurationMinutes", minutes}};
		}

		std::string Environment(const char* name)
		{
			const char* value = std::getenv(name); return value ? std::string(value) : std::string{};
		}

#if defined(__APPLE__)
		std::string KeychainService(const std::string& configured_directory)
		{
			std::string service = "Claude Code-credentials";
			if (configured_directory.empty()) return service;
			// Claude Code normalizes its config directory to NFC before hashing its keychain suffix.
			CFStringRef raw = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(configured_directory.data()), static_cast<CFIndex>(configured_directory.size()), kCFStringEncodingUTF8, false);
			if (!raw) return {};
			CFMutableStringRef normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, raw); CFRelease(raw);
			if (!normalized) return {};
			CFStringNormalize(normalized, kCFStringNormalizationFormC);
			const CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(normalized), kCFStringEncodingUTF8) + 1;
			std::string text(static_cast<std::size_t>(capacity), '\0');
			const bool converted = CFStringGetCString(normalized, text.data(), capacity, kCFStringEncodingUTF8); CFRelease(normalized);
			if (!converted) return {};
			text.resize(std::char_traits<char>::length(text.c_str()));
			std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
			CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), digest.data());
			static constexpr char hex[] = "0123456789abcdef";
			service += '-';
			for (std::size_t index = 0; index < 4; ++index) { service += hex[digest[index] >> 4]; service += hex[digest[index] & 15]; }
			return service;
		}

		MacAccountCredentialResult ReadKeychainCredentials(const std::string& configured_directory, std::stop_token stop)
		{
			const std::string service = KeychainService(configured_directory); if (service.empty()) return {};
			std::string account = Environment("USER");
			if (account.empty()) { const passwd* user = getpwuid(getuid()); if (user && user->pw_name) account = user->pw_name; }
			if (account.empty()) return {};
			return ReadMacAccountCredential(service, account, stop);
		}
#endif

		std::string ReadAccessToken(std::stop_token stop, std::string& error, bool& account_unavailable)
		{
			std::string token = Environment("CLAUDE_CODE_OAUTH_TOKEN"); if (!token.empty()) return token;
			const std::string configured_directory = Environment("CLAUDE_CONFIG_DIR");
			if (configured_directory.size() > 4096) { error = "Claude Code configuration location is invalid."; account_unavailable = true; return {}; }
			std::string encoded;
			MacAccountCredentialResult keychain; keychain.account_unavailable = true;
#if defined(__APPLE__)
			keychain = ReadKeychainCredentials(configured_directory, stop); encoded = std::move(keychain.credential);
#endif
			if (stop.stop_requested()) { error = "Claude Code account allowance refresh was canceled."; return {}; }
			const bool existing_keychain_value = !encoded.empty();
			nlohmann::json credentials = nlohmann::json::parse(encoded, nullptr, false);
			if (!credentials.is_object())
			{
				std::optional<std::filesystem::path> directory;
				if (!configured_directory.empty()) directory = std::filesystem::u8path(configured_directory);
				else if (const auto home = PlatformServicesFactory::Instance().path_service.ResolveUserHomePath()) directory = *home / ".claude";
				if (directory && io::TryReadTextFile(*directory / ".credentials.json", encoded, kCredentialLimit)) credentials = nlohmann::json::parse(encoded, nullptr, false);
			}
			std::fill(encoded.begin(), encoded.end(), '\0');
			if (!credentials.is_object() || !credentials.contains("claudeAiOauth") || !credentials["claudeAiOauth"].is_object())
			{
				error = keychain.error.empty() ? "No existing Claude Code subscription login is available without a permission prompt." : keychain.error;
				account_unavailable = existing_keychain_value || keychain.account_unavailable; return {};
			}
			const auto& oauth = credentials["claudeAiOauth"];
			const auto access = oauth.find("accessToken"), expiry = oauth.find("expiresAt"), scopes = oauth.find("scopes");
			if (access == oauth.end() || !access->is_string() || access->get_ref<const std::string&>().empty())
			{ error = "No existing Claude Code subscription login is available."; account_unavailable = true; return {}; }
			if (expiry != oauth.end() && expiry->is_number())
			{
				const double expires_at = expiry->get<double>();
				const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
				if (!std::isfinite(expires_at) || expires_at <= static_cast<double>(now_ms))
				{ error = "The existing Claude Code login has expired; refresh it in Claude Code."; account_unavailable = true; return {}; }
			}
			if (scopes == oauth.end() || !scopes->is_array() || std::find(scopes->begin(), scopes->end(), "user:profile") == scopes->end())
			{ error = "The existing Claude Code login does not expose account usage permissions."; account_unavailable = true; return {}; }
			return access->get<std::string>();
		}
	}

	AccountAllowanceFetchResult NormalizeClaudeAccountAllowance(const nlohmann::json& response, std::int64_t now_ms)
	{
		if (now_ms <= 0 || now_ms > kMaxExactTokenCount || !response.is_object()) return Failure("Claude Code account allowance response is unavailable or invalid.");
		nlohmann::json primary = Window(response, "five_hour", 300), secondary = Window(response, "seven_day", 10080);
		if (primary.is_null() && secondary.is_null()) return Failure("Claude Code did not report subscription allowance windows.");
		AccountAllowanceFetchResult result;
		result.limits = {{"updatedAt", now_ms}, {"limitId", "claude-account"}, {"limitName", "Claude subscription"}, {"primary", std::move(primary)}, {"secondary", std::move(secondary)},
		    {"credits", nullptr}, {"individualLimit", nullptr}, {"spendControlReached", nullptr}, {"planType", nullptr}, {"rateLimitReachedType", nullptr}};
		result.primary_label = "5-hour allowance"; result.secondary_label = "Weekly allowance"; return result;
	}

	AccountAllowanceFetchResult FetchClaudeAccountAllowance(const std::filesystem::path& working_directory, std::stop_token stop)
	{
		if (stop.stop_requested()) return Failure("Claude Code account allowance refresh was canceled.");
		if (!Environment("CLAUDE_CODE_CUSTOM_OAUTH_URL").empty()) return Failure("Claude Code uses a custom OAuth service; its account allowances are unavailable here.", true);
		bool account_unavailable = false;
		std::string error, token = ReadAccessToken(stop, error, account_unavailable);
		if (token.empty()) return Failure(error.empty() ? "No existing Claude Code subscription login is available." : std::move(error), account_unavailable);
		return FetchClaudeAccountAllowanceWithProcessService(PlatformServicesFactory::Instance().process_service, working_directory, std::move(token), stop);
	}

	AccountAllowanceFetchResult FetchClaudeAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    const std::filesystem::path& working_directory, std::string token, std::stop_token stop, std::chrono::milliseconds timeout)
	{
		if (stop.stop_requested()) return Failure("Claude Code account allowance refresh was canceled.");
		if (timeout.count() <= 0) return Failure("Claude Code account allowance refresh timed out.");
		const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, std::chrono::milliseconds(10000));
		if (token.empty() || token.size() > 16 * 1024 || std::any_of(token.begin(), token.end(), [](unsigned char value) { return value < 32 || value > 126; }))
		{ std::fill(token.begin(), token.end(), '\0'); return Failure("The existing Claude Code login has an invalid credential format.", true); }
		std::string input = "header = \"Authorization: Bearer ";
		for (char character : token) { if (character == '\\' || character == '"') input += '\\'; input += character; }
		input += "\"\n"; std::fill(token.begin(), token.end(), '\0');
#if defined(_WIN32)
		const std::string system_root = Environment("SystemRoot");
		if (system_root.empty()) return Failure("The system HTTPS client is unavailable for Claude Code account allowances.");
		const std::string executable = paths::Utf8PathString(std::filesystem::u8path(system_root) / "System32" / "curl.exe");
#else
		const std::string executable = "/usr/bin/curl";
#endif
		const std::vector<std::string> arguments{executable, "-q", "--silent", "--proto", "=https", "--connect-timeout", "3", "--max-time", "8", "--max-filesize", std::to_string(kResponseLimit),
		    "--header", "anthropic-beta: oauth-2025-04-20", "--header", "Content-Type: application/json", "--config", "-", "--write-out", "\n%{http_code}", "https://api.anthropic.com/api/oauth/usage"};
		platform::StdioProcessPlatformFields process;
		if (stop.stop_requested()) { std::fill(input.begin(), input.end(), '\0'); return Failure("Claude Code account allowance refresh was canceled."); }
		// A pipe keeps the bearer credential out of argv, environment, shell commands, and temporary input files.
		if (!service.StartStdioProcess(process, working_directory, arguments, nullptr))
		{ std::fill(input.begin(), input.end(), '\0'); return Failure("The system HTTPS client could not start for Claude Code account allowances."); }
		struct Guard
		{
			const IPlatformProcessService& service; platform::StdioProcessPlatformFields& process;
			~Guard() { service.StopStdioProcess(process, true); service.CloseStdioProcessHandles(process); }
		} guard{service, process};
		const bool written = service.WriteToStdioProcess(process, input.data(), input.size(), nullptr); std::fill(input.begin(), input.end(), '\0');
		if (!written) return Failure("Claude Code account allowance request could not be sent.");
		service.CloseStdioProcessInput(process);
		std::string output; std::array<char, 8192> bytes{}; int exit_code = -1; bool exited = false;
		while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
		{
			// Observe exit before the final read: output written before exit is already in the pipe.
			const bool exit_observed = service.PollStdioProcessExited(process, &exit_code);
			const auto read = service.ReadStdioProcessStdout(process, bytes.data(), bytes.size(), nullptr);
			if (read == -1) return Failure("Claude Code account allowance response could not be read.");
			if (read > 0)
			{
				if (output.size() + static_cast<std::size_t>(read) > kResponseLimit + 16) return Failure("Claude Code account allowance response exceeded its size limit.");
				output.append(bytes.data(), static_cast<std::size_t>(read));
			}
			// Stderr is drained and discarded; it must never become a UI error or application log.
			(void)service.ReadStdioProcessStderr(process, bytes.data(), bytes.size(), nullptr);
			if (read <= 0 && exit_observed) { exited = true; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (stop.stop_requested()) return Failure("Claude Code account allowance refresh was canceled.");
		if (!exited) return Failure("Claude Code account allowance refresh timed out.");
		if (exit_code != 0) return Failure("Claude Code account allowances could not be reached over HTTPS.");
		const auto status = output.rfind('\n');
		if (status == std::string::npos) return Failure("Claude Code account allowance response is invalid.");
		const std::string http_status = output.substr(status + 1); output.resize(status);
		if (http_status == "401" || http_status == "403") return Failure("Claude Code did not authorize this account allowance read; refresh its existing login.", true);
		if (http_status == "429") return Failure("Claude Code account allowance reads are rate limited; try again later.");
		if (http_status != "200") return Failure("Claude Code account allowance service is unavailable.");
		const std::int64_t fetched_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		return NormalizeClaudeAccountAllowance(nlohmann::json::parse(output, nullptr, false), fetched_at_ms);
	}
}
