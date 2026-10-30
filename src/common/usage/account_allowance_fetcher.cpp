#include "account_allowance_fetcher.h"
#include "common/provider/provider_ids.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/usage/transcript_usage.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <optional>
#include <thread>

namespace uam::usage
{
	namespace
	{
		using Json = nlohmann::json;
		constexpr std::size_t kMaxOutputBytes = 1024 * 1024;
		constexpr std::size_t kMaxHeaderBytes = 8192;

		AccountAllowanceFetchResult Failure(const char* message, bool account_unavailable = false)
		{
			AccountAllowanceFetchResult result;
			result.error = message;
			result.account_unavailable = account_unavailable;
			return result;
		}

		const Json* Field(const Json& object, const char* name)
		{
			if (!object.is_object()) return nullptr;
			const auto found = object.find(name);
			return found == object.end() ? nullptr : &*found;
		}

		std::optional<double> Number(const Json* value, double maximum = 100)
		{
			if (value == nullptr || !value->is_number()) return std::nullopt;
			const double number = value->get<double>();
			if (!std::isfinite(number) || number < 0 || number > maximum) return std::nullopt;
			return number;
		}

		Json PositiveInteger(const Json* value, std::uint64_t maximum)
		{
			if (value == nullptr || !value->is_number_integer()) return nullptr;
			if (value->is_number_unsigned())
			{
				const auto number = value->get<std::uint64_t>();
				return number > 0 && number <= maximum ? Json(number) : Json(nullptr);
			}
			const auto number = value->get<std::int64_t>();
			return number > 0 && static_cast<std::uint64_t>(number) <= maximum ? Json(number) : Json(nullptr);
		}

		Json Text(const Json* value, std::size_t maximum = 256)
		{
			if (value == nullptr || !value->is_string() || value->get_ref<const std::string&>().size() > maximum) return nullptr;
			return *value;
		}

		Json DecimalAmount(const Json* value)
		{
			if (value == nullptr) return nullptr;
			if (value->is_string())
			{
				const std::string& text = value->get_ref<const std::string&>();
				if (text.empty() || text.size() > 128) return nullptr;
				const Json number = Json::parse(text, nullptr, false);
				if (number.is_discarded() || !Number(&number, 1e18)) return nullptr;
				return *value;
			}
			if (!Number(value, 1e18)) return nullptr;
			return value->dump();
		}

		bool True(const Json* value)
		{
			return value != nullptr && value->is_boolean() && value->get<bool>();
		}

		Json EmptyLimits(std::int64_t now_ms)
		{
			return {{"updatedAt", now_ms}, {"limitId", ""}, {"limitName", ""}, {"primary", nullptr}, {"secondary", nullptr},
			    {"credits", nullptr}, {"individualLimit", nullptr}, {"spendControlReached", nullptr}, {"planType", ""}, {"rateLimitReachedType", ""}};
		}

		Json CodexWindow(const Json* value)
		{
			if (value == nullptr) return nullptr;
			const auto percent = Number(Field(*value, "usedPercent"));
			if (!percent) return nullptr;
			return {{"usedPercent", *percent}, {"resetsAt", PositiveInteger(Field(*value, "resetsAt"), 253402300799ULL)},
			    {"windowDurationMinutes", PositiveInteger(Field(*value, "windowDurationMins"), 52560000)}};
		}

		Json CopilotWindow(const Json* value)
		{
			// hasQuota is not in every SDK schema; only an explicit false means the snapshot has no quota.
			const Json* has_quota = value == nullptr ? nullptr : Field(*value, "hasQuota");
			if (value == nullptr || !value->is_object() || (has_quota != nullptr && !True(has_quota)) || True(Field(*value, "isUnlimitedEntitlement"))) return nullptr;
			const Json* entitlement = Field(*value, "entitlementRequests");
			// Without hasQuota, only a stated finite entitlement shows the snapshot is a real limit.
			if (has_quota == nullptr && entitlement == nullptr) return nullptr;
			if (entitlement != nullptr)
			{
				const auto count = Number(entitlement, 1e18);
				if (!count || *count == 0) return nullptr;
			}
			const auto remaining = Number(Field(*value, "remainingPercentage"));
			if (!remaining) return nullptr;
			Json reset = nullptr;
			const Json date = Text(Field(*value, "resetDate"));
			if (date.is_string())
			{
				const auto parsed = TimestampMilliseconds(date.get_ref<const std::string&>());
				if (parsed && *parsed > 0) reset = *parsed / 1000;
			}
			return {{"usedPercent", 100.0 - *remaining}, {"resetsAt", reset}, {"windowDurationMinutes", nullptr}};
		}

