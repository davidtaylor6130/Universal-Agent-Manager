#pragma once

#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <poll.h>
#endif

namespace uam::platform
{
	enum class ProcessStopOutcome { Unknown, Graceful, Forced, Failed };

	/// <summary>Waits up to max for output instead of a fixed sleep, so pending output drains as soon as it is written.</summary>
	inline void WaitForStdioProcessOutput(StdioProcessPlatformFields& process, std::chrono::milliseconds max, bool include_stderr = true)
	{
#if defined(_WIN32)
		(void)process;
		(void)include_stderr;
		std::this_thread::sleep_for(max); // ponytail: anonymous pipes have no readiness wait, so Windows keeps its fixed pause.
#else
		std::array<pollfd, 2> fds{{{process.stdout_read_fd, POLLIN, 0}, {include_stderr ? process.stderr_read_fd : -1, POLLIN, 0}}};
		const int ready = ::poll(fds.data(), fds.size(), static_cast<int>(max.count()));
		// Callers wait only after a drain read nothing, so a hangup (macOS reports POLLIN|POLLHUP at EOF) means no
		// more output: pause rather than spin until the caller notices the exit.
		if (ready > 0 && std::ranges::all_of(fds, [](const pollfd& fd) { return fd.revents == 0 || (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0; }))
			std::this_thread::sleep_for(max);
#endif
	}

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
			const std::size_t drained_before = result.standard_output.size() + result.standard_error.size();
			drain();
			if (service.PollStdioProcessExited(process, &result.exit_code))
			{
				drain();
				result.exit_confirmed = true;
				result.outcome = result.exit_code == 0 && !output_failed ? ProcessStopOutcome::Graceful : ProcessStopOutcome::Failed;
				return result;
			}
			if (std::chrono::steady_clock::now() >= deadline) break;
			// A child still flushing output is drained again at once; the pause is only for an idle child.
			if (result.standard_output.size() + result.standard_error.size() == drained_before)
				WaitForStdioProcessOutput(process, std::chrono::milliseconds(10));
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
