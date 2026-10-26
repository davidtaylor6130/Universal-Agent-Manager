#include "test_harness.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_native_context.h"
#include "common/usage/account_allowance_fetcher.h"
#include "common/usage/account_credentials_macos.h"
#include "common/usage/claude_account_allowance.h"
#include "common/usage/google_account_allowance.h"
#include "common/utils/base64.h"
#include "remote/runner_bootstrap.h"

#include <cstring>
#include <functional>

using namespace uam_test;

namespace
{
	/// <summary>A child that has already exited when first polled; its final output becomes readable only once that exit is observed.</summary>
	class ExitRaceProcess final : public IPlatformProcessService
	{
	  public:
		std::vector<std::string> outputs;
		std::function<std::string(std::string_view)> reply;
		int exit_code = 0;
		mutable std::string output;
		mutable std::size_t offset = 0;
		mutable std::size_t starts = 0;
		mutable bool exit_observed = false;

		bool SupportsDetachedProcesses() const override { return false; }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
		std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override { return {}; }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { return false; }
		int NormalizeCapturedCommandExitCode(int code) const override { return code; }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { return {}; }
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&,
		    std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			output = starts < outputs.size() ? outputs[starts] : std::string{};
			++starts; offset = 0; exit_observed = false;
			return true;
		}
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&,
		    std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { return false; }
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override {}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char* bytes, std::size_t length, std::string*) const override
		{
			if (reply) output += reply(std::string_view(bytes, length));
			return true;
		}
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override {}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override {}
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override {}
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char* bytes, std::size_t count, std::string*) const override
		{
			if (!exit_observed) return -2;
			if (offset == output.size()) return 0;
			// Small chunks keep the post-exit drain looping more than once.
			const std::size_t length = std::min({count, output.size() - offset, std::size_t(7)});
			std::memcpy(bytes, output.data() + offset, length); offset += length;
			return static_cast<std::ptrdiff_t>(length);
		}
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override
		{
			return exit_observed ? 0 : -2;
		}
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int* code) const override
		{
			exit_observed = true;
			if (code) *code = exit_code;
			return true;
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
}

UAM_TEST(ProcessExitDrainBootstrapStepKeepsVersionLineWrittenJustBeforeExit)
{
	ExitRaceProcess process; process.outputs = {"4.5.7\n"};
	std::string output, error;
	UAM_ASSERT(uam::remote::RunBootstrapStepForTests({"Verify runner version", {"ssh", "home-lab", "uam-runner --version"}, "4.5.7"},
	    process, output, error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(output, std::string("4.5.7\n"));
}

UAM_TEST(ProcessExitDrainBootstrapStepFailureKeepsFinalDiagnosticWithExitCode)
{
	ExitRaceProcess process; process.outputs = {"synthetic permission denied"}; process.exit_code = 3;
	std::string output, error;
	UAM_ASSERT(!uam::remote::RunBootstrapStepForTests({"Copy runner", {"ssh", "home-lab", "copy"}, ""}, process, output, error));
	UAM_ASSERT_EQ(error, std::string("Remote setup step failed (exit 3): Copy runner — synthetic permission denied"));
}

UAM_TEST(ProcessExitDrainGuardedBootstrapStepKeepsResultLineWrittenJustBeforeExit)
{
	ExitRaceProcess process;
	process.reply = [](std::string_view request)
	{
		const std::string id(request.substr(0, request.find(':')));
		return "UAM_INSTALL_RESULT:" + id + ":0:" + uam::base64::Encode("4.5.7") + "\n";
	};
	std::string output, error;
	UAM_ASSERT(uam::remote::RunGuardedBootstrapStepForTests({"Verify runner version", {"ssh", "home-lab", "uam-runner --version"}, "4.5.7"},
	    process, output, error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(output, std::string("4.5.7"));
}

UAM_TEST(ProcessExitDrainAccountReadersKeepResponseWrittenJustBeforeExit)
{
	ExitRaceProcess claude; claude.outputs = {"{\"five_hour\":{\"utilization\":57}}\n200"};
	const auto claude_result = uam::usage::FetchClaudeAccountAllowanceWithProcessService(claude, {}, "synthetic-token");
	UAM_ASSERT(claude_result.error.empty());
	UAM_ASSERT_EQ(claude_result.limits.at("primary").at("usedPercent"), 57);

	ExitRaceProcess keychain; keychain.outputs = {nlohmann::json{{"status", 0}, {"credential", "synthetic-login"}}.dump()};
	const auto credential = uam::usage::ReadMacAccountCredentialWithProcessService(keychain, "service", "account");
	UAM_ASSERT(credential.error.empty());
	UAM_ASSERT_EQ(credential.credential, std::string("synthetic-login"));

	ExitRaceProcess google;
	google.outputs = {nlohmann::json{{"cloudaicompanionProject", "synthetic-project"}}.dump() + "\n200",
	    nlohmann::json{{"buckets", {{{"modelId", "gemini-pro"}, {"remainingFraction", 0.75}}}}}.dump() + "\n200"};
	const auto google_result = uam::usage::FetchGeminiAccountAllowanceWithProcessService(google, {},
	    {{"access_token", "synthetic-access"}, {"expiry_date", 4102444800000LL}});
	UAM_ASSERT(google_result.error.empty());
	UAM_ASSERT_EQ(google_result.windows.size(), std::size_t(1));

	ExitRaceProcess codex;
	codex.reply = [](std::string_view line)
	{
		const nlohmann::json request = nlohmann::json::parse(line);
		if (!request.contains("id")) return std::string{};
		nlohmann::json response{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", nlohmann::json::object()}};
		if (request.value("method", "") == "account/rateLimits/read")
			response["result"] = {{"rateLimits", {{"primary", {{"usedPercent", 25}}}}}};
		return response.dump() + "\n";
	};
	const auto codex_result = uam::usage::FetchCliAccountAllowanceWithProcessService(codex, "codex-cli", {"codex"}, {});
	UAM_ASSERT(codex_result.error.empty());
	UAM_ASSERT_EQ(codex_result.limits.at("primary").at("usedPercent"), 25);
}

UAM_TEST(ProcessExitDrainCodexProfileRenderKeepsPromptWrittenJustBeforeExit)
{
	ExitRaceProcess process;
	process.outputs = {R"([{"role":"developer","content":[{"text":"policy"},{"text":"profile instructions"}]}])",
	    R"([{"role":"developer","content":[{"text":"policy"}]}])"};
	std::string instructions, error;
	UAM_ASSERT(uam::provider_native_context::ReadCodexProfileInstructions(process, {"codex", "-p", "work"}, {}, {}, instructions, error, {}));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(instructions, std::string("profile instructions"));
}

UAM_TEST(ProcessExitDrainBootstrapStepKeepsRealFastExitChildOutput)
{
	// Smoke check on real pipes; the fake-service cases above are the deterministic race proof.
#if defined(_WIN32)
	const std::vector<std::string> argv{"cmd.exe", "/d", "/c", "echo 4.5.7"};
#else
	const std::vector<std::string> argv{"/bin/sh", "-c", "printf '4.5.7\\n'"};
#endif
	for (int attempt = 0; attempt < 50; ++attempt)
	{
		std::string output, error;
		UAM_ASSERT(uam::remote::RunBootstrapStepForTests({"Verify runner version", argv, "4.5.7"},
		    PlatformServicesFactory::Instance().process_service, output, error));
		UAM_ASSERT(error.empty());
	}
}
