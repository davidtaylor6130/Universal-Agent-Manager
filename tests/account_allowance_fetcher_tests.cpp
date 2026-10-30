#include "test_harness.h"
#include "common/usage/account_allowance_fetcher.h"
#include "common/platform/platform_services.h"

#include <cstring>

using namespace uam_test;

namespace
{
	class AccountAllowanceProcessFixture final : public IPlatformProcessService
	{
	  public:
		mutable std::vector<std::string> started_argv;
		mutable std::vector<nlohmann::json> requests;
		mutable std::string pending;
		mutable std::size_t offset = 0;
		mutable int starts = 0;
		mutable int stops = 0;
		bool copilot = false;
		bool silent = false;
		bool output_flood = false;
		bool rpc_error = false;
		bool interaction = false;
		bool fragmented = false;

		bool SupportsDetachedProcesses() const override { return false; }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
		std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override { return {}; }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { return false; }
		int NormalizeCapturedCommandExitCode(int status) const override { return status; }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { return {}; }
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>& argv,
		    std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			++starts; started_argv = argv;
			return true;
		}
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&,
		    std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { return false; }
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override {}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char* bytes, std::size_t len, std::string*) const override
		{
			std::string text(bytes, len);
			if (copilot) text = text.substr(text.find("\r\n\r\n") + 4);
			const auto request = nlohmann::json::parse(text);
			requests.push_back(request);
			if (silent || !request.contains("id")) return true;
			nlohmann::json response{{"jsonrpc", "2.0"}, {"id", request["id"]}};
			if (request.value("method", "") == "initialize" || request.value("method", "") == "connect")
				response["result"] = {{"protocolVersion", 3}};
			else if (interaction)
				response = {{"jsonrpc", "2.0"}, {"id", "permission"}, {"method", "userInput.request"}, {"params", {{"secret", "do-not-leak"}}}};
			else if (rpc_error)
				response["error"] = {{"code", -32601}, {"message", "secret-auth-token-do-not-leak"}};
			else if (copilot)
				response["result"] = {{"quotaSnapshots", {{"chat", {{"remainingPercentage", 75}, {"hasQuota", true}, {"entitlementRequests", 200}}}}}};
			else
				response["result"] = {{"rateLimits", {{"primary", {{"usedPercent", 25}}}}}};
			const std::string payload = response.dump();
			pending += copilot ? "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload : payload + "\n";
			return true;
		}
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override {}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++stops; }
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++stops; }
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char* bytes, std::size_t count, std::string*) const override
		{
			if (output_flood) { std::memset(bytes, 'x', count); return static_cast<std::ptrdiff_t>(count); }
			if (offset == pending.size()) return -2;
			const std::size_t size = std::min({count, pending.size() - offset, fragmented ? std::size_t(3) : count});
			std::memcpy(bytes, pending.data() + offset, size); offset += size;
			return static_cast<std::ptrdiff_t>(size);
		}
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { return -2; }
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int*) const override { return false; }
		fs::path ResolveCurrentExecutablePath() const override { return {}; }
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const fs::path&, std::string*) const override { return {}; }
		uintmax_t NativeGeminiSessionMaxFileBytes() const override { return 0; }
		std::size_t NativeGeminiSessionMaxMessages() const override { return 0; }
		std::string GenerateUuid() const override { return {}; }
		bool LaunchShellAt(const fs::path&, std::string*) const override { return false; }
		void SetKeepSystemAwake(bool) const override {}
		bool EmbeddedBrowserUsesMockKeychain() const override { return false; }
	};
}

