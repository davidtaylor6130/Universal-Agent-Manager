#include "test_harness.h"
#include "common/usage/google_account_allowance.h"
#include "common/platform/platform_services.h"
#include <cstring>

using namespace uam_test;

namespace
{
	const std::int64_t kGoogleNow = 1792116000000LL;
	class GoogleAccountProcessFixture final : public IPlatformProcessService
	{
	public:
		std::vector<std::pair<nlohmann::json, int>> responses;
		mutable std::vector<std::vector<std::string>> arguments;
		mutable std::vector<std::string> inputs;
		mutable std::string output;
		mutable std::size_t offset = 0;
		mutable int stops = 0;
		bool silent = false;
		bool flood = false;
		bool SupportsDetachedProcesses() const override { return false; }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
		std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override { return {}; }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { return false; }
		int NormalizeCapturedCommandExitCode(int code) const override { return code; }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { return {}; }
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>& argv,
		    std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			const std::size_t index = arguments.size(); arguments.push_back(argv); inputs.emplace_back(); offset = 0;
			output = index < responses.size() ? responses[index].first.dump() + "\n" + std::to_string(responses[index].second) : "{}\n500";
			return true;
		}
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&,
		    std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { return false; }
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override {}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char* data, std::size_t size, std::string*) const override
		{
			inputs.back().append(data, size); return true;
		}
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override {}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++stops; }
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { ++stops; }
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char* data, std::size_t size, std::string*) const override
		{
			if (silent) return -2;
			if (flood) { std::memset(data, 'x', size); return static_cast<std::ptrdiff_t>(size); }
			if (offset == output.size()) return 0;
			const auto count = std::min(size, output.size() - offset); std::memcpy(data, output.data() + offset, count); offset += count;
			return static_cast<std::ptrdiff_t>(count);
		}
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { return -2; }
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int* code) const override
		{
			if (silent || flood || offset != output.size()) return false;
			if (code) *code = 0; return true;
		}
		fs::path ResolveCurrentExecutablePath() const override { return {}; }
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const fs::path&, std::string*) const override { return {}; }
		uintmax_t NativeGeminiSessionMaxFileBytes() const override { return 0; }
		std::size_t NativeGeminiSessionMaxMessages() const override { return 0; }
		std::string GenerateUuid() const override { return {}; }
		bool LaunchShellAt(const fs::path&, std::string*) const override { return false; }
		void SetKeepSystemAwake(bool) const override {}
		bool EmbeddedBrowserUsesMockKeychain() const override { return false; }
	};
	nlohmann::json GeminiCredential()
	{
		return {{"access_token", "synthetic-access"}, {"expiry_date", 4102444800000LL}};
	}
	nlohmann::json AntigravityCredential()
	{
		return {{"auth_method", "consumer"}, {"token", {{"access_token", "synthetic-access"}, {"expiry", "2100-01-01T00:00:00Z"}}}};
	}
}

UAM_TEST(GeminiAccountAllowancePreservesEveryModelBucketAndReportedReset)
{
	const auto result = uam::usage::NormalizeGeminiAccountAllowance({{"buckets", {
	    {{"modelId", "gemini-pro"}, {"tokenType", "REQUESTS"}, {"remainingFraction", 0.75}, {"resetTime", "2026-11-01T00:00:00Z"}},
	    {{"modelId", "gemini-flash"}, {"tokenType", "REQUESTS"}, {"remainingFraction", 1}},
	    {{"modelId", "gemini-pro"}, {"tokenType", "TOKENS"}, {"remainingFraction", 0}}
	}}}, kGoogleNow);
	UAM_ASSERT(result.error.empty()); UAM_ASSERT_EQ(result.windows.size(), std::size_t(3));
	UAM_ASSERT_EQ(result.windows[0]["id"], "gemini-pro:REQUESTS");
	UAM_ASSERT_EQ(result.windows[0]["usedPercent"], 25);
	UAM_ASSERT_EQ(result.windows[0]["resetsAt"], 1793491200);
	UAM_ASSERT(result.windows[0]["windowDurationMinutes"].is_null());
	UAM_ASSERT_EQ(result.windows[1]["usedPercent"], 0);
	UAM_ASSERT_EQ(result.windows[2]["usedPercent"], 100);
	UAM_ASSERT(result.limits.at("primary").is_null()); UAM_ASSERT(result.limits.at("secondary").is_null());
	UAM_ASSERT_EQ(result.limits.at("updatedAt"), kGoogleNow);
}

