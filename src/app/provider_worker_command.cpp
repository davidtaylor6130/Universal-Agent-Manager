#include "app/provider_worker_command.h"

#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/state/app_state.h"

#include <array>
#include <chrono>
#include <iterator>
#include <thread>
#include <utility>

namespace
{
	bool IsCopilotProfile(const ProviderProfile& profile)
	{
		return uam::provider_ids::IsCliProviderAliasOf(profile.id, uam::provider_ids::kCopilotCli);
	}

	void SetError(std::string* error_out, std::string message)
	{
		if (error_out != nullptr)
		{
			*error_out = std::move(message);
		}
	}

	std::optional<std::filesystem::path> PrepareCopilotWorkerIsolationDirectory()
	{
		const std::optional<std::filesystem::path> temp = uam::paths::TempDirectoryPathNoThrow();
		if (!temp)
		{
			return std::nullopt;
		}

		const std::filesystem::path workspace = *temp / "uam-copilot-text-worker-7f4938d1";
		std::error_code error;
		std::filesystem::create_directories(workspace, error);
		return error ? std::nullopt : std::optional<std::filesystem::path>(workspace);
	}

	bool RemoveCopilotPromptArgument(std::vector<std::string>& argv)
	{
		const auto prompt_flag = std::ranges::find(argv, "-p");
		if (prompt_flag == argv.end() || std::next(prompt_flag) == argv.end())
		{
			return false;
		}

		argv.erase(prompt_flag, std::next(prompt_flag, 2));
		return true;
	}

	enum class ProviderWorkerReadStatus
	{
		Data,
		NoData,
		Closed,
		Error,
	};

	ProviderWorkerReadStatus ReadProviderWorkerOutput(IPlatformProcessService& process_service, uam::platform::StdioProcessPlatformFields& process, ProcessExecutionResult& result, bool preserve_existing_error)
	{
		std::array<char, 4096> buffer{};
		std::string read_error;
		const std::ptrdiff_t bytes_read = process_service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &read_error);
		if (bytes_read > 0)
		{
			result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
			return ProviderWorkerReadStatus::Data;
		}
		if (bytes_read == 0)
		{
			return ProviderWorkerReadStatus::Closed;
		}
		if (bytes_read == -2)
		{
			return ProviderWorkerReadStatus::NoData;
		}

		if (!preserve_existing_error)
		{
			result.error = "Failed to read provider worker output: " + uam::strings::NonEmptyOrFallback(read_error, "unknown error") + ".";
		}
		return ProviderWorkerReadStatus::Error;
	}

	void DrainProviderWorkerTail(IPlatformProcessService& process_service, uam::platform::StdioProcessPlatformFields& process, ProcessExecutionResult& result, bool preserve_existing_error)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
		for (;;)
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return;
			}
			const ProviderWorkerReadStatus status = ReadProviderWorkerOutput(process_service, process, result, preserve_existing_error);
			if (status == ProviderWorkerReadStatus::Data)
			{
				continue;
			}
			if (status == ProviderWorkerReadStatus::Closed || status == ProviderWorkerReadStatus::Error)
			{
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	ProcessExecutionResult ExecuteProviderWorkerDirect(const uam::ProviderWorkerInvocation& invocation, const std::filesystem::path& working_directory, int timeout_ms, std::stop_token stop_token)
	{
		ProcessExecutionResult result;
		auto& process_service = PlatformServicesFactory::Instance().process_service;
		uam::platform::StdioProcessPlatformFields process;
		if (!process_service.StartStdioProcessWithInput(process, working_directory, invocation.argv, invocation.standard_input, &result.error))
		{
			return result;
		}

		const auto deadline = timeout_ms >= 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms) : std::chrono::steady_clock::time_point::max();

		int exit_code = -1;
		for (;;)
		{
			const ProviderWorkerReadStatus read_status = ReadProviderWorkerOutput(process_service, process, result, false);
			if (read_status == ProviderWorkerReadStatus::Error)
			{
				process_service.TerminateStdioProcess(process, true);
				DrainProviderWorkerTail(process_service, process, result, true);
				process_service.CloseStdioProcessHandles(process);
				return result;
			}

			if (process_service.PollStdioProcessExited(process, &exit_code))
			{
				break;
			}
			if (stop_token.stop_requested())
			{
				result.canceled = true;
				result.error = "Command canceled.";
				process_service.TerminateStdioProcess(process, false);
				DrainProviderWorkerTail(process_service, process, result, true);
				process_service.CloseStdioProcessHandles(process);
				return result;
			}
			if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
			{
				result.timed_out = true;
				result.error = "Command timed out.";
				process_service.TerminateStdioProcess(process, false);
				DrainProviderWorkerTail(process_service, process, result, true);
				process_service.CloseStdioProcessHandles(process);
				return result;
			}

			if (read_status != ProviderWorkerReadStatus::Data)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		DrainProviderWorkerTail(process_service, process, result, false);
		process_service.CloseStdioProcessHandles(process);
		result.exit_code = exit_code;
		result.ok = result.error.empty() && result.exit_code == 0;
		return result;
	}
} // namespace

