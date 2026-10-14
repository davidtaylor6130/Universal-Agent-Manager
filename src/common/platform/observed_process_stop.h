#pragma once

#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"

#include <array>
#include <chrono>
#include <string>
#include <thread>

namespace uam::platform
{
	enum class ProcessStopOutcome { Unknown, Graceful, Forced, Failed };

	struct ObservedProcessStopResult
	{
		ProcessStopOutcome outcome = ProcessStopOutcome::Unknown;
		int exit_code = -1;
		bool exit_confirmed = false;
		std::string standard_output;
		std::string standard_error;
	};

	/// <summary>Reads bounded available stop output without blocking or closing the owned pipes.</summary>
	inline bool DrainObservedProcessStopOutput(IPlatformProcessService& service, StdioProcessPlatformFields& process, ObservedProcessStopResult& result)
	{
		std::array<char, 16384> buffer{};
		bool output_failed = false;
		for (bool stderr_stream : {false, true})
		{
			std::string& output = stderr_stream ? result.standard_error : result.standard_output;
			for (int chunk = 0; chunk < 64; ++chunk)
			{
				const std::ptrdiff_t count = stderr_stream ? service.ReadStdioProcessStderr(process, buffer.data(), buffer.size())
				                                         : service.ReadStdioProcessStdout(process, buffer.data(), buffer.size());
				if (count <= 0) { if (count == -1) output_failed = true; break; }
				if (output.size() + static_cast<std::size_t>(count) <= 4 * 1024 * 1024)
					output.append(buffer.data(), static_cast<std::size_t>(count));
				else output_failed = true;
			}
		}
		return !output_failed;
	}

	/// <summary>Closes input, observes a bounded cooperative exit, and forces only the owned tree on timeout. Call off the UI thread with exclusive process ownership. Output handles remain open for the caller.</summary>
	inline ObservedProcessStopResult StopStdioProcessObserved(IPlatformProcessService& service, StdioProcessPlatformFields& process,
	                                                         std::chrono::milliseconds grace = std::chrono::milliseconds(800), bool allow_forced_stop = true)
	{
		ObservedProcessStopResult result;
		service.CloseStdioProcessInput(process);
		bool output_failed = false;
		const auto drain = [&]
		{
			if (!DrainObservedProcessStopOutput(service, process, result)) output_failed = true;
		};
		const auto deadline = std::chrono::steady_clock::now() + grace;
		for (;;)
		{
			drain();
			if (service.PollStdioProcessExited(process, &result.exit_code))
			{
				drain();
				result.exit_confirmed = true;
				result.outcome = result.exit_code == 0 && !output_failed ? ProcessStopOutcome::Graceful : ProcessStopOutcome::Failed;
				return result;
			}
			if (std::chrono::steady_clock::now() >= deadline) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (!allow_forced_stop) { result.outcome = ProcessStopOutcome::Failed; return result; }
		service.TerminateStdioProcess(process, true);
		const auto forced_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!(result.exit_confirmed = service.PollStdioProcessExited(process, &result.exit_code)) && std::chrono::steady_clock::now() < forced_deadline)
		{
			drain();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		drain();
		result.outcome = result.exit_confirmed ? ProcessStopOutcome::Forced : ProcessStopOutcome::Failed;
		return result;
	}
}