UAM_TEST(AccountAllowanceCodexNormalizesReadOnlyResponseWithoutInventingWindows)
{
	const auto result = uam::usage::NormalizeCodexAccountAllowance({{"rateLimits", {
	    {"limitId", "codex"}, {"limitName", "Codex"}, {"planType", "plus"},
	    {"primary", {{"usedPercent", 91}, {"windowDurationMins", 10080}, {"resetsAt", 1792653053}}},
	    {"secondary", nullptr}, {"credits", {{"hasCredits", false}, {"unlimited", false}, {"balance", nullptr}}}
	}}}, 123456);
	UAM_ASSERT(result.error.empty());
	UAM_ASSERT_EQ(result.limits.at("updatedAt"), 123456);
	UAM_ASSERT_EQ(result.limits.at("primary").at("usedPercent"), 91);
	UAM_ASSERT_EQ(result.limits.at("primary").at("windowDurationMinutes"), 10080);
	UAM_ASSERT_EQ(result.limits.at("primary").at("resetsAt"), 1792653053);
	UAM_ASSERT(result.limits.at("secondary").is_null());
}

UAM_TEST(AccountAllowanceCodexRejectsMalformedPercentagesWithoutFabricatingZero)
{
	for (const nlohmann::json bad : {nlohmann::json(-1), nlohmann::json(101), nlohmann::json("unknown"), nlohmann::json(nullptr)})
	{
		const auto result = uam::usage::NormalizeCodexAccountAllowance({{"rateLimits", {{"primary", {{"usedPercent", bad}}}}}}, 123);
		UAM_ASSERT(result.limits.is_null());
		UAM_ASSERT(!result.error.empty());
	}
	const auto valid = uam::usage::NormalizeCodexAccountAllowance({{"rateLimits", {{"primary", {{"usedPercent", 0}, {"windowDurationMins", -1}, {"resetsAt", 1.5}}}}}}, 123);
	UAM_ASSERT_EQ(valid.limits.at("primary").at("usedPercent"), 0);
	UAM_ASSERT(valid.limits.at("primary").at("windowDurationMinutes").is_null());
	UAM_ASSERT(valid.limits.at("primary").at("resetsAt").is_null());
}

UAM_TEST(AccountAllowanceCodexSpendLimitsKeepWireStringsAndRejectInvalidAmounts)
{
	for (const nlohmann::json amount : {nlohmann::json("30.00"), nlohmann::json(30), nlohmann::json(30.25)})
	{
		const auto result = uam::usage::NormalizeCodexAccountAllowance({{"rateLimits", {
		    {"individualLimit", {{"limit", amount}, {"used", "10.50"}, {"remainingPercent", 65}, {"resetsAt", 1793491200}}}
		}}}, 123);
		UAM_ASSERT(result.error.empty());
		const auto& spend = result.limits.at("individualLimit");
		UAM_ASSERT(spend.at("limit").is_string());
		UAM_ASSERT(spend.at("used").is_string());
		UAM_ASSERT_EQ(spend.at("used"), "10.50");
		UAM_ASSERT_EQ(spend.at("remainingPercent"), 65);
		UAM_ASSERT_EQ(spend.at("resetsAt"), 1793491200);
	}
	const auto numeric = uam::usage::NormalizeCodexAccountAllowance({{"rateLimits", {
	    {"individualLimit", {{"limit", 30}, {"used", 10.5}, {"remainingPercent", 65}, {"resetsAt", 1793491200}}}
	}}}, 123);
	UAM_ASSERT_EQ(numeric.limits.at("individualLimit").at("limit"), "30");
	UAM_ASSERT_EQ(numeric.limits.at("individualLimit").at("used"), "10.5");
	for (const nlohmann::json bad : {nlohmann::json("-1"), nlohmann::json("NaN"), nlohmann::json("1e400"), nlohmann::json("invalid"), nlohmann::json(-1), nlohmann::json(1e30)})
	{
		const auto result = uam::usage::NormalizeCodexAccountAllowance({{"rateLimits", {
		    {"primary", {{"usedPercent", 25}}}, {"individualLimit", {{"limit", bad}, {"used", 0}, {"remainingPercent", 65}, {"resetsAt", 1793491200}}}
		}}}, 123);
		UAM_ASSERT(result.error.empty());
		UAM_ASSERT(result.limits.at("individualLimit").is_null());
	}
}

