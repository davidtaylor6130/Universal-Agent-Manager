#include "common/usage/claude_account_allowance.h"
#include "common/usage/account_credentials_macos.h"
#include "test_harness.h"
#include "common/platform/platform_services.h"

#include <cstring>
#include <limits>

using namespace uam_test;

namespace
{
	class ClaudeAccountHttpsFixture final : public IPlatformProcessService
	{
	  public:
		mutable std::vector<std::string> arguments;
		mutable std::string input;
		mutable std::size_t offset = 0;
		mutable int starts = 0, stops = 0, preloaded_starts = 0, input_closes = 0;
		std::string response = "{\"five_hour\":{\"utilization\":57}}\n200";
		bool silent = false, flood = false;
		std::size_t chunk_size = 13;
		std::stop_source* cancel_on_read = nullptr;

		bool SupportsDetachedProcesses() const override { return false; }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
		std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override { return {}; }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { return false; }
		int NormalizeCapturedCommandExitCode(int code) const override { return code; }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { return {}; }
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>& argv,
		    std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { ++starts; arguments = argv; return true; }
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&,
		    std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { ++preloaded_starts; return false; }
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override {}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char* bytes, std::size_t length, std::string*) const override { input.append(bytes, length); return true; }
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override { ++input_closes; }
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++stops; }
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++stops; }
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char* bytes, std::size_t count, std::string*) const override
		{
			if (cancel_on_read) cancel_on_read->request_stop();
			if (silent) return -2;
			if (flood) { std::memset(bytes, 'x', count); return static_cast<std::ptrdiff_t>(count); }
			if (offset == response.size()) return 0;
			const std::size_t length = std::min({count, response.size() - offset, chunk_size});
			std::memcpy(bytes, response.data() + offset, length); offset += length; return static_cast<std::ptrdiff_t>(length);
		}
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { return -2; }
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int* code) const override { if (silent || flood) return false; if (code) *code = 0; return offset == response.size(); }
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

UAM_TEST(ClaudeAccountAllowanceReadsPercentageUnitsWithoutChatEvents)
{
	const auto result = uam::usage::NormalizeClaudeAccountAllowance({{"five_hour", {{"utilization", 29.6}, {"resets_at", "2026-10-16T12:00:00Z"}}},
	    {"seven_day", {{"utilization", 0.57}, {"resets_at", "2026-10-20T12:00:00Z"}}}}, 1792100000000);
	UAM_ASSERT(result.error.empty()); UAM_ASSERT(result.limits.is_object());
	UAM_ASSERT_EQ(result.limits.at("updatedAt"), 1792100000000LL);
	UAM_ASSERT_EQ(result.limits.at("primary").at("usedPercent"), 29.6);
	UAM_ASSERT_EQ(result.limits.at("secondary").at("usedPercent"), 0.57);
	UAM_ASSERT_EQ(result.limits.at("primary").at("windowDurationMinutes"), 300);
	UAM_ASSERT_EQ(result.limits.at("secondary").at("windowDurationMinutes"), 10080);
}

UAM_TEST(ClaudeAccountAllowanceRejectsMalformedOrMissingUtilization)
{
	for (const auto& utilization : std::vector<nlohmann::json>{nullptr, "57", true, -0.1, 100.1,
	         std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
	{
		const auto result = uam::usage::NormalizeClaudeAccountAllowance({{"five_hour", {{"utilization", utilization}}}}, 1000);
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty());
	}
	for (const auto& response : std::vector<nlohmann::json>{nullptr, nlohmann::json::array(), nlohmann::json::object(),
	         {{"five_hour", nullptr}}, {{"five_hour", {{"resets_at", "2026-10-16T12:00:00Z"}}}}})
	{
		const auto result = uam::usage::NormalizeClaudeAccountAllowance(response, 1000);
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty());
	}
}

UAM_TEST(ClaudeAccountAllowanceKeepsIndependentWindowsAndGenuineZero)
{
	const auto result = uam::usage::NormalizeClaudeAccountAllowance({{"five_hour", {{"utilization", "invalid"}}},
	    {"seven_day", {{"utilization", 0}, {"resets_at", nullptr}}}, {"seven_day_sonnet", {{"utilization", 100}}}}, 1000);
	UAM_ASSERT(result.error.empty()); UAM_ASSERT(result.limits.is_object());
	UAM_ASSERT(result.limits.at("primary").is_null());
	UAM_ASSERT_EQ(result.limits.at("secondary").at("usedPercent"), 0);
	UAM_ASSERT(result.limits.at("secondary").at("resetsAt").is_null());
	const auto only_model = uam::usage::NormalizeClaudeAccountAllowance({{"seven_day_sonnet", {{"utilization", 57}}}}, 1000);
	UAM_ASSERT(only_model.limits.is_null()); UAM_ASSERT(!only_model.error.empty());
}

