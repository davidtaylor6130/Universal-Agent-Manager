#include "common/usage/google_account_allowance.h"
#include "common/usage/transcript_usage.h"
#include "common/usage/account_credentials_macos.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/time_utils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <set>
#include <thread>

namespace uam::usage
{
	namespace
	{
		using Json = nlohmann::json;
		// These are public installed-application OAuth registrations distributed by
		// the respective Google CLIs, not user credentials. Refreshes reuse an
		// existing grant in memory and never enroll accounts or write token files.
		constexpr std::string_view kGeminiClientId = "";
		constexpr std::string_view kGeminiClientSecret = "";
		constexpr std::string_view kAntigravityClientId = "";
		constexpr std::string_view kAntigravityClientSecret = "";
		constexpr std::size_t kOutputLimit = 1024 * 1024;
		AccountAllowanceFetchResult Failure(std::string message, bool unavailable = false)
		{
			AccountAllowanceFetchResult result; result.error = std::move(message); result.account_unavailable = unavailable; return result;
		}
		const Json* Field(const Json& value, const char* key)
		{
			if (!value.is_object()) return nullptr;
			const auto found = value.find(key); return found == value.end() ? nullptr : &*found;
		}
		std::string Text(const Json& value, const char* key)
		{
			const Json* field = Field(value, key);
			return field && field->is_string() && field->get_ref<const std::string&>().size() <= 128 ? field->get<std::string>() : std::string{};
		}
		Json EmptyLimits(std::int64_t now)
		{
			return {{"updatedAt", now}, {"limitId", ""}, {"limitName", ""}, {"primary", nullptr}, {"secondary", nullptr}, {"credits", nullptr},
			    {"individualLimit", nullptr}, {"spendControlReached", nullptr}, {"planType", ""}, {"rateLimitReachedType", ""}};
		}
		Json Window(const Json& bucket, const std::string& id, const std::string& label)
		{
			const Json* fraction = Field(bucket, "remainingFraction");
			const Json* disabled = Field(bucket, "disabled");
			if ((disabled && disabled->is_boolean() && disabled->get<bool>()) || !fraction || !fraction->is_number() || id.empty() || id.size() > 128 || label.empty() || label.size() > 128) return nullptr;
			const double remaining = fraction->get<double>();
			if (!std::isfinite(remaining) || remaining < 0 || remaining > 1) return nullptr;
			Json reset = nullptr;
			if (const auto timestamp = TimestampMilliseconds(Text(bucket, "resetTime")); timestamp && *timestamp > 0) reset = *timestamp / 1000;
			return {{"id", id}, {"label", label}, {"usedPercent", (1 - remaining) * 100}, {"resetsAt", reset}, {"windowDurationMinutes", nullptr}};
		}
		bool ValidCredential(const std::string& value)
		{
			return !value.empty() && value.size() <= 16384 && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 33 || c > 126; });
		}
		std::string String(const Json& object, const char* key)
		{
			const Json* value = Field(object, key);
			return value && value->is_string() ? value->get<std::string>() : std::string{};
		}
		void Wipe(std::string& value)
		{
			std::fill(value.begin(), value.end(), '\0'); value.clear();
		}
		void WipeSecrets(Json& object)
		{
			if (!object.is_object()) return;
			for (const char* key : {"access_token", "refresh_token", "id_token"})
				if (auto found = object.find(key); found != object.end() && found->is_string()) Wipe(found->get_ref<std::string&>());
			if (auto found = object.find("token"); found != object.end())
			{
				if (found->is_object()) WipeSecrets(*found);
				else if (found->is_string()) Wipe(found->get_ref<std::string&>());
			}
		}
		struct CredentialGuard
		{
			Json& object;
			~CredentialGuard() { WipeSecrets(object); }
		};
		std::string FormEncode(std::string_view value)
		{
			constexpr char hex[] = "0123456789ABCDEF";
			std::string result;
			for (unsigned char c : value)
			{
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
				else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
			}
			return result;
		}
		struct HttpResult
		{
			Json body = nullptr;
			std::string error;
			int status = 0;
		};
		struct HttpContext
		{
			const IPlatformProcessService& service;
			const std::filesystem::path& directory;
			std::stop_token stop;
			std::chrono::steady_clock::time_point deadline;
			std::string_view user_agent = "UniversalAgentManager/account-allowance";
			std::size_t bytes_read = 0;

			HttpResult Post(std::string_view url, std::string body, const std::string& token = {})
			{
				if (stop.stop_requested()) return {nullptr, "Google account allowance refresh was canceled."};
				if (std::chrono::steady_clock::now() >= deadline) return {nullptr, "Google account allowance refresh timed out."};
				std::string input = "request = \"POST\"\nheader = " + Json(token.empty() ? "Content-Type: application/x-www-form-urlencoded" : "Content-Type: application/json").dump() + "\ndata = " + Json(body).dump() + "\n";
				Wipe(body);
				if (!token.empty()) input += "header = " + Json("Authorization: Bearer " + token).dump() + "\nheader = " + Json("User-Agent: " + std::string(user_agent)).dump() + "\n";
#if defined(_WIN32)
				const char* system_root = std::getenv("SystemRoot");
				if (!system_root || !*system_root) { Wipe(input); return {nullptr, "The system HTTPS account reader is unavailable."}; }
				const std::string executable = paths::Utf8PathString(std::filesystem::u8path(system_root) / "System32" / "curl.exe");
#else
				const std::string executable = "/usr/bin/curl";
#endif
				const std::vector<std::string> arguments{executable, "-q", "--silent", "--proto", "=https", "--connect-timeout", "3", "--max-time", "10", "--max-filesize", std::to_string(kOutputLimit), "--config", "-", "--write-out", "\n%{http_code}", std::string(url)};
				platform::StdioProcessPlatformFields process;
				if (!service.StartStdioProcess(process, directory, arguments)) { Wipe(input); return {nullptr, "The system HTTPS account reader could not start."}; }
				struct Guard
				{
					const IPlatformProcessService& service; platform::StdioProcessPlatformFields& process;
					~Guard() { service.StopStdioProcess(process, true); }
				} guard{service, process};
				const bool written = service.WriteToStdioProcess(process, input.data(), input.size()); Wipe(input);
				if (!written) return {nullptr, "The Google account request could not be sent."};
				service.CloseStdioProcessInput(process);
				std::array<char, 8192> bytes{}; std::string output; int exit_code = -1; bool exited = false;
				while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
				{
					bool progress = false;
					for (bool stderr_stream : {false, true})
					{
						const auto count = stderr_stream ? service.ReadStdioProcessStderr(process, bytes.data(), bytes.size()) : service.ReadStdioProcessStdout(process, bytes.data(), bytes.size());
						if (count == -1) { Wipe(output); return {nullptr, "The Google account response could not be read."}; }
						if (count <= 0) continue;
						progress = true;
						if (static_cast<std::size_t>(count) > kOutputLimit - bytes_read) { Wipe(output); return {nullptr, "The Google account response exceeded its size limit."}; }
						bytes_read += static_cast<std::size_t>(count);
						if (!stderr_stream) output.append(bytes.data(), static_cast<std::size_t>(count));
					}
					if (!progress && service.PollStdioProcessExited(process, &exit_code)) { exited = true; break; }
					if (!progress) std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				if (stop.stop_requested()) { Wipe(output); return {nullptr, "Google account allowance refresh was canceled."}; }
				if (!exited) { Wipe(output); return {nullptr, "Google account allowance refresh timed out."}; }
				if (exit_code != 0) { Wipe(output); return {nullptr, "Google account allowances could not be reached over HTTPS."}; }
				const auto newline = output.rfind('\n');
				if (newline == std::string::npos || output.size() - newline != 4 || output[newline + 1] < '1' || output[newline + 1] > '5' || output[newline + 2] < '0' || output[newline + 2] > '9' || output[newline + 3] < '0' || output[newline + 3] > '9')
				{ Wipe(output); return {nullptr, "The Google account HTTP response was invalid."}; }
				const int status = (output[newline + 1] - '0') * 100 + (output[newline + 2] - '0') * 10 + output[newline + 3] - '0';
				Json parsed = Json::parse(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(newline), nullptr, false); Wipe(output);
				if (!parsed.is_object()) return {nullptr, "The Google account response was invalid.", status};
				if (status < 200 || status >= 300) { WipeSecrets(parsed); return {nullptr, "Google could not return account allowances for the existing login.", status}; }
				return {std::move(parsed), {}, status};
			}
		};
		AccountAllowanceFetchResult FetchGoogle(HttpContext& context, Json& credentials, bool antigravity)
		{
			CredentialGuard credential_guard{credentials};
			if (context.stop.stop_requested()) return Failure("Google account allowance refresh was canceled.");
			if (!credentials.is_object()) return Failure("No valid existing Google account login is available.", true);
			Json token_fields = antigravity ? credentials.value("token", Json::object()) : credentials;
			if (token_fields.is_string()) token_fields = Json::parse(token_fields.get_ref<const std::string&>(), nullptr, false);
			CredentialGuard token_guard{token_fields};
			if (!token_fields.is_object()) return Failure("No valid existing Google account login is available.", true);
			std::string token = String(token_fields, "access_token"), refresh = String(token_fields, "refresh_token");
			struct TokenGuard { std::string& token; std::string& refresh; ~TokenGuard() { Wipe(token); Wipe(refresh); } } token_cleanup{token, refresh};
			if ((!token.empty() && !ValidCredential(token)) || (!refresh.empty() && !ValidCredential(refresh))) return Failure("The existing Google account credential format is invalid.", true);
			bool expired = true;
			if (antigravity)
			{
				const auto expiry = TimestampMilliseconds(String(token_fields, "expiry")); expired = !expiry || *expiry <= time::SystemEpochMillisecondsNow();
				const std::string method = String(credentials, "auth_method");
				if (!method.empty() && method != "consumer") return Failure("This Antigravity login type does not expose independent account allowances here.", true);
			}
			else if (const Json* expiry = Field(token_fields, "expiry_date"); expiry && expiry->is_number())
			{
				const double value = expiry->get<double>(); expired = !std::isfinite(value) || value <= static_cast<double>(time::SystemEpochMillisecondsNow());
			}
			if (token.empty() || expired)
			{
				if (refresh.empty()) return Failure("The existing Google account login has expired; refresh it in the provider CLI.", true);
				const auto client_id = antigravity ? kAntigravityClientId : kGeminiClientId;
				const auto secret = antigravity ? kAntigravityClientSecret : kGeminiClientSecret;
				std::string body = "grant_type=refresh_token&client_id=" + FormEncode(client_id) + "&client_secret=" + FormEncode(secret) + "&refresh_token=" + FormEncode(refresh);
				HttpResult fresh = context.Post("https://oauth2.googleapis.com/token", std::move(body));
				CredentialGuard fresh_guard{fresh.body};
				if (!fresh.error.empty()) return Failure(fresh.error, fresh.status == 400 || fresh.status == 401 || fresh.status == 403);
				token = String(fresh.body, "access_token");
				if (!ValidCredential(token)) return Failure("The existing Google account grant did not return a valid access token.", true);
			}
			const Json metadata{{"metadata", {{"ideType", "IDE_UNSPECIFIED"}, {"platform", "PLATFORM_UNSPECIFIED"}, {"pluginType", "GEMINI"}}}};
			HttpResult account = context.Post("https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist", metadata.dump(), token);
			if (!account.error.empty()) return Failure(account.error, account.status == 401 || account.status == 403);
			const std::string project = String(account.body, "cloudaicompanionProject");
			if (project.empty() || project.size() > 1024)
			{
				if (const Json* reasons = Field(account.body, "ineligibleTiers"); reasons && reasons->is_array())
					for (const auto& reason : *reasons) if (String(reason, "reasonCode") == "UNSUPPORTED_CLIENT")
						return Failure("Google reports this Gemini CLI account is unsupported by the current client.", true);
				return Failure("Google did not return an existing eligible account project; account enrollment is required in the provider CLI.", true);
			}
			HttpResult quota = context.Post(antigravity ? "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary" : "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota", Json{{"project", project}}.dump(), token);
			if (!quota.error.empty()) return Failure(quota.error, quota.status == 401 || quota.status == 403);
			return antigravity ? NormalizeAntigravityAccountAllowance(quota.body, time::SystemEpochMillisecondsNow()) : NormalizeGeminiAccountAllowance(quota.body, time::SystemEpochMillisecondsNow());
		}
	}

	AccountAllowanceFetchResult NormalizeGeminiAccountAllowance(const nlohmann::json& response, std::int64_t now)
	{
		const Json* buckets = Field(response, "buckets");
		if (now <= 0 || now > 9007199254740991LL || !buckets || !buckets->is_array() || buckets->size() > 64)
			return Failure("Gemini CLI account quota response is unavailable or invalid.");
		AccountAllowanceFetchResult result;
		std::set<std::string> seen;
		for (const auto& bucket : *buckets)
		{
			const std::string model = Text(bucket, "modelId"), type = Text(bucket, "tokenType");
			if (model.empty() && type.empty()) continue;
			std::string id = model + (type.empty() ? "" : ":" + type);
			if (seen.contains(id)) id += ":" + std::to_string(result.windows.size());
			const std::string label = model.empty() ? type : model + (type.empty() ? "" : " · " + type);
			Json window = Window(bucket, id, label);
			if (!window.is_null()) { seen.insert(id); result.windows.push_back(std::move(window)); }
		}
		if (result.windows.empty()) return Failure("Gemini CLI did not report readable account quota buckets.");
		result.limits = EmptyLimits(now); result.limits["limitId"] = "gemini-account"; result.limits["limitName"] = "Gemini CLI account";
		return result;
	}

	AccountAllowanceFetchResult NormalizeAntigravityAccountAllowance(const nlohmann::json& response, std::int64_t now)
	{
		if (now <= 0 || now > 9007199254740991LL || !response.is_object()) return Failure("Antigravity account quota response is unavailable or invalid.");
		AccountAllowanceFetchResult result;
		std::set<std::string> seen;
		bool oversized = false;
		const auto append = [&](const Json* buckets, const std::string& group)
		{
			if (!buckets || !buckets->is_array()) return;
			if (buckets->size() > 64) { oversized = true; return; }
			for (const auto& bucket : *buckets)
			{
				const std::string id = Text(bucket, "bucketId");
				if (seen.contains(id)) continue;
				std::string label = Text(bucket, "displayName"); if (label.empty()) label = id;
				if (!group.empty()) label = group + " · " + label;
				Json window = Window(bucket, id, label);
				if (!window.is_null()) { seen.insert(id); result.windows.push_back(std::move(window)); }
				if (result.windows.size() > 64) oversized = true;
			}
		};
		if (const Json* groups = Field(response, "groups"); groups && groups->is_array())
		{
			if (groups->size() > 64) oversized = true;
			else for (const auto& group : *groups) append(Field(group, "buckets"), Text(group, "displayName"));
		}
		append(Field(response, "buckets"), "");
		if (oversized || result.windows.empty()) return Failure("Antigravity did not report valid readable account quota buckets.");
		result.limits = EmptyLimits(now); result.limits["limitId"] = "antigravity-account"; result.limits["limitName"] = "Antigravity account";
		return result;
	}
	AccountAllowanceFetchResult FetchGeminiAccountAllowance(const std::filesystem::path& directory, std::stop_token stop)
	{
		if (stop.stop_requested()) return Failure("Gemini CLI account allowance refresh was canceled.");
		const char* custom_home = std::getenv("GEMINI_CLI_HOME");
		const auto home = custom_home && *custom_home ? std::optional<std::filesystem::path>{std::filesystem::u8path(custom_home)} : PlatformServicesFactory::Instance().path_service.ResolveUserHomePath();
		if (!home) return Failure("No Gemini CLI account directory is available.", true);
		std::string encoded;
		if (!io::TryReadTextFile(*home / ".gemini" / "oauth_creds.json", encoded, 64 * 1024)) return Failure("No existing Gemini CLI subscription login is available.", true);
		Json credentials = Json::parse(encoded, nullptr, false); Wipe(encoded);
		if (!credentials.is_object()) return Failure("The existing Gemini CLI account credential is invalid.", true);
		return FetchGeminiAccountAllowanceWithProcessService(PlatformServicesFactory::Instance().process_service, directory, std::move(credentials), stop);
	}
	AccountAllowanceFetchResult FetchAntigravityAccountAllowance(const std::filesystem::path& directory, std::stop_token stop)
	{
		auto credential = ReadMacAccountCredential("gemini", "antigravity", stop);
		if (!credential.error.empty()) return Failure(credential.error, credential.account_unavailable);
		if (credential.credential.starts_with("go-keyring-base64:"))
		{
			std::string decoded;
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			unsigned value = 0; int bits = 0;
			for (char c : std::string_view(credential.credential).substr(18))
			{
				if (c == '=') break;
				const auto digit = alphabet.find(c);
				if (digit == std::string_view::npos) { Wipe(credential.credential); Wipe(decoded); return Failure("The existing Antigravity account credential encoding is invalid.", true); }
				value = (value << 6) | static_cast<unsigned>(digit); bits += 6;
				if (bits >= 8) { bits -= 8; decoded += static_cast<char>((value >> bits) & 255); }
			}
			Wipe(credential.credential); credential.credential = std::move(decoded);
		}
		Json profile = Json::parse(credential.credential, nullptr, false); Wipe(credential.credential);
		if (!profile.is_object()) return Failure("The existing Antigravity account profile is invalid.", true);
		return FetchAntigravityAccountAllowanceWithProcessService(PlatformServicesFactory::Instance().process_service, directory, std::move(profile), stop);
	}
	AccountAllowanceFetchResult FetchGeminiAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    const std::filesystem::path& directory, Json credentials, std::stop_token stop, std::chrono::milliseconds timeout)
	{
		HttpContext context{service, directory, stop, std::chrono::steady_clock::now() + std::min(timeout, std::chrono::milliseconds(10000))};
		return FetchGoogle(context, credentials, false);
	}
	AccountAllowanceFetchResult FetchAntigravityAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    const std::filesystem::path& directory, Json profile, std::stop_token stop, std::chrono::milliseconds timeout)
	{
		HttpContext context{service, directory, stop, std::chrono::steady_clock::now() + std::min(timeout, std::chrono::milliseconds(10000))};
		context.user_agent = "antigravity";
		return FetchGoogle(context, profile, true);
	}
}
