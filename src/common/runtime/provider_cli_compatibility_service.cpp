#include "provider_cli_compatibility_service.h"

#include "common/constants/app_constants.h"
#include "common/platform/platform_services.h"
#include "common/state/app_state.h"

#include <atomic>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
	constexpr const char* kRuntimeVersionProbeCommand = "gemini --version";
	constexpr const char* kSupportedRuntimeCliVersion = uam::constants::kSupportedGeminiVersion;

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

	std::string ExecuteCommandCaptureOutput(const std::string& command)
	{
		std::string output;
		int raw_status = -1;
		std::string error_message;
		auto& process_service = PlatformServicesFactory::Instance().process_service;
		const bool captured = process_service.CaptureCommandOutput(command, &output, &raw_status, &error_message);

		if (!captured)
		{
			std::ostringstream message;
			message << "Failed to run command: " << command;

			if (!error_message.empty())
			{
				message << "\n\n" << error_message;
			}

			return message.str();
		}

		const int exit_code = process_service.NormalizeCapturedCommandExitCode(raw_status);

		if (output.empty())
		{
			output = "(Provider CLI returned no output.)";
		}

		if (exit_code != 0)
		{
			output += "\n\n[Provider CLI exited with code " + std::to_string(exit_code) + "]";
		}

		return output;
	}

	void StartAsyncCommandTask(uam::AsyncCommandTask& task, const std::string& command)
	{
		task.running = true;
		task.command_preview = command;
		task.completed = std::make_shared<std::atomic<bool>>(false);
		task.output = std::make_shared<std::string>();
		std::shared_ptr<std::atomic<bool>> completed = task.completed;
		std::shared_ptr<std::string> output = task.output;
		auto run_command_task = [command, completed, output]()
		{
			*output = ExecuteCommandCaptureOutput(command);
			completed->store(true, std::memory_order_release);
		};

		std::thread(run_command_task).detach();
	}

	bool TryConsumeAsyncCommandTaskOutput(uam::AsyncCommandTask& task, std::string& output_out)
	{
		if (!task.running)
		{
			return false;
		}

		if (task.completed == nullptr || task.output == nullptr)
		{
			task.running = false;
			task.command_preview.clear();
			task.completed.reset();
			task.output.reset();
			output_out.clear();
			return true;
		}

		if (!task.completed->load(std::memory_order_acquire))
		{
			return false;
		}

		output_out = *task.output;
		task.running = false;
		task.command_preview.clear();
		task.completed.reset();
		task.output.reset();
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

std::string ProviderCliCompatibilityService::BuildVersionCheckCommand() const
{
	return kRuntimeVersionProbeCommand;
}

std::string ProviderCliCompatibilityService::BuildPinCommand() const
{
	return PlatformServicesFactory::Instance().process_service.GeminiDowngradeCommand();
}

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

	StartAsyncCommandTask(app.runtime_cli_version_check_task, BuildVersionCheckCommand());
	app.runtime_cli_version_message = "Checking installed provider CLI version...";
}

void ProviderCliCompatibilityService::StartPinToSupported(uam::AppState& app) const
{
	if (app.runtime_cli_pin_task.running)
	{
		return;
	}

	const std::string command = BuildPinCommand();
	StartAsyncCommandTask(app.runtime_cli_pin_task, command);
	app.runtime_cli_pin_output.clear();
	app.status_line = "Running provider CLI pin command...";
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
			app.runtime_cli_version_supported = (app.runtime_cli_installed_version == kSupportedRuntimeCliVersion);

			if (app.runtime_cli_version_supported)
			{
				app.runtime_cli_version_message = "Provider CLI version is supported.";
			}
			else
			{
				app.runtime_cli_version_message = "Installed provider CLI version is unsupported for this app.";
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
			app.status_line = "Provider CLI pin command failed. Review output in Settings.";
			app.runtime_cli_version_message = "Downgrade command failed.";
		}
		else
		{
			app.status_line = "Provider CLI pin completed. Re-checking installed version.";
			StartVersionCheck(app, true);
		}
	}
}

ProviderCliCompatibilityService& GetProviderCliCompatibilityService()
{
	static ProviderCliCompatibilityService service;
	return service;
}
