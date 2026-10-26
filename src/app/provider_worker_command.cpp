#include "common/provider/provider_text_worker.h"
#include "app/provider_worker_command.h"
#include "common/config/execution_host_config.h"
#include "common/config/provider_availability.h"

#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/state/app_state.h"
#include "common/utils/io_utils.h"
#include "remote/runner_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <thread>
#include <utility>

namespace
{
	bool IsProvider(const ProviderProfile& profile, std::string_view provider_id)
	{
		return uam::provider_ids::IsCliProviderAliasOf(profile.id, provider_id);
	}

	void SetError(std::string* error_out, std::string message)
	{
		if (error_out != nullptr)
		{
			*error_out = std::move(message);
		}
	}

	std::shared_ptr<const std::filesystem::path> PrepareWorkerIsolationDirectory()
	{
		const std::optional<std::filesystem::path> temp = uam::paths::TempDirectoryPathNoThrow();
		if (!temp)
		{
			return {};
		}

		const std::string worker_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (worker_id.empty())
		{
			return {};
		}

		const std::filesystem::path workspace = *temp / ("uam-text-worker-" + worker_id);
		std::error_code error;
		const bool created = std::filesystem::create_directory(workspace, error);
		if (!created || error)
		{
			return {};
		}
		return std::shared_ptr<const std::filesystem::path>(new std::filesystem::path(workspace), [](const std::filesystem::path* path)
		                                                {
			                                                std::error_code cleanup_error;
			                                                (void)uam::paths::RemoveAllNoThrow(*path, &cleanup_error);
			                                                delete path;
		                                                });
	}



	std::vector<std::pair<std::string, std::string>> WorkerEnvironment(uam::ProviderWorkerPathMode path_mode)
	{
#if defined(_WIN32)
		(void)path_mode;
		return {};
#else
		std::string path = uam::JoinProviderWorkerPathEntries(uam::ProviderWorkerPathEntries(path_mode));
		if (const std::optional<std::string> inherited = uam::env::GetNonEmptyString("PATH"))
		{
			if (!path.empty()) path += ":";
			path += *inherited;
		}
		return path.empty() ? std::vector<std::pair<std::string, std::string>>{} : std::vector<std::pair<std::string, std::string>>{{"PATH", std::move(path)}};
#endif
	}

	enum class ProviderWorkerReadStatus
	{
		Data,
		NoData,
		Closed,
		Error,
	};