UAM_TEST(ClaudeAccountAllowanceParsesTimezoneQualifiedResetsWithoutLocalTime)
{
	const auto result = uam::usage::NormalizeClaudeAccountAllowance({{"five_hour", {{"utilization", 100}, {"resets_at", "1970-01-02T01:00:00.123456+01:00"}}},
	    {"seven_day", {{"utilization", 25}, {"resets_at", "1970-01-02T00:00:00Z"}}}}, 1000);
	UAM_ASSERT(result.limits.is_object());
	UAM_ASSERT_EQ(result.limits.at("primary").at("resetsAt"), 86400);
	UAM_ASSERT_EQ(result.limits.at("secondary").at("resetsAt"), 86400);
	const auto invalid = uam::usage::NormalizeClaudeAccountAllowance({{"five_hour", {{"utilization", 57}, {"resets_at", "2026-02-30T12:00:00Z"}}}}, 1000);
	UAM_ASSERT(invalid.limits.is_object()); UAM_ASSERT(invalid.limits.at("primary").at("resetsAt").is_null());
}

UAM_TEST(ClaudeAccountAllowanceNeverReturnsRawProviderErrorOrInvalidClock)
{
	const auto error = uam::usage::NormalizeClaudeAccountAllowance({{"error", {{"message", "secret-token-must-not-leak"}}}}, 1000);
	UAM_ASSERT(error.limits.is_null()); UAM_ASSERT(!error.error.empty()); UAM_ASSERT(error.error.find("secret-token") == std::string::npos);
	for (std::int64_t invalid : {0LL, -1LL, std::numeric_limits<long long>::max()})
	{
		const auto result = uam::usage::NormalizeClaudeAccountAllowance({{"five_hour", {{"utilization", 57}}}}, invalid);
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty());
	}
}

UAM_TEST(ClaudeAccountAllowanceTransportKeepsCredentialInPipeAndUsesVerifiedFixedHttps)
{
	ClaudeAccountHttpsFixture fixture;
	const auto result = uam::usage::FetchClaudeAccountAllowanceWithProcessService(fixture, {}, "synthetic-private-token");
	UAM_ASSERT(result.error.empty()); UAM_ASSERT_EQ(result.limits.at("primary").at("usedPercent"), 57);
	UAM_ASSERT_EQ(fixture.starts, 1); UAM_ASSERT_EQ(fixture.stops, 1); UAM_ASSERT_EQ(fixture.preloaded_starts, 0); UAM_ASSERT_EQ(fixture.input_closes, 1);
	UAM_ASSERT_EQ(fixture.input, "header = \"Authorization: Bearer synthetic-private-token\"\n");
	UAM_ASSERT_EQ(fixture.arguments.at(1), "-q"); UAM_ASSERT_EQ(fixture.arguments.back(), "https://api.anthropic.com/api/oauth/usage");
	for (const auto& argument : fixture.arguments)
	{
		UAM_ASSERT(argument.find("synthetic-private-token") == std::string::npos);
		UAM_ASSERT(argument != "--insecure"); UAM_ASSERT(argument != "--location"); UAM_ASSERT(argument != "-L");
	}
}

UAM_TEST(ClaudeAccountAllowanceTransportRejectsCredentialControlCharactersBeforeSpawning)
{
	for (const auto& token : std::vector<std::string>{"", "synthetic\nheader=\"injected\"", std::string("token\0hidden", 12), std::string(16385, 'x')})
	{
		ClaudeAccountHttpsFixture fixture;
		const auto result = uam::usage::FetchClaudeAccountAllowanceWithProcessService(fixture, {}, token);
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty()); UAM_ASSERT(result.account_unavailable); UAM_ASSERT_EQ(fixture.starts, 0);
	}
}

UAM_TEST(ClaudeAccountAllowanceTransportReapsOnCancellationDeadlineAndOversizedOutput)
{
	std::stop_source already_canceled; already_canceled.request_stop(); ClaudeAccountHttpsFixture initial;
	UAM_ASSERT(!uam::usage::FetchClaudeAccountAllowanceWithProcessService(initial, {}, "synthetic", already_canceled.get_token()).error.empty()); UAM_ASSERT_EQ(initial.starts, 0);
	for (int mode = 0; mode < 3; ++mode)
	{
		ClaudeAccountHttpsFixture fixture; std::stop_source cancel;
		fixture.silent = mode < 2; fixture.flood = mode == 2; if (mode == 0) fixture.cancel_on_read = &cancel;
		const auto result = uam::usage::FetchClaudeAccountAllowanceWithProcessService(fixture, {}, "synthetic", cancel.get_token(), std::chrono::milliseconds(mode == 1 ? 10 : 500));
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty()); UAM_ASSERT_EQ(fixture.starts, 1); UAM_ASSERT_EQ(fixture.stops, 1);
		UAM_ASSERT(!result.account_unavailable);
	}
}

UAM_TEST(ClaudeAccountAllowanceTransportSanitizesHttpErrorsWithoutLeakingProviderBody)
{
	for (const std::string code : {"401", "403", "429", "500", "302"})
	{
		ClaudeAccountHttpsFixture fixture; fixture.response = "{\"error\":\"synthetic-secret-do-not-leak\"}\n" + code;
		const auto result = uam::usage::FetchClaudeAccountAllowanceWithProcessService(fixture, {}, "synthetic");
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(!result.error.empty()); UAM_ASSERT(result.error.find("synthetic-secret") == std::string::npos); UAM_ASSERT_EQ(fixture.stops, 1);
		UAM_ASSERT_EQ(result.account_unavailable, code == "401" || code == "403");
	}
}