		std::optional<Json> TakeMessage(std::string& pending, bool framed, bool& malformed)
		{
			std::size_t body_start = 0;
			std::size_t body_size = 0;
			std::size_t consumed = 0;
			if (framed)
			{
				const auto end = pending.find("\r\n\r\n");
				if (end == std::string::npos)
				{
					malformed = pending.size() > kMaxHeaderBytes;
					return std::nullopt;
				}
				if (end > kMaxHeaderBytes)
				{
					malformed = true;
					return std::nullopt;
				}
				bool found_length = false;
				std::size_t cursor = 0;
				while (cursor < end)
				{
					const auto newline = pending.find("\r\n", cursor);
					const auto line_end = std::min(newline, end);
					std::string_view line(pending.data() + cursor, line_end - cursor);
					const auto colon = line.find(':');
					if (colon == std::string_view::npos)
					{
						malformed = true;
						return std::nullopt;
					}
					std::string name(line.substr(0, colon));
					std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (name == "content-length")
					{
						std::string_view value = line.substr(colon + 1);
						while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
						while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
						const auto parsed = std::from_chars(value.data(), value.data() + value.size(), body_size);
						if (found_length || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || body_size == 0 || body_size > kMaxOutputBytes)
						{
							malformed = true;
							return std::nullopt;
						}
						found_length = true;
					}
					cursor = line_end + 2;
				}
				if (!found_length)
				{
					malformed = true;
					return std::nullopt;
				}
				body_start = end + 4;
				if (body_size > pending.size() - body_start) return std::nullopt;
				consumed = body_start + body_size;
			}
			else
			{
				const auto end = pending.find('\n');
				if (end == std::string::npos) return std::nullopt;
				body_size = end;
				consumed = end + 1;
			}
			Json message = Json::parse(pending.begin() + static_cast<std::ptrdiff_t>(body_start), pending.begin() + static_cast<std::ptrdiff_t>(body_start + body_size), nullptr, false);
			pending.erase(0, consumed);
			if (message.is_discarded() || !message.is_object())
			{
				malformed = true;
				return std::nullopt;
			}
			return message;
		}
	}

	AccountAllowanceCommand ValidateAccountAllowanceCommand(std::string_view provider_id, const std::vector<std::string>& command)
	{
		AccountAllowanceCommand result;
		result.error = "Independent account allowances are unavailable for this custom provider command; use the standard provider launcher without prompts, wrappers or account overrides.";
		std::string_view expected;
		if (provider_id == uam::provider_ids::kCodexCli) expected = "codex";
		else if (provider_id == uam::provider_ids::kClaudeCli) expected = "claude";
		else if (provider_id == uam::provider_ids::kCopilotCli) expected = "copilot";
		else if (provider_id == uam::provider_ids::kGeminiCli) expected = "gemini";
		else if (provider_id == uam::provider_ids::kAntigravityCli) expected = "agy";
		else return result;
		if (command.empty() || command.front().empty() || command.front().size() > 4096 || command.front().find('\0') != std::string::npos) return result;
		if (command.size() != 1 && !(provider_id == uam::provider_ids::kCodexCli && command.size() == 2 && command[1] == "--no-alt-screen")) return result;
		const std::string& executable = command.front();
		const auto separator = executable.find_last_of("/\\");
		if (separator != std::string::npos)
		{
			const bool absolute = executable.front() == '/'
			    || (executable.size() >= 3 && std::isalpha(static_cast<unsigned char>(executable[0])) && executable[1] == ':' && (executable[2] == '/' || executable[2] == '\\'))
			    || executable.starts_with("\\\\");
			if (!absolute) return result;
		}
		std::string name = executable.substr(separator == std::string::npos ? 0 : separator + 1);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (name.ends_with(".exe")) name.resize(name.size() - 4);
		if (name != expected) return result;
		result.command = {executable};
		result.error.clear();
		return result;
	}