	ProviderWorkerReadStatus ReadProviderWorkerOutput(IPlatformProcessService& process_service, uam::platform::StdioProcessPlatformFields& process, ProcessExecutionResult& result, bool preserve_existing_error, const std::function<bool(std::string_view)>& sink = {})
	{
		std::array<char, 4096> buffer{};
		std::string read_error;
		const std::ptrdiff_t bytes_read = process_service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &read_error);
		if (bytes_read > 0)
		{
			if (sink ? !sink(std::string_view(buffer.data(), static_cast<std::size_t>(bytes_read))) :
			    !uam::platform::AppendCapturedCommandOutput(result, buffer.data(), static_cast<std::size_t>(bytes_read)))
			{
				result.error = sink ? "Failed to write provider worker output." : std::string(uam::platform::kCapturedCommandOutputLimitError);
				return ProviderWorkerReadStatus::Error;
			}
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

	ProviderWorkerReadStatus DrainStructuredWorkerDiagnostic(IPlatformProcessService& process_service, uam::platform::StdioProcessPlatformFields& process, ProcessExecutionResult& result)
	{
		std::array<char, 4096> buffer{};
		std::string read_error;
		const std::ptrdiff_t bytes = process_service.ReadStdioProcessStderr(process, buffer.data(), buffer.size(), &read_error);
		if (bytes > 0) return ProviderWorkerReadStatus::Data;
		if (bytes == 0) return ProviderWorkerReadStatus::Closed;
		if (bytes == -2) return ProviderWorkerReadStatus::NoData;
		result.error = "Failed to read provider worker diagnostics.";
		return ProviderWorkerReadStatus::Error;
	}

	ProcessExecutionResult ExecuteProviderWorkerDirect(const uam::ProviderWorkerInvocation& invocation, const std::filesystem::path& working_directory, int timeout_ms, std::stop_token stop_token)
	{
		ProcessExecutionResult result;
		auto& process_service = PlatformServicesFactory::Instance().process_service;
		uam::platform::StdioProcessPlatformFields process;
		if (invocation.structured_stdout && !invocation.standard_input.empty())
		{
			result.error = "Structured provider workers require empty input.";
			return result;
		}
		const bool started = invocation.structured_stdout
		    ? process_service.StartStdioProcess(process, working_directory, invocation.argv, &result.error, invocation.environment_overrides)
		    : process_service.StartStdioProcessWithInput(process, working_directory, invocation.argv, invocation.standard_input, &result.error, invocation.environment_overrides);
		if (!started) return result;
		if (invocation.structured_stdout) process_service.CloseStdioProcessInput(process);

		const auto deadline = timeout_ms >= 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms) : std::chrono::steady_clock::time_point::max();

		int exit_code = -1;
		for (;;)
		{
			const ProviderWorkerReadStatus read_status = ReadProviderWorkerOutput(process_service, process, result, false, invocation.output_sink);
			const ProviderWorkerReadStatus diagnostic_status = invocation.structured_stdout
			    ? DrainStructuredWorkerDiagnostic(process_service, process, result) : ProviderWorkerReadStatus::Closed;
			if (read_status == ProviderWorkerReadStatus::Error || diagnostic_status == ProviderWorkerReadStatus::Error)
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

			if (read_status != ProviderWorkerReadStatus::Data && diagnostic_status != ProviderWorkerReadStatus::Data)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		if (invocation.output_sink || invocation.structured_stdout)
		{
			for (;;)
			{
				const ProviderWorkerReadStatus status = ReadProviderWorkerOutput(process_service, process, result, false, invocation.output_sink);
				const ProviderWorkerReadStatus diagnostic_status = invocation.structured_stdout
				    ? DrainStructuredWorkerDiagnostic(process_service, process, result) : ProviderWorkerReadStatus::Closed;
				if (status == ProviderWorkerReadStatus::Error || diagnostic_status == ProviderWorkerReadStatus::Error ||
				    (status == ProviderWorkerReadStatus::Closed && diagnostic_status == ProviderWorkerReadStatus::Closed)) break;
				if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
				{
					result.canceled = stop_token.stop_requested();
					result.timed_out = !result.canceled;
					result.error = result.canceled ? "Command canceled." : "Command output did not close before its deadline.";
					break;
				}
				if (status != ProviderWorkerReadStatus::Data && diagnostic_status != ProviderWorkerReadStatus::Data) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		else DrainProviderWorkerTail(process_service, process, result, false);
		process_service.CloseStdioProcessHandles(process);
		result.exit_code = exit_code;
		result.ok = result.error.empty() && result.exit_code == 0;
		return result;
	}
	// Copilot 1.0.103 removed --no-remote and renamed --no-remote-export to --sync-session=off; older CLIs reject the new flag.
	// ponytail: trusts the last version check; a CLI self-updated across 1.0.103 gets the right flags after its next check.
	void UseLegacyCopilotWorkerFlagsBefore103(const uam::AppState& app, std::string_view host_id, std::vector<std::string>& argv)
	{
		const auto state = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(uam::provider_ids::kCopilotCli, host_id));
		if (state == app.runtime_cli_versions_by_provider_id.end() || state->second.installed_version.empty() ||
		    CliProviderVersionAtLeast(state->second.installed_version, "1.0.103"))
		{
			return;
		}
		// Skip "copilot -p <prompt>" so a prompt equal to the flag is never rewritten.
		const auto sync = std::find(argv.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(3, argv.size())), argv.end(), std::string_view("--sync-session=off"));
		if (sync != argv.end())
		{
			*sync = "--no-remote-export";
			argv.insert(sync, "--no-remote");
		}
	}
} // namespace

namespace uam
{
	ProviderWorkerInvocation BuildProviderWorkerInvocation(const AppState& app, const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id, ProviderWorkerPathMode path_mode, std::string* error_out, std::string_view remote_host_id)
	{
		const bool remote_target = !remote_host_id.empty() && remote_host_id != uam::execution_hosts::kLocalHostId;
		if (error_out != nullptr)
		{
			error_out->clear();
		}
		if (!remote_target && !provider_availability::IsEnabled(settings, profile.id))
		{
			SetError(error_out, provider_availability::DisabledReason());
			return {};
		}
		if (const std::string update_error = remote_target ? std::string{} : ProviderCliLaunchBlockReason(app, profile.id); !update_error.empty())
		{
			SetError(error_out, update_error);
			return {};
		}
		if (!remote_target && IsProvider(profile, uam::provider_ids::kCopilotCli))
		{
			if (const std::string compatibility_error = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli).LocalCliCompatibilityError(app); !compatibility_error.empty())
			{
				SetError(error_out, compatibility_error);
				return {};
			}
		}

