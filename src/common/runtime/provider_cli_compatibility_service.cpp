#include "provider_cli_compatibility_service.h"

#include "common/platform/platform_services.h"
#include "common/state/app_state.h"
#include "core/gemini_cli_compat.h"

#include <atomic>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace
{
	constexpr const char* kRuntimeVersionProbeCommand = "gemini --version";

	std::string TrimAscii(const std::string& value)
	{
		const std::size_t start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const std::size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	void ResetAsyncCommandTask(uam::AsyncCommandTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker->detach();
			task.worker.reset();
		}

		task.running = false;
		task.command_preview.clear();
		task.state.reset();
	}

	void StartAsyncCommandTask(uam::AsyncCommandTask& task, const std::string& command)
	{
		ResetAsyncCommandTask(task);
		task.running = true;
		task.command_preview = command;
		task.state = std::make_shared<AsyncProcessTaskState>();
		std::shared_ptr<AsyncProcessTaskState> state = task.state;
		task.worker = std::make_unique<std::jthread>([command, state](std::stop_token stop_token)
		{
			state->result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, -1, stop_token);

			if (!state->result.error.empty() && state->result.output.empty())
			{
				std::ostringstream message;
				message << "Failed to run command: " << command;
				message << "\n\n" << state->result.error;
				state->result.output = message.str();
			}
			else
			{
				if (state->result.output.empty())
				{
					state->result.output = "(Provider CLI returned no output.)";
				}

				if (state->result.timed_out)
				{
					state->result.output += "\n\n[Provider CLI command timed out]";
				}
				else if (state->result.canceled)
				{
					state->result.output += "\n\n[Provider CLI command canceled]";
				}
				else if (state->result.exit_code != 0)
				{
					state->result.output += "\n\n[Provider CLI exited with code " + std::to_string(state->result.exit_code) + "]";
				}
			}

			state->completed.store(true, std::memory_order_release);
		});
	}

	bool TryConsumeAsyncCommandTaskOutput(uam::AsyncCommandTask& task, std::string& output_out)
	{
		if (!task.running)
		{
			return false;
		}

		if (task.state == nullptr)
		{
			ResetAsyncCommandTask(task);
			output_out.clear();
			return true;
		}

		if (!task.state->completed.load(std::memory_order_acquire))
		{
			return false;
		}

		output_out = task.state->result.output;
		ResetAsyncCommandTask(task);
		return true;
	}

	std::optional<std::string> ExtractSemverVersion(const std::string& text)
	{
		static const std::regex semver_pattern(R"((\d+)\.(\d+)\.(\d+))");
		std::smatch match;

		if (std::regex_search(text, match, semver_pattern) && !match.str(0).empty())
		{
			return match.str(0);
		}

		return std::nullopt;
	}

	bool OutputContainsNonZeroExit(const std::string& output)
	{
		return output.find("[Provider CLI exited with code ") != std::string::npos;
	}

} // namespace

void ProviderCliCompatibilityService::StartVersionCheck(uam::AppState& app, const bool force) const
{
	if (app.runtime_cli_version_check_task.running)
	{
		return;
	}

	if (!force && app.runtime_cli_version_checked)
	{
		return;
	}

	StartAsyncCommandTask(app.runtime_cli_version_check_task, kRuntimeVersionProbeCommand);
	app.runtime_cli_version_message = "Checking installed provider CLI version...";
}

void ProviderCliCompatibilityService::StartPinToSupported(uam::AppState& app) const
{
	if (app.runtime_cli_pin_task.running)
	{
		return;
	}

	const std::string command = PlatformServicesFactory::Instance().process_service.GeminiDowngradeCommand();
	StartAsyncCommandTask(app.runtime_cli_pin_task, command);
	app.runtime_cli_pin_output.clear();
	app.status_line = "Running provider CLI update command...";
}

void ProviderCliCompatibilityService::Poll(uam::AppState& app) const
{
	std::string output;

	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_version_check_task, output))
	{
		app.runtime_cli_version_checked = true;
		app.runtime_cli_version_raw_output = output;
		app.runtime_cli_installed_version.clear();
		app.runtime_cli_version_supported = false;

		const std::optional<std::string> parsed = ExtractSemverVersion(output);

		if (parsed.has_value())
		{
			app.runtime_cli_installed_version = parsed.value();
			app.runtime_cli_version_supported = uam::IsSupportedGeminiCliVersion(app.runtime_cli_installed_version);

			if (app.runtime_cli_version_supported)
			{
				app.runtime_cli_version_message = "Provider CLI version is supported.";
			}
			else
			{
				app.runtime_cli_version_message = "Installed provider CLI version is unsupported for this app. Supported Gemini CLI versions: " + uam::SupportedGeminiCliVersionsLabel() + ".";
			}
		}
		else
		{
			const std::string lowered = TrimAscii(output);

			if (lowered.find("not found") != std::string::npos || lowered.find("not recognized") != std::string::npos)
			{
				app.runtime_cli_version_message = "Provider CLI is not installed or not on PATH.";
			}
			else
			{
				app.runtime_cli_version_message = "Could not parse provider CLI version output.";
			}
		}
	}

	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_pin_task, output))
	{
		app.runtime_cli_pin_output = output;

		if (OutputContainsNonZeroExit(output))
		{
			app.status_line = "Provider CLI update command failed. Review output in Settings.";
			app.runtime_cli_version_message = "Update command failed.";
		}
		else
		{
			app.status_line = "Provider CLI update completed. Re-checking installed version.";
			StartVersionCheck(app, true);
		}
	}
}