namespace uam
{
	ProviderWorkerInvocation BuildProviderWorkerInvocation(const AppState& app, const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id, ProviderWorkerPathMode path_mode, std::string* error_out)
	{
		if (error_out != nullptr)
		{
			error_out->clear();
		}
		if (IsCopilotProfile(profile))
		{
			if (const std::string compatibility_error = CopilotLaunchBlockReason(app); !compatibility_error.empty())
			{
				SetError(error_out, compatibility_error);
				return {};
			}
		}

		const auto& runtime = ProviderRuntimeRegistry::Resolve(profile);
		auto argv = runtime.BuildWorkerArgv(profile, settings, prompt, model_id);
		if (argv.empty())
		{
			return {};
		}

		if (IsCopilotProfile(profile))
		{
			const std::optional<std::filesystem::path> isolation_directory = PrepareCopilotWorkerIsolationDirectory();
			if (!isolation_directory)
			{
				SetError(error_out, "Failed to prepare the isolated Copilot worker directory.");
				return {};
			}

			argv.insert(argv.begin() + 1, {
			                                  "-C",
			                                  uam::paths::Utf8PathString(*isolation_directory),
			                              });
		}

#if defined(_WIN32)
		if (IsCopilotProfile(profile))
		{
			if (!RemoveCopilotPromptArgument(argv))
			{
				SetError(error_out, "Copilot worker prompt arguments are invalid.");
				return {};
			}

			ProviderWorkerInvocation invocation;
			invocation.direct_process = true;
			invocation.argv = std::move(argv);
			invocation.standard_input = std::string(prompt);
			invocation.command_preview = BuildProviderWorkerShellCommand(invocation.argv, path_mode) + " <prompt via stdin>";
			return invocation;
		}
#endif

		ProviderWorkerInvocation invocation;
		invocation.argv = std::move(argv);
		invocation.shell_command = BuildProviderWorkerShellCommand(invocation.argv, path_mode);
		invocation.command_preview = invocation.shell_command;
		return invocation;
	}

	ProcessExecutionResult ExecuteProviderWorkerInvocation(const ProviderWorkerInvocation& invocation, const std::filesystem::path& working_directory, int timeout_ms, std::stop_token stop_token)
	{
		if (invocation.Empty())
		{
			ProcessExecutionResult result;
			result.error = "Provider worker command is empty.";
			return result;
		}
		if (invocation.direct_process)
		{
			return ExecuteProviderWorkerDirect(invocation, working_directory, timeout_ms, stop_token);
		}

		auto& process_service = PlatformServicesFactory::Instance().process_service;
		const std::string shell_command = process_service.BuildShellCommandWithWorkingDirectory(working_directory, invocation.shell_command);
		return process_service.ExecuteCommand(shell_command, timeout_ms, stop_token);
	}
} // namespace uam