UAM_TEST(GoogleAccountAllowanceRejectsMissingMalformedAndOversizedQuotaWithoutInventingZero)
{
	for (const nlohmann::json bad : {nlohmann::json(-0.1), nlohmann::json(1.1), nlohmann::json("0.5"), nlohmann::json(nullptr)})
	{
		const auto result = uam::usage::NormalizeGeminiAccountAllowance({{"buckets", {{{"modelId", "gemini-pro"}, {"remainingFraction", bad}}}}}, kGoogleNow);
		UAM_ASSERT(result.limits.is_null()); UAM_ASSERT(result.windows.empty()); UAM_ASSERT(!result.error.empty());
	}
	nlohmann::json excessive = nlohmann::json::array();
	for (int i = 0; i < 65; ++i) excessive.push_back({{"modelId", "model-" + std::to_string(i)}, {"remainingFraction", 0.5}});
	UAM_ASSERT(uam::usage::NormalizeGeminiAccountAllowance({{"buckets", excessive}}, kGoogleNow).limits.is_null());
	UAM_ASSERT(uam::usage::NormalizeGeminiAccountAllowance({{"buckets", nlohmann::json::array()}}, kGoogleNow).limits.is_null());
	UAM_ASSERT(uam::usage::NormalizeGeminiAccountAllowance({{"buckets", {{{"modelId", "model"}, {"remainingFraction", 0.5}}}}}, 0).limits.is_null());
}

UAM_TEST(AntigravityAccountAllowancePreservesAllGroupsAndDoesNotInventPeriods)
{
	const auto result = uam::usage::NormalizeAntigravityAccountAllowance({{"groups", {
	    {{"displayName", "Gemini models"}, {"buckets", {
	        {{"bucketId", "gemini-weekly"}, {"displayName", "Weekly"}, {"window", "weekly"}, {"remainingFraction", 0.75}, {"resetTime", "2026-11-01T00:00:00Z"}},
	        {{"bucketId", "gemini-session"}, {"displayName", "Session"}, {"remainingFraction", 0.5}}
	    }}},
	    {{"displayName", "Claude models"}, {"buckets", {
	        {{"bucketId", "claude-weekly"}, {"remainingFraction", 1}},
	        {{"bucketId", "claude-session"}, {"remainingFraction", 0}},
	        {{"bucketId", "disabled"}, {"remainingFraction", 0}, {"disabled", true}}
	    }}}
	}}}, kGoogleNow);
	UAM_ASSERT(result.error.empty()); UAM_ASSERT_EQ(result.windows.size(), std::size_t(4));
	UAM_ASSERT_EQ(result.windows[0]["id"], "gemini-weekly");
	UAM_ASSERT_EQ(result.windows[0]["label"], "Gemini models · Weekly");
	UAM_ASSERT_EQ(result.windows[0]["usedPercent"], 25);
	UAM_ASSERT_EQ(result.windows[0]["resetsAt"], 1793491200);
	UAM_ASSERT(result.windows[0]["windowDurationMinutes"].is_null());
	UAM_ASSERT_EQ(result.windows[2]["usedPercent"], 0);
	UAM_ASSERT_EQ(result.windows[3]["usedPercent"], 100);
}

UAM_TEST(AntigravityAccountAllowanceHandlesFlatBucketsWithoutDuplicateGroupedMeters)
{
	const nlohmann::json bucket{{"bucketId", "weekly"}, {"displayName", "Weekly allowance"}, {"remainingFraction", 0.29}, {"resetTime", "invalid"}};
	const auto flat = uam::usage::NormalizeAntigravityAccountAllowance({{"buckets", {bucket}}}, kGoogleNow);
	UAM_ASSERT(flat.error.empty()); UAM_ASSERT_EQ(flat.windows.size(), std::size_t(1));
	UAM_ASSERT(flat.windows[0]["resetsAt"].is_null());
	const auto both = uam::usage::NormalizeAntigravityAccountAllowance({{"buckets", {bucket}}, {"groups", {{{"displayName", "Models"}, {"buckets", {bucket}}}}}}, kGoogleNow);
	UAM_ASSERT(both.error.empty()); UAM_ASSERT_EQ(both.windows.size(), std::size_t(1));
	UAM_ASSERT_EQ(both.windows[0]["label"], "Models · Weekly allowance");
}

