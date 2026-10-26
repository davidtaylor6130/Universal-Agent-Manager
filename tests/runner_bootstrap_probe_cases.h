#pragma once

#include "remote/runner_bootstrap_internal.h"
#include "bootstrap_probe_process_fixture.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

UAM_TEST(RemoteRunnerBootstrapPreservesInjectedProbeFailuresPrivately)
{
	using namespace uam::remote;
	using namespace uam::remote::detail;
	const BootstrapPlan plan{.steps = {{"unix", {}, ""}, {"windows", {}, ""}}};
	const std::vector<std::pair<BootstrapProbeStatus, std::string>> failures = {
	    {BootstrapProbeStatus::TransportFailed, "Remote setup SSH transport failed (exit 255)."},
	    {BootstrapProbeStatus::LaunchFailed, "Remote setup platform probe could not be launched."},
	    {BootstrapProbeStatus::ReadFailed, "Remote setup output could not be read."},
	    {BootstrapProbeStatus::OutputLimitExceeded, "Remote setup output exceeded 1 MiB."},
	    {BootstrapProbeStatus::TimedOut, "Remote setup timed out."},
	    {BootstrapProbeStatus::Canceled, "Remote setup was canceled."},
	    {BootstrapProbeStatus::WorkingDirectoryUnavailable, "The remote setup working directory is unavailable."},
	    {BootstrapProbeStatus::VerificationFailed, "Remote runner version verification failed."}};
	for (const auto& [status, expected] : failures)
	for (const std::size_t failed_index : {std::size_t{0}, std::size_t{1}})
	{
		BootstrapResult result;
		std::size_t calls = 0;
		const auto execute = [&](const BootstrapStep& step, std::stop_token)
		{
			UAM_ASSERT_EQ(step.label, plan.steps[calls].label);
			const bool fail_here = calls++ == failed_index;
			return BootstrapProbeResult{fail_here ? status : BootstrapProbeStatus::CommandFailed,
			    fail_here ? 255 : 127, "Linux\nx86_64\n", "private-key=/private/fixture/identity; secret-token"};
		};
		UAM_ASSERT(!ProbeBootstrapPlatform(plan, result, execute));
		UAM_ASSERT_EQ(calls, failed_index + 1);
		UAM_ASSERT_EQ(result.error, expected);
		UAM_ASSERT(result.platform.empty());
		UAM_ASSERT(result.architecture.empty());
		UAM_ASSERT(!result.install_lease);
	}
	BootstrapResult result;
	std::size_t calls = 0;
	UAM_ASSERT(!ProbeBootstrapPlatform(plan, result, [&](const BootstrapStep&, std::stop_token)
	{
		++calls;
		return BootstrapProbeResult{BootstrapProbeStatus::CommandFailed, calls == 1 ? 127 : 23,
		    "private stdout", "private stderr"};
	}));
	UAM_ASSERT_EQ(calls, std::size_t{2});
	UAM_ASSERT_EQ(result.error, std::string("Remote setup platform probe command failed (exit 23)."));
}

UAM_TEST(RemoteRunnerBootstrapParsesOnlySuccessfulProbeOutput)
{
	using namespace uam::remote;
	using namespace uam::remote::detail;
	const BootstrapPlan plan{.steps = {{"unix", {}, ""}, {"windows", {}, ""}}};
	for (const std::string architecture : {"x86_64", "AMD64", "aarch64", "ARM64"})
	for (const bool fallback : {false, true})
	{
		BootstrapResult result;
		std::size_t calls = 0;
		UAM_ASSERT(ProbeBootstrapPlatform(plan, result, [&](const BootstrapStep&, std::stop_token)
		{
			if (calls++ == 0 && fallback)
				return BootstrapProbeResult{BootstrapProbeStatus::CommandFailed, 127, "stale output", "private diagnostic"};
			return BootstrapProbeResult{BootstrapProbeStatus::Succeeded, 0,
			    std::string(fallback ? "Windows\r\n" : "Linux\n") + architecture + "\r\n", "private warning"};
		}));
		UAM_ASSERT_EQ(calls, fallback ? std::size_t{2} : std::size_t{1});
		UAM_ASSERT_EQ(result.platform, std::string(fallback ? "windows" : "linux"));
		UAM_ASSERT_EQ(result.architecture, std::string(architecture == "x86_64" || architecture == "AMD64" ? "x86_64" : "arm64"));
		UAM_ASSERT(result.error.empty());
	}
}