UAM_TEST(AccountAllowanceCopilotUsesEntitledBucketsAndReportedResetWithoutInventingPeriods)
{
	const auto result = uam::usage::NormalizeCopilotAccountAllowance({{"quotaSnapshots", {
	    {"chat", {{"remainingPercentage", 99.6}, {"hasQuota", true}, {"entitlementRequests", 200}, {"resetDate", "2026-11-01T00:00:00Z"}}},
	    {"completions", {{"remainingPercentage", 100}, {"hasQuota", true}, {"entitlementRequests", 2000}}},
	    {"premium_interactions", {{"remainingPercentage", 0}, {"hasQuota", false}, {"entitlementRequests", 0}}}
	}}}, 123);
	UAM_ASSERT(result.error.empty());
	UAM_ASSERT_EQ(result.primary_label, "Chat allowance");
	UAM_ASSERT_EQ(result.secondary_label, "Completions allowance");
	UAM_ASSERT(std::abs(result.limits.at("primary").at("usedPercent").get<double>() - 0.4) < 0.000001);
	UAM_ASSERT_EQ(result.limits.at("primary").at("resetsAt"), 1793491200);
	UAM_ASSERT(result.limits.at("primary").at("windowDurationMinutes").is_null());
	UAM_ASSERT_EQ(result.limits.at("secondary").at("usedPercent"), 0);
}

UAM_TEST(AccountAllowanceCopilotReadsPooledPlanPerUserLimitFromAnyQuotaType)
{
	// Pooled business plans: chat and completions are unlimited and the per-user limit is premium_interactions.
	// The current SDK schema has no hasQuota field.
	const auto pooled = uam::usage::NormalizeCopilotAccountAllowance({{"quotaSnapshots", {
	    {"chat", {{"isUnlimitedEntitlement", true}, {"entitlementRequests", -1}, {"remainingPercentage", 100}}},
	    {"completions", {{"isUnlimitedEntitlement", true}, {"entitlementRequests", -1}, {"remainingPercentage", 100}}},
	    {"premium_interactions", {{"isUnlimitedEntitlement", false}, {"entitlementRequests", 300}, {"remainingPercentage", 75}, {"resetDate", "2026-11-01T00:00:00Z"}}}
	}}}, 123);
	UAM_ASSERT(pooled.error.empty());
	UAM_ASSERT_EQ(pooled.primary_label, "Premium requests allowance");
	UAM_ASSERT_EQ(pooled.limits.at("primary").at("usedPercent"), 25);
	UAM_ASSERT_EQ(pooled.limits.at("primary").at("resetsAt"), 1793491200);
	UAM_ASSERT(pooled.limits.at("secondary").is_null());
	// A quota type UAM does not know yet is still shown, with a readable label.
	const auto other = uam::usage::NormalizeCopilotAccountAllowance({{"quotaSnapshots", {
	    {"chat", {{"isUnlimitedEntitlement", true}, {"entitlementRequests", -1}, {"remainingPercentage", 100}}},
	    {"token_pool", {{"isUnlimitedEntitlement", false}, {"entitlementRequests", 1000000}, {"remainingPercentage", 40}}}
	}}}, 123);
	UAM_ASSERT(other.error.empty());
	UAM_ASSERT_EQ(other.primary_label, "Token pool allowance");
	UAM_ASSERT_EQ(other.limits.at("primary").at("usedPercent"), 60);
}

UAM_TEST(AccountAllowanceCopilotDoesNotTurnUnavailableOrUnlimitedEntitlementsIntoExhaustion)
{
	for (const auto bucket : {
	    nlohmann::json{{"remainingPercentage", 0}, {"hasQuota", false}, {"entitlementRequests", 0}},
	    nlohmann::json{{"remainingPercentage", 0}, {"hasQuota", true}, {"isUnlimitedEntitlement", true}},
	    nlohmann::json{{"remainingPercentage", -1}, {"hasQuota", true}},
	    nlohmann::json{{"remainingPercentage", "100"}, {"hasQuota", true}}})
	{
		const auto result = uam::usage::NormalizeCopilotAccountAllowance({{"quotaSnapshots", {{"chat", bucket}}}}, 123);
		UAM_ASSERT(result.limits.is_null());
		UAM_ASSERT(!result.error.empty());
	}
}