	AccountAllowanceFetchResult NormalizeCodexAccountAllowance(const nlohmann::json& response, std::int64_t now_ms)
	{
		const Json* input = Field(response, "rateLimits");
		if (input == nullptr || !input->is_object()) return Failure("Codex did not return account allowance data.", true);
		AccountAllowanceFetchResult result;
		result.limits = EmptyLimits(now_ms);
		for (const char* key : {"limitId", "limitName", "planType", "rateLimitReachedType"})
		{
			const Json text = Text(Field(*input, key));
			if (!text.is_null()) result.limits[key] = text;
		}
		result.limits["primary"] = CodexWindow(Field(*input, "primary"));
		result.limits["secondary"] = CodexWindow(Field(*input, "secondary"));
		if (const Json* credits = Field(*input, "credits"))
		{
			const Json* has_credits = Field(*credits, "hasCredits");
			const Json* unlimited = Field(*credits, "unlimited");
			if (has_credits != nullptr && unlimited != nullptr && has_credits->is_boolean() && unlimited->is_boolean())
				result.limits["credits"] = {{"hasCredits", *has_credits}, {"unlimited", *unlimited}, {"balance", Text(Field(*credits, "balance"))}};
		}
		if (const Json* spend = Field(*input, "individualLimit"))
		{
			const Json limit = DecimalAmount(Field(*spend, "limit"));
			const Json used = DecimalAmount(Field(*spend, "used"));
			const auto remaining = Number(Field(*spend, "remainingPercent"));
			const Json reset = PositiveInteger(Field(*spend, "resetsAt"), 253402300799ULL);
			if (!limit.is_null() && !used.is_null() && remaining && !reset.is_null())
				result.limits["individualLimit"] = {{"limit", limit}, {"used", used}, {"remainingPercent", *remaining}, {"resetsAt", reset}};
		}
		if (const Json* reached = Field(*input, "spendControlReached"); reached != nullptr && reached->is_boolean())
			result.limits["spendControlReached"] = *reached;
		if (result.limits["primary"].is_null() && result.limits["secondary"].is_null() && result.limits["credits"].is_null() && result.limits["individualLimit"].is_null())
			return Failure("Codex did not return valid account allowance data.", true);
		return result;
	}

	AccountAllowanceFetchResult NormalizeCopilotAccountAllowance(const nlohmann::json& response, std::int64_t now_ms)
	{
		const Json* snapshots = Field(response, "quotaSnapshots");
		if (snapshots == nullptr || !snapshots->is_object()) return Failure("Copilot did not return account allowance data.", true);
		AccountAllowanceFetchResult result;
		result.limits = EmptyLimits(now_ms);
		// Snapshots are keyed by quota type. Chat and completions keep their place for plans that limit them; pooled
		// business plans leave both unlimited and put the per-user limit in premium_interactions (or another type).
		std::vector<std::string> order = {"chat", "completions", "premium_interactions"};
		for (const auto& [key, ignored] : snapshots->items())
			if (std::ranges::find(order, key) == order.end()) order.push_back(key);
		const auto label = [](const std::string& key)
		{
			if (key == "premium_interactions") return std::string("Premium requests allowance");
			if (key == "chat") return std::string("Chat allowance");
			if (key == "completions") return std::string("Completions allowance");
			if (key.empty() || key.size() > 40) return std::string("Copilot allowance");
			std::string text = key;
			std::ranges::replace(text, '_', ' ');
			if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
			return text + " allowance";
		};
		for (const std::string& key : order)
		{
			Json window = CopilotWindow(Field(*snapshots, key.c_str()));
			if (window.is_null()) continue;
			if (result.limits["primary"].is_null()) { result.limits["primary"] = std::move(window); result.primary_label = label(key); }
			else { result.limits["secondary"] = std::move(window); result.secondary_label = label(key); break; }
		}
		if (result.limits["primary"].is_null())
			return Failure("Copilot did not return a finite entitled account allowance.", true);
		return result;
	}
	AccountAllowanceFetchResult FetchCliAccountAllowance(std::string_view provider_id, const std::vector<std::string>& command,
	    const std::filesystem::path& working_directory, std::stop_token stop)
	{
		return FetchCliAccountAllowanceWithProcessService(PlatformServicesFactory::Instance().process_service,
		    provider_id, command, working_directory, stop);
	}