		const auto& runtime = ProviderRuntimeRegistry::Resolve(profile);
		auto argv = runtime.BuildWorkerArgv(profile, settings, prompt, model_id);
		if (IsProvider(profile, uam::provider_ids::kCopilotCli))
		{
			UseLegacyCopilotWorkerFlagsBefore103(app, remote_host_id, argv);
		}
		if (argv.empty())
		{
			SetError(error_out, "This provider does not support text workers.");
			return {};
		}
		std::string launch_error;
		bool private_isolation = false;
		if (!remote_target && !runtime.PrepareLocalWorkerLaunch(app, profile, argv, launch_error, &private_isolation))
		{
			SetError(error_out, launch_error);
			return {};
		}

		const std::shared_ptr<const std::filesystem::path> isolation_directory = PrepareWorkerIsolationDirectory();
		if (!isolation_directory)
		{
			SetError(error_out, "Failed to prepare the isolated provider worker directory.");
			return {};
		}
		if (!uam::provider_workers::ApplyWorkerIsolationPolicy(profile, *isolation_directory, argv, private_isolation))
		{
			SetError(error_out, "Failed to prepare the provider worker safety policy.");
			return {};
		}
		if (!private_isolation && !uam::provider_workers::RemoveWorkerPromptArgument(profile, argv))
		{
			SetError(error_out, "Provider worker prompt arguments are invalid.");
			return {};
		}

		ProviderWorkerInvocation invocation;
		invocation.direct_process = true;
		invocation.argv = std::move(argv);
		invocation.standard_input = std::string(prompt);
		if (!remote_target) invocation.environment_overrides =
		    uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(profile);
		const std::vector<std::pair<std::string, std::string>> worker_environment =
		    WorkerEnvironment(path_mode);
		if (!remote_target) invocation.environment_overrides.insert(invocation.environment_overrides.end(),
		                                        worker_environment.begin(), worker_environment.end());
		invocation.command_preview = BuildProviderWorkerShellCommand(invocation.argv, path_mode) + " <prompt via stdin>";
		invocation.isolated_working_directory = isolation_directory;
		return invocation;
	}

	ProcessExecutionResult ExecuteProviderWorkerInvocation(const ProviderWorkerInvocation& invocation, const std::filesystem::path& working_directory, int timeout_ms, std::stop_token stop_token)
	{
		if (invocation.Empty() || (invocation.output_sink && !invocation.direct_process))
		{
			ProcessExecutionResult result;
			result.error = invocation.Empty() ? "Provider worker command is empty." : "Provider worker output streaming requires a direct process.";
			return result;
		}
		const std::filesystem::path& actual_working_directory = invocation.isolated_working_directory == nullptr ? working_directory : *invocation.isolated_working_directory;
		ProcessExecutionResult result;
		if (invocation.direct_process)
		{
			result = ExecuteProviderWorkerDirect(invocation, actual_working_directory, timeout_ms, stop_token);
		}
		else
		{
			auto& process_service = PlatformServicesFactory::Instance().process_service;
			const std::string shell_command = process_service.BuildShellCommandWithWorkingDirectory(actual_working_directory, invocation.shell_command);
			result = process_service.ExecuteCommand(shell_command, timeout_ms, stop_token);
		}

		return result;
	}

	ProcessExecutionResult ExecuteIsolatedProviderTextWorker(const ProviderWorkerInvocation& invocation, std::string_view provider_id, const ExecutionHost* remote_host, int timeout_ms, std::stop_token stop_token)
	{
		if (remote_host == nullptr) return ExecuteProviderWorkerInvocation(invocation, {}, timeout_ms, stop_token);
		ProcessExecutionResult result;
		if (stop_token.stop_requested()) { result.canceled = true; return result; }
		if (!uam::remote::CanAttemptRunnerConnection(*remote_host)) { result.error = "The selected remote runner is not ready. Recheck it in Settings."; return result; }
		if (!invocation.direct_process || invocation.argv.empty()) { result.error = "This provider cannot run an isolated remote text worker."; return result; }
		IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
		uam::remote::RunnerClient client(service, *remote_host);
		const std::string id = service.GenerateUuid();
		std::filesystem::path directory;
		if (!client.PrepareTextWorker(id, provider_id, directory, &result.error, stop_token)) return result;
		std::vector<std::string> argv = invocation.argv;
		uam::provider_workers::RemapWorkerPaths(argv, invocation.isolated_working_directory ? *invocation.isolated_working_directory : std::filesystem::path{}, directory, remote_host->platform);
		try { result = client.ExecuteCommand(id, directory, argv, timeout_ms, stop_token, invocation.standard_input); }
		catch (...) { (void)client.RemoveTextWorker(id); throw; }
		std::string cleanup_error;
		if (!client.RemoveTextWorker(id, &cleanup_error) && result.ok) { result.ok = false; result.error = cleanup_error; }
		return result;
	}
} // namespace uam
