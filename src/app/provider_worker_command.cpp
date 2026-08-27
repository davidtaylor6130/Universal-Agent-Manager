#include "app/provider_worker_command.h"

#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/state/app_state.h"
#include "common/utils/io_utils.h"

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

	bool ApplyWorkerIsolationPolicy(const ProviderProfile& profile, const std::filesystem::path& workspace, std::vector<std::string>& argv)
	{
		if (IsProvider(profile, uam::provider_ids::kOpenCodeCli))
		{
			if (!uam::io::WriteTextFile(workspace / "opencode.json", R"({"permission":{"*":"deny","external_directory":"deny"},"share":"disabled","autoupdate":false})"))
			{
				return false;
			}
			argv.insert(argv.begin() + 2, "--pure");
		}
		else if (IsProvider(profile, uam::provider_ids::kGeminiCli))
		{
			const std::filesystem::path policy_file = workspace / "deny-all-tools.toml";
			if (!uam::io::WriteTextFile(policy_file, R"([[rule]]
toolName = "*"
decision = "deny"
priority = 999999

[[rule]]
mcpName = "*"
decision = "deny"
priority = 999999
)"))
			{
				return false;
			}
			argv.insert(argv.begin() + 1, {
			                                  "--approval-mode",
			                                  "plan",
			                                  "--admin-policy",
			                                  uam::paths::Utf8PathString(policy_file),
			                              });
		}
		return true;
	}

	bool RemoveWorkerPromptArgument(const ProviderProfile& profile, std::vector<std::string>& argv)
	{
		if (IsProvider(profile, uam::provider_ids::kGeminiCli) || IsProvider(profile, uam::provider_ids::kCopilotCli))
		{
			const auto prompt_flag = std::ranges::find(argv, "-p");
			if (prompt_flag == argv.end() || std::next(prompt_flag) == argv.end()) return false;
			argv.erase(prompt_flag, std::next(prompt_flag, 2));
			return true;
		}
		if (IsProvider(profile, uam::provider_ids::kClaudeCli))
		{
			if (argv.size() < 2 || argv[argv.size() - 2] != "--") return false;
			argv.erase(argv.end() - 2, argv.end());
			return true;
		}
		if (IsProvider(profile, uam::provider_ids::kCodexCli) || IsProvider(profile, uam::provider_ids::kOpenCodeCli))
		{
			if (argv.size() < 2) return false;
			argv.pop_back();
			return true;
		}
		return false;
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

	ProviderWorkerReadStatus ReadProviderWorkerOutput(IPlatformProcessService& process_service, uam::platform::StdioProcessPlatformFields& process, ProcessExecutionResult& result, bool preserve_existing_error)
	{
		std::array<char, 4096> buffer{};
		std::string read_error;
		const std::ptrdiff_t bytes_read = process_service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &read_error);
		if (bytes_read > 0)
		{
			if (!uam::platform::AppendCapturedCommandOutput(result, buffer.data(), static_cast<std::size_t>(bytes_read)))
			{
				result.error = std::string(uam::platform::kCapturedCommandOutputLimitError);
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

	ProcessExecutionResult ExecuteProviderWorkerDirect(const uam::ProviderWorkerInvocation& invocation, const std::filesystem::path& working_directory, int timeout_ms, std::stop_token stop_token)
	{
		ProcessExecutionResult result;
		auto& process_service = PlatformServicesFactory::Instance().process_service;
		uam::platform::StdioProcessPlatformFields process;
		if (!process_service.StartStdioProcessWithInput(process, working_directory, invocation.argv, invocation.standard_input, &result.error, invocation.environment_overrides))
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
		if (IsProvider(profile, uam::provider_ids::kCopilotCli))
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

		const std::shared_ptr<const std::filesystem::path> isolation_directory = PrepareWorkerIsolationDirectory();
		if (!isolation_directory)
		{
			SetError(error_out, "Failed to prepare the isolated provider worker directory.");
			return {};
		}
		if (!ApplyWorkerIsolationPolicy(profile, *isolation_directory, argv))
		{
			SetError(error_out, "Failed to prepare the provider worker safety policy.");
			return {};
		}
		if (!RemoveWorkerPromptArgument(profile, argv))
		{
			SetError(error_out, "Provider worker prompt arguments are invalid.");
			return {};
		}

		ProviderWorkerInvocation invocation;
		invocation.direct_process = true;
		invocation.argv = std::move(argv);
		invocation.standard_input = std::string(prompt);
		invocation.environment_overrides =
		    uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(profile);
		const std::vector<std::pair<std::string, std::string>> worker_environment =
		    WorkerEnvironment(path_mode);
		invocation.environment_overrides.insert(invocation.environment_overrides.end(),
		                                        worker_environment.begin(), worker_environment.end());
		invocation.command_preview = BuildProviderWorkerShellCommand(invocation.argv, path_mode) + " <prompt via stdin>";
		invocation.isolated_working_directory = isolation_directory;
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
} // namespace uam