UAM_TEST(AccountAllowanceCodexTransportUsesOnlyInitializationAndAccountReadAndReapsChild)
{
	AccountAllowanceProcessFixture fixture;
	const auto result = uam::usage::FetchCliAccountAllowanceWithProcessService(fixture, "codex-cli", {"/Applications/Provider Tools/codex"}, fs::path{});
	UAM_ASSERT(result.error.empty());
	UAM_ASSERT_EQ(result.limits.at("primary").at("usedPercent"), 25);
	UAM_ASSERT_EQ(fixture.starts, 1); UAM_ASSERT_EQ(fixture.stops, 1);
	UAM_ASSERT_EQ(fixture.started_argv, (std::vector<std::string>{"/Applications/Provider Tools/codex", "app-server", "--listen", "stdio://"}));
	UAM_ASSERT_EQ(fixture.requests.size(), std::size_t(3));
	UAM_ASSERT_EQ(fixture.requests[0].at("method"), "initialize");
	UAM_ASSERT_EQ(fixture.requests[1].at("method"), "initialized");
	UAM_ASSERT_EQ(fixture.requests[2].at("method"), "account/rateLimits/read");
}

UAM_TEST(AccountAllowanceCopilotTransportHandlesFragmentedFramesWithoutCreatingSession)
{
	AccountAllowanceProcessFixture fixture; fixture.copilot = true; fixture.fragmented = true;
	const auto result = uam::usage::FetchCliAccountAllowanceWithProcessService(fixture, "copilot-cli", {"copilot"}, fs::path{});
	UAM_ASSERT(result.error.empty());
	UAM_ASSERT_EQ(result.limits.at("primary").at("usedPercent"), 25);
	UAM_ASSERT_EQ(fixture.stops, 1);
	UAM_ASSERT_EQ(fixture.started_argv, (std::vector<std::string>{"copilot", "--headless", "--stdio", "--no-auto-update"}));
	UAM_ASSERT_EQ(fixture.requests.size(), std::size_t(2));
	UAM_ASSERT_EQ(fixture.requests[0].at("method"), "connect");
	UAM_ASSERT_EQ(fixture.requests[1].at("method"), "account.getQuota");
}

UAM_TEST(AccountAllowanceTransportCancellationAndDeadlineAreBoundedAndReapOwnedChild)
{
	AccountAllowanceProcessFixture fixture; fixture.silent = true;
	std::stop_source cancellation; cancellation.request_stop();
	const auto cancelled = uam::usage::FetchCliAccountAllowanceWithProcessService(fixture, "codex-cli", {"codex"}, {}, cancellation.get_token());
	UAM_ASSERT(!cancelled.error.empty()); UAM_ASSERT_EQ(fixture.starts, 0);
	const auto deadline = uam::usage::FetchCliAccountAllowanceWithProcessService(fixture, "codex-cli", {"codex"}, {}, {}, std::chrono::milliseconds(10));
	UAM_ASSERT(!deadline.error.empty()); UAM_ASSERT(deadline.limits.is_null());
	UAM_ASSERT_EQ(fixture.starts, 1); UAM_ASSERT_EQ(fixture.stops, 1);
}

UAM_TEST(AccountAllowanceTransportCapsOutputAndRedactsProviderErrorsAndInteractionRequests)
{
	for (int mode = 0; mode < 3; ++mode)
	{
		AccountAllowanceProcessFixture fixture;
		fixture.output_flood = mode == 0; fixture.rpc_error = mode == 1; fixture.interaction = mode == 2;
		const auto result = uam::usage::FetchCliAccountAllowanceWithProcessService(fixture, "codex-cli", {"codex"}, {});
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty());
		UAM_ASSERT(result.error.find("secret") == std::string::npos);
		UAM_ASSERT_EQ(fixture.stops, 1);
		UAM_ASSERT(fixture.requests.size() <= 3);
	}
	AccountAllowanceProcessFixture unsupported;
	UAM_ASSERT(!uam::usage::FetchCliAccountAllowanceWithProcessService(unsupported, "unknown", {"provider"}, {}).error.empty());
	UAM_ASSERT_EQ(unsupported.starts, 0);
}