	AccountAllowanceFetchResult FetchCliAccountAllowanceWithProcessService(const IPlatformProcessService& service,
	    std::string_view provider_id, const std::vector<std::string>& command, const std::filesystem::path& working_directory,
	    std::stop_token stop, std::chrono::milliseconds timeout)
	{
		const bool codex = provider_id == uam::provider_ids::kCodexCli;
		const bool copilot = provider_id == uam::provider_ids::kCopilotCli;
		if (!codex && !copilot) return Failure("Independent account allowance acquisition is not available for this provider.");
		const auto validated = ValidateAccountAllowanceCommand(provider_id, command);
		if (!validated.error.empty()) return Failure(validated.error.c_str());
		if (stop.stop_requested()) return Failure("Account allowance refresh was cancelled.");
		if (timeout <= std::chrono::milliseconds::zero()) return Failure("Account allowance refresh timed out.");
		const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, std::chrono::milliseconds(10000));
		std::vector<std::string> argv = validated.command;
		if (codex) argv.insert(argv.end(), {"app-server", "--listen", "stdio://"});
		else argv.insert(argv.end(), {"--headless", "--stdio", "--no-auto-update"});
		platform::StdioProcessPlatformFields process;
		if (!service.StartStdioProcess(process, working_directory, argv)) return Failure("The provider account reader could not start.");
		struct Cleanup
		{
			const IPlatformProcessService& service;
			platform::StdioProcessPlatformFields& process;
			~Cleanup()
			{
				service.StopStdioProcess(process, true);
			}
		} cleanup{service, process};
		const auto send = [&](const Json& message)
		{
			const std::string payload = message.dump();
			const std::string bytes = copilot ? "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload : payload + "\n";
			return service.WriteToStdioProcess(process, bytes.data(), bytes.size());
		};
		const Json handshake = codex
		    ? Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"clientInfo", {{"name", "universal_agent_manager"}, {"version", "1.0.0"}}}}}}
		    : Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "connect"}, {"params", {{"clientInfo", {{"editorName", "Universal Agent Manager"}}}}}};
		if (!send(handshake)) return Failure("The provider account reader could not receive initialization.");
		std::array<char, 8192> buffer{};
		std::string pending;
		std::size_t output_bytes = 0;
		bool initialized = false;
		for (;;)
		{
			if (stop.stop_requested()) return Failure("Account allowance refresh was cancelled.");
			if (std::chrono::steady_clock::now() >= deadline) return Failure("Account allowance refresh timed out.");
			// A response written just before exit is still read.
			const bool exit_observed = service.PollStdioProcessExited(process);
			bool progress = false;
			for (bool stderr_stream : {false, true})
			{
				const auto count = stderr_stream ? service.ReadStdioProcessStderr(process, buffer.data(), buffer.size())
				                                 : service.ReadStdioProcessStdout(process, buffer.data(), buffer.size());
				if (count == -1) return Failure("The provider account reader connection failed.");
				if (count <= 0) continue;
				progress = true;
				const auto size = static_cast<std::size_t>(count);
				if (size > kMaxOutputBytes - output_bytes) return Failure("The provider account reader exceeded its output limit.");
				output_bytes += size;
				if (!stderr_stream) pending.append(buffer.data(), size);
			}
			for (;;)
			{
				bool malformed = false;
				const auto message = TakeMessage(pending, copilot, malformed);
				if (malformed) return Failure("The provider account reader returned an invalid response.");
				if (!message) break;
				if (message->contains("method"))
				{
					if (message->contains("id")) return Failure("The provider requested interaction; account allowance refresh was stopped.");
					continue;
				}
				const Json* id = Field(*message, "id");
				if (id == nullptr || *id != (initialized ? 2 : 1)) continue;
				if (message->contains("error")) return Failure("The provider could not return account allowances; check its existing sign-in and CLI compatibility.", true);
				const Json* result = Field(*message, "result");
				if (result == nullptr || !result->is_object()) return Failure("The provider account reader returned an invalid response.");
				if (initialized)
					return codex ? NormalizeCodexAccountAllowance(*result, time::SystemEpochMillisecondsNow())
					             : NormalizeCopilotAccountAllowance(*result, time::SystemEpochMillisecondsNow());
				if (copilot && Field(*result, "protocolVersion") != nullptr && *Field(*result, "protocolVersion") != 3)
					return Failure("The Copilot account reader protocol is incompatible.");
				if (codex && !send({{"jsonrpc", "2.0"}, {"method", "initialized"}}))
					return Failure("The provider account reader could not receive initialization.");
				initialized = true;
				if (!send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", codex ? "account/rateLimits/read" : "account.getQuota"}, {"params", Json::object()}}))
					return Failure("The provider account reader could not receive the allowance request.");
			}
			if (!progress)
			{
				if (exit_observed) return Failure("The provider account reader exited before returning allowances.");
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
	}
}