UAM_TEST(RemoteRunnerBootstrapSeparatesUnsupportedAndMalformedProbeOutput)
{
	using namespace uam::remote;
	using namespace uam::remote::detail;
	const BootstrapPlan plan{.steps = {{"unix", {}, ""}, {"windows", {}, ""}}};
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {"Darwin\narm64\n", "Remote host is not Ubuntu/Linux or Windows."},
	    {"Linux\nriscv64\n", "Remote CPU architecture is unsupported."},
	    {"Linux\nprivate-token\n", "Remote CPU architecture is unsupported."},
	    {"", "Remote setup platform probe returned invalid output."},
	    {"Linux\n", "Remote setup platform probe returned invalid output."},
	    {"Linux\n/private/fixture/key\n", "Remote setup platform probe returned invalid output."},
	    {"Linux\nx86_64\nprivate diagnostic\n", "Remote setup platform probe returned invalid output."}};
	for (const auto& [output, expected] : cases)
	{
		BootstrapResult result;
		std::size_t calls = 0;
		UAM_ASSERT(!ProbeBootstrapPlatform(plan, result, [&](const BootstrapStep&, std::stop_token)
		{
			++calls;
			return BootstrapProbeResult{BootstrapProbeStatus::Succeeded, 0, output, "private diagnostic"};
		}));
		UAM_ASSERT_EQ(calls, std::size_t{1});
		UAM_ASSERT_EQ(result.error, expected);
		UAM_ASSERT(result.platform.empty());
		UAM_ASSERT(result.architecture.empty());
	}
}

UAM_TEST(RemoteRunnerBootstrapCancellationFencesProbeFallbackAndParsing)
{
	using namespace uam::remote;
	using namespace uam::remote::detail;
	const BootstrapPlan plan{.steps = {{"unix", {}, ""}, {"windows", {}, ""}}};
	for (const bool before_probe : {false, true})
	for (const bool successful_probe : {false, true})
	{
		BootstrapResult result;
		std::stop_source canceled;
		if (before_probe) canceled.request_stop();
		std::size_t calls = 0;
		UAM_ASSERT(!ProbeBootstrapPlatform(plan, result, [&](const BootstrapStep&, std::stop_token)
		{
			++calls;
			canceled.request_stop();
			return BootstrapProbeResult{successful_probe ? BootstrapProbeStatus::Succeeded : BootstrapProbeStatus::CommandFailed,
			    successful_probe ? 0 : 127, "Linux\nx86_64\n", "private diagnostic"};
		}, canceled.get_token()));
		UAM_ASSERT_EQ(calls, before_probe ? std::size_t{0} : std::size_t{1});
		UAM_ASSERT_EQ(result.error, std::string("Remote setup was canceled."));
		UAM_ASSERT(result.platform.empty());
		UAM_ASSERT(result.architecture.empty());
	}
}