UAM_TEST(AccountAllowanceCommandValidationPreservesKnownLaunchersAndStripsCodexPresentationFlag)
{
	UAM_ASSERT(uam::usage::ValidateAccountAllowanceCommand("gemini-cli", {"gemini"}).error.empty());
	UAM_ASSERT(uam::usage::ValidateAccountAllowanceCommand("antigravity-cli", {"agy"}).error.empty());
	for (const std::string provider : {"codex", "claude", "copilot"})
	{
		for (const std::string executable : {provider, "/Applications/Provider Tools/" + provider, "C:\\Program Files\\Provider\\" + provider + ".exe"})
		{
			const auto result = uam::usage::ValidateAccountAllowanceCommand(provider + "-cli", {executable});
			UAM_ASSERT(result.error.empty());
			UAM_ASSERT_EQ(result.command, (std::vector<std::string>{executable}));
		}
	}
	const auto codex_default = uam::usage::ValidateAccountAllowanceCommand("codex-cli", {"codex", "--no-alt-screen"});
	UAM_ASSERT(codex_default.error.empty());
	UAM_ASSERT_EQ(codex_default.command, (std::vector<std::string>{"codex"}));
}

UAM_TEST(AccountAllowanceCommandValidationRejectsPromptsSubcommandsWrappersAndAccountOverrides)
{
	for (const auto& command : std::vector<std::vector<std::string>>{
	    {}, {""}, {"codex", "hello"}, {"codex", "exec", "hello"}, {"codex", "--", "hello"},
	    {"codex", "--profile", "another-account"}, {"codex", "--config", "model_provider=other"},
	    {"codex", "--no-alt-screen", "hello"}, {"codex", "--no-alt-screen", "--no-alt-screen"},
	    {"sh", "-c", "codex"}, {"env", "CODEX_HOME=/tmp/other", "codex"}, {"custom-codex"}, {"./codex"},
	    {"C:codex.exe"}, {"/Applications/codex-wrapper"}})
	{
		const auto result = uam::usage::ValidateAccountAllowanceCommand("codex-cli", command);
		UAM_ASSERT(!result.error.empty()); UAM_ASSERT(result.command.empty());
	}
	for (const auto& provider : {"claude", "copilot"})
	{
		for (const auto& command : std::vector<std::vector<std::string>>{{provider, "--config-dir", "/tmp/other"}, {provider, "--no-alt-screen"}, {"custom-" + std::string(provider)}})
		{
			const auto result = uam::usage::ValidateAccountAllowanceCommand(std::string(provider) + "-cli", command);
			UAM_ASSERT(!result.error.empty()); UAM_ASSERT(result.command.empty());
		}
	}
	UAM_ASSERT(!uam::usage::ValidateAccountAllowanceCommand("unknown", {"codex"}).error.empty());
}

UAM_TEST(AccountAllowanceTransportRejectsUnsafeCommandsBeforeLaunching)
{
	for (const std::string provider : {"codex", "copilot"})
	{
		for (const auto& command : std::vector<std::vector<std::string>>{{provider, "--", "prompt"}, {"custom-" + provider}, {provider, "--profile", "other"}})
		{
			AccountAllowanceProcessFixture fixture;
			const auto result = uam::usage::FetchCliAccountAllowanceWithProcessService(fixture, provider + "-cli", command, {});
			UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty());
			UAM_ASSERT_EQ(fixture.starts, 0); UAM_ASSERT_EQ(fixture.stops, 0);
		}
	}
}