UAM_TEST(GeminiAccountAllowanceTransportReadsOnlyExistingProjectAndQuotaWithPrivateCredentialPipe)
{
	GoogleAccountProcessFixture fixture;
	fixture.responses = {{{{"cloudaicompanionProject", "synthetic-project"}}, 200},
	    {{{"buckets", {{{"modelId", "gemini-pro"}, {"remainingFraction", 0.75}}}}}, 200}};
	const auto result = uam::usage::FetchGeminiAccountAllowanceWithProcessService(fixture, {}, GeminiCredential());
	UAM_ASSERT(result.error.empty()); UAM_ASSERT_EQ(result.windows.size(), std::size_t(1));
	UAM_ASSERT_EQ(fixture.arguments.size(), std::size_t(2)); UAM_ASSERT_EQ(fixture.stops, 2);
	UAM_ASSERT_EQ(fixture.arguments[0].back(), "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist");
	UAM_ASSERT_EQ(fixture.arguments[1].back(), "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota");
	UAM_ASSERT(nlohmann::json(fixture.arguments).dump().find("synthetic-access") == std::string::npos);
	UAM_ASSERT(fixture.inputs[0].find("synthetic-access") != std::string::npos);
	UAM_ASSERT(fixture.inputs[1].find("synthetic-project") != std::string::npos);
}

UAM_TEST(GeminiAccountAllowanceRefreshesExpiredGrantInMemoryAndStopsAtUnsupportedClient)
{
	GoogleAccountProcessFixture fixture;
	fixture.responses = {{{{"access_token", "synthetic-fresh"}, {"expires_in", 3600}}, 200},
	    {{{"ineligibleTiers", {{{"reasonCode", "UNSUPPORTED_CLIENT"}, {"reasonMessage", "raw-secret-do-not-leak"}}}}}, 200}};
	const auto result = uam::usage::FetchGeminiAccountAllowanceWithProcessService(fixture, {}, {{"refresh_token", "synthetic-refresh"}, {"expiry_date", 1}, {"client_id", "test-client"}, {"client_secret", "test-secret"}});
	UAM_ASSERT(result.account_unavailable); UAM_ASSERT(!result.error.empty()); UAM_ASSERT(result.limits.is_null());
	UAM_ASSERT(result.error.find("unsupported") != std::string::npos);
	UAM_ASSERT(result.error.find("raw-secret") == std::string::npos);
	UAM_ASSERT_EQ(fixture.arguments.size(), std::size_t(2)); UAM_ASSERT_EQ(fixture.stops, 2);
	UAM_ASSERT_EQ(fixture.arguments[0].back(), "https://oauth2.googleapis.com/token");
	UAM_ASSERT(fixture.inputs[0].find("synthetic-refresh") != std::string::npos);
	UAM_ASSERT(fixture.inputs[0].find("client_id=test-client&client_secret=test-secret") != std::string::npos);
	UAM_ASSERT(nlohmann::json(fixture.arguments).dump().find("test-secret") == std::string::npos);
	UAM_ASSERT(nlohmann::json(fixture.arguments).dump().find("synthetic-refresh") == std::string::npos);
}

UAM_TEST(AntigravityAccountAllowanceTransportResolvesProjectBeforeGroupedSummary)
{
	GoogleAccountProcessFixture fixture;
	fixture.responses = {{{{"cloudaicompanionProject", "synthetic-project"}}, 200},
	    {{{"groups", {{{"displayName", "Gemini Models"}, {"buckets", {{{"bucketId", "gemini-weekly"}, {"remainingFraction", 1}}}}},
	                   {{"displayName", "Claude Models"}, {"buckets", {{{"bucketId", "3p-weekly"}, {"remainingFraction", 0.5}}}}}}}}, 200}};
	const auto result = uam::usage::FetchAntigravityAccountAllowanceWithProcessService(fixture, {}, AntigravityCredential());
	UAM_ASSERT(result.error.empty()); UAM_ASSERT_EQ(result.windows.size(), std::size_t(2));
	UAM_ASSERT_EQ(fixture.arguments.size(), std::size_t(2)); UAM_ASSERT_EQ(fixture.stops, 2);
	UAM_ASSERT_EQ(fixture.arguments[1].back(), "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary");
	UAM_ASSERT(fixture.inputs[1].find("synthetic-project") != std::string::npos);
	UAM_ASSERT(nlohmann::json(fixture.arguments).dump().find("synthetic-access") == std::string::npos);
}