UAM_TEST(MacAccountCredentialHelperUsesNoPromptSecurityQueryAndKeepsReturnedSecretPrivate)
{
	ClaudeAccountHttpsFixture fixture; fixture.response = nlohmann::json{{"status", 0}, {"credential", "synthetic-private-login"}}.dump();
	const auto result = uam::usage::ReadMacAccountCredentialWithProcessService(fixture, "service with spaces; literal", "existing-account");
	UAM_ASSERT(result.error.empty()); UAM_ASSERT_EQ(result.credential, "synthetic-private-login"); UAM_ASSERT(!result.account_unavailable);
	UAM_ASSERT_EQ(fixture.arguments.at(0), "/usr/bin/osascript"); UAM_ASSERT_EQ(fixture.arguments.at(1), "-l"); UAM_ASSERT_EQ(fixture.arguments.at(2), "JavaScript");
	UAM_ASSERT_EQ(fixture.arguments.at(5), "service with spaces; literal"); UAM_ASSERT_EQ(fixture.arguments.at(6), "existing-account");
	UAM_ASSERT(fixture.arguments.at(4).find("SecItemCopyMatching") != std::string::npos);
	UAM_ASSERT(fixture.arguments.at(4).find("kSecUseAuthenticationUIFail") != std::string::npos);
	for (const auto& argument : fixture.arguments) UAM_ASSERT(argument.find("synthetic-private-login") == std::string::npos);
	UAM_ASSERT(fixture.input.empty()); UAM_ASSERT_EQ(fixture.preloaded_starts, 0); UAM_ASSERT_EQ(fixture.input_closes, 1); UAM_ASSERT_EQ(fixture.stops, 1);
}

UAM_TEST(MacAccountCredentialHelperCancellationTimeoutAndOversizeKeepPriorAccountEligible)
{
	std::stop_source stopped; stopped.request_stop(); ClaudeAccountHttpsFixture initial;
	UAM_ASSERT(!uam::usage::ReadMacAccountCredentialWithProcessService(initial, "service", "account", stopped.get_token()).error.empty()); UAM_ASSERT_EQ(initial.starts, 0);
	for (int mode = 0; mode < 3; ++mode)
	{
		ClaudeAccountHttpsFixture fixture; std::stop_source cancel;
		fixture.silent = mode != 2; fixture.flood = mode == 2; if (mode == 0) fixture.cancel_on_read = &cancel;
		const auto result = uam::usage::ReadMacAccountCredentialWithProcessService(fixture, "service", "account", cancel.get_token(), std::chrono::milliseconds(mode == 1 ? 10 : 1500));
		UAM_ASSERT(result.credential.empty()); UAM_ASSERT(!result.error.empty()); UAM_ASSERT(!result.account_unavailable);
		UAM_ASSERT_EQ(fixture.starts, 1); UAM_ASSERT_EQ(fixture.stops, 1);
	}
}

UAM_TEST(MacAccountCredentialHelperDistinguishesMissingLoginFromLockedOrMalformedStorage)
{
	for (int status : {-25300, -25308, -25293, -50})
	{
		ClaudeAccountHttpsFixture fixture; fixture.response = nlohmann::json{{"status", status}, {"secret", "synthetic-do-not-leak"}}.dump();
		const auto result = uam::usage::ReadMacAccountCredentialWithProcessService(fixture, "service", "account");
		UAM_ASSERT(result.credential.empty()); UAM_ASSERT(!result.error.empty()); UAM_ASSERT_EQ(result.account_unavailable, status == -25300);
		UAM_ASSERT(result.error.find("synthetic-do-not-leak") == std::string::npos); UAM_ASSERT_EQ(fixture.stops, 1);
	}
	for (const auto& response : std::vector<std::string>{"synthetic-do-not-leak", nlohmann::json{{"status", 0}, {"credential", std::string(65537, 'x')}}.dump()})
	{
		ClaudeAccountHttpsFixture fixture; fixture.response = response; fixture.chunk_size = 8192;
		const auto result = uam::usage::ReadMacAccountCredentialWithProcessService(fixture, "service", "account");
		UAM_ASSERT(result.credential.empty()); UAM_ASSERT(!result.error.empty()); UAM_ASSERT(!result.account_unavailable);
		UAM_ASSERT(result.error.find("synthetic-do-not-leak") == std::string::npos);
	}
}

UAM_TEST(MacAccountCredentialHelperRejectsInvalidIdentifiersBeforeSpawning)
{
	for (const auto& name : std::vector<std::string>{"", "service\nother", std::string("service\0hidden", 14), std::string(1025, 'x')})
	{
		ClaudeAccountHttpsFixture fixture;
		const auto result = uam::usage::ReadMacAccountCredentialWithProcessService(fixture, name, "account");
		UAM_ASSERT(result.credential.empty()); UAM_ASSERT(!result.error.empty()); UAM_ASSERT_EQ(fixture.starts, 0);
	}
}
