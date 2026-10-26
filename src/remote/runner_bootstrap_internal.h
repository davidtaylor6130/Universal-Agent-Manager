#pragma once

#include "remote/runner_bootstrap.h"

#include <functional>

class IPlatformProcessService;

namespace uam::remote::detail
{
	enum class BootstrapProbeStatus
	{
		Succeeded,
		CommandFailed,
		TransportFailed,
		LaunchFailed,
		ReadFailed,
		OutputLimitExceeded,
		TimedOut,
		Canceled,
		WorkingDirectoryUnavailable,
		VerificationFailed
	};

	struct BootstrapProbeResult
	{
		BootstrapProbeStatus status = BootstrapProbeStatus::LaunchFailed;
		int exit_code = -1;
		std::string output;
		std::string diagnostic;
	};

	using BootstrapProbeExecutor = std::function<BootstrapProbeResult(const BootstrapStep&, std::stop_token)>;

	BootstrapProbeResult RunBootstrapProbe(const BootstrapStep& step, IPlatformProcessService& service,
	                                       std::stop_token stop_token = {});

	// Shared production/test seam: probe failures never make raw diagnostics public.
	bool ProbeBootstrapPlatform(const BootstrapPlan& plan, BootstrapResult& result,
	                            const BootstrapProbeExecutor& execute, std::stop_token stop_token = {});
}