UAM_TEST(RemoteRunnerBootstrapClassifiesInjectedProcessFailures)
{
	using namespace uam::remote;
	using namespace uam::remote::detail;
	const BootstrapStep step{"fixture probe", {"never-executed"}, ""};
	for (const auto status : {BootstrapProbeStatus::Succeeded, BootstrapProbeStatus::TransportFailed,
	    BootstrapProbeStatus::CommandFailed, BootstrapProbeStatus::LaunchFailed, BootstrapProbeStatus::ReadFailed,
	    BootstrapProbeStatus::OutputLimitExceeded})
	{
		uam_test::BootstrapProbeProcessFixture service;
		service.outcome = status;
		const auto probe = RunBootstrapProbe(step, service);
		UAM_ASSERT(probe.status == status);
		UAM_ASSERT_EQ(service.starts, std::size_t{1});
		UAM_ASSERT_EQ(service.stops, status == BootstrapProbeStatus::ReadFailed || status == BootstrapProbeStatus::OutputLimitExceeded ? std::size_t{1} : std::size_t{0});
		UAM_ASSERT_EQ(service.closes, status == BootstrapProbeStatus::ReadFailed || status == BootstrapProbeStatus::OutputLimitExceeded || status == BootstrapProbeStatus::LaunchFailed ? std::size_t{0} : std::size_t{1});
		if (status == BootstrapProbeStatus::TransportFailed) UAM_ASSERT_EQ(probe.exit_code, 255);
		if (status == BootstrapProbeStatus::CommandFailed) UAM_ASSERT_EQ(probe.exit_code, 127);
	}
	for (const bool before_launch : {false, true})
	{
		uam_test::BootstrapProbeProcessFixture service;
		std::stop_source canceled;
		if (before_launch) canceled.request_stop();
		else service.cancel_on_read = &canceled;
		const auto probe = RunBootstrapProbe(step, service, canceled.get_token());
		UAM_ASSERT(probe.status == BootstrapProbeStatus::Canceled);
		UAM_ASSERT_EQ(service.starts, before_launch ? std::size_t{0} : std::size_t{1});
		UAM_ASSERT_EQ(service.stops, before_launch ? std::size_t{0} : std::size_t{1});
	}
	BootstrapStep verify = step;
	verify.expected_output = "different-output";
	uam_test::BootstrapProbeProcessFixture service;
	UAM_ASSERT(RunBootstrapProbe(verify, service).status == BootstrapProbeStatus::VerificationFailed);
}

UAM_TEST(RemoteRunnerBootstrapCanceledPlanNeverReachesPlatformFactory)
{
	using namespace uam::remote;
	BootstrapPlan plan;
	plan.ssh_alias = "fixture";
	plan.steps = {{"unix", {"never-executed"}, ""}, {"windows", {"never-executed"}, ""}};
	plan.artifacts = {{"linux", "x86_64", {}, std::string(64, 'a')}};
	std::stop_source canceled;
	canceled.request_stop();
	const auto result = AcquireBootstrapPlan(plan, canceled.get_token());
	UAM_ASSERT(!result.ok);
	UAM_ASSERT_EQ(result.error, std::string("Remote setup was canceled."));
	UAM_ASSERT(!result.install_lease);
}

#if defined(__APPLE__) || defined(__linux__)
UAM_TEST(RemoteRunnerBootstrapMissingWorkingDirectoryNeverLaunchesInjectedProcess)
{
	const auto original = std::filesystem::current_path();
	struct RestorePath
	{
		std::filesystem::path original;
		std::filesystem::path created;
		~RestorePath()
		{
			std::error_code error;
			std::filesystem::current_path(original, error);
			if (!created.empty()) std::filesystem::remove(created, error);
		}
	} restore{original, {}};
	const std::string prefix = "bootstrap-missing-cwd-fixture-" +
	    uam::time::SteadyEpochNanosecondsTokenNow() + "-";
	for (std::size_t suffix = 0; ; ++suffix)
	{
		const auto candidate = original / (prefix + std::to_string(suffix));
		std::error_code error;
		if (std::filesystem::create_directory(candidate, error))
		{
			restore.created = candidate;
			break;
		}
		UAM_ASSERT(!error || error == std::errc::file_exists);
	}
	std::filesystem::current_path(restore.created);
	UAM_ASSERT(std::filesystem::remove(restore.created));
	uam_test::BootstrapProbeProcessFixture service;
	const auto probe = uam::remote::detail::RunBootstrapProbe({"fixture", {"never-executed"}, ""}, service);
	UAM_ASSERT(probe.status == uam::remote::detail::BootstrapProbeStatus::WorkingDirectoryUnavailable);
	UAM_ASSERT_EQ(service.starts, std::size_t{0});
}
#endif