UAM_TEST(GoogleAccountAllowanceTransportRejectsCredentialInjectionAndReapsDeadlineAndOutputLimit)
{
	for (bool antigravity : {false, true})
	{
		GoogleAccountProcessFixture fixture;
		auto credential = antigravity ? AntigravityCredential() : GeminiCredential();
		if (antigravity) credential["token"]["access_token"] = "token\nheader = unsafe";
		else credential["access_token"] = "token\nheader = unsafe";
		const auto invalid = antigravity ? uam::usage::FetchAntigravityAccountAllowanceWithProcessService(fixture, {}, credential)
		    : uam::usage::FetchGeminiAccountAllowanceWithProcessService(fixture, {}, credential);
		UAM_ASSERT(!invalid.error.empty()); UAM_ASSERT(fixture.arguments.empty());
	}
	for (bool flood : {false, true})
	{
		GoogleAccountProcessFixture fixture; fixture.flood = flood; fixture.silent = !flood;
		const auto result = uam::usage::FetchGeminiAccountAllowanceWithProcessService(fixture, {}, GeminiCredential(), {}, std::chrono::milliseconds(flood ? 1000 : 10));
		UAM_ASSERT(!result.error.empty()); UAM_ASSERT(result.limits.is_null()); UAM_ASSERT_EQ(fixture.stops, 1);
	}
	GoogleAccountProcessFixture cancelled; std::stop_source stop; stop.request_stop();
	UAM_ASSERT(!uam::usage::FetchGeminiAccountAllowanceWithProcessService(cancelled, {}, GeminiCredential(), stop.get_token()).error.empty());
	UAM_ASSERT(cancelled.arguments.empty());
}

UAM_TEST(GoogleAccountAllowanceTransportClearsUnauthorizedAccountWithoutExposingProviderBody)
{
	GoogleAccountProcessFixture fixture;
	fixture.responses = {{{{"error", {{"code", 401}, {"message", "synthetic-access raw-token-user-data"}}}}, 401}};
	const auto result = uam::usage::FetchGeminiAccountAllowanceWithProcessService(fixture, {}, GeminiCredential());
	UAM_ASSERT(result.account_unavailable); UAM_ASSERT(!result.error.empty()); UAM_ASSERT(result.limits.is_null());
	UAM_ASSERT(result.error.find("synthetic-access") == std::string::npos);
	UAM_ASSERT(result.error.find("raw-token") == std::string::npos);
	UAM_ASSERT_EQ(fixture.stops, 1);
}

UAM_TEST(GoogleAccountAllowanceExpiredLoginRequiresExistingRegistrationBeforeNetwork)
{
	for (bool antigravity : {false, true})
	{
		for (bool malformed : {false, true})
		{
			GoogleAccountProcessFixture fixture;
			nlohmann::json token{{"access_token", "synthetic-expired"}, {"refresh_token", "synthetic-refresh"}, {"expiry_date", 1}, {"expiry", "2000-01-01T00:00:00Z"}};
			if (malformed) { token["client_id"] = "test-client"; token["client_secret"] = "bad\nregistration"; }
			const nlohmann::json credential = antigravity ? nlohmann::json{{"auth_method", "consumer"}, {"token", token}} : token;
			const auto result = antigravity ? uam::usage::FetchAntigravityAccountAllowanceWithProcessService(fixture, {}, credential)
			    : uam::usage::FetchGeminiAccountAllowanceWithProcessService(fixture, {}, credential);
			UAM_ASSERT(result.account_unavailable); UAM_ASSERT(result.limits.is_null());
			UAM_ASSERT(result.error.find("provider CLI") != std::string::npos);
			UAM_ASSERT(fixture.arguments.empty()); UAM_ASSERT(fixture.inputs.empty());
		}
	}
}

UAM_TEST(AntigravityAccountAllowanceRefreshUsesOnlyExistingProfileRegistration)
{
	GoogleAccountProcessFixture fixture;
	fixture.responses = {{{{"access_token", "synthetic-fresh"}}, 200},
	    {{{"cloudaicompanionProject", "synthetic-project"}}, 200},
	    {{{"buckets", {{{"bucketId", "weekly"}, {"remainingFraction", 0.5}}}}}, 200}};
	const nlohmann::json profile{{"auth_method", "consumer"}, {"client_id", "test-client"}, {"client_secret", "test-secret"},
	    {"token", {{"refresh_token", "synthetic-refresh"}, {"expiry", "2000-01-01T00:00:00Z"}}}};
	const auto result = uam::usage::FetchAntigravityAccountAllowanceWithProcessService(fixture, {}, profile);
	UAM_ASSERT(result.error.empty()); UAM_ASSERT_EQ(result.windows.size(), std::size_t(1));
	UAM_ASSERT_EQ(fixture.arguments.size(), std::size_t(3));
	UAM_ASSERT(fixture.inputs[0].find("client_id=test-client&client_secret=test-secret") != std::string::npos);
	UAM_ASSERT(nlohmann::json(fixture.arguments).dump().find("test-secret") == std::string::npos);
}
