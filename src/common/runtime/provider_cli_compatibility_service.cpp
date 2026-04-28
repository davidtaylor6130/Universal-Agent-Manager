#include "provider_cli_compatibility_service.h"

#include "common/platform/platform_services.h"
#include "common/provider/provider_profile.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/state/app_state.h"
#include "core/gemini_cli_compat.h"

#include <atomic>
#include <algorithm>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	constexpr const char* kRuntimeVersionProbeCommand = "gemini --version";
	constexpr const char* kGeminiProviderId = "gemini-cli";
	constexpr const char* kCodexProviderId = "codex-cli";
	constexpr const char* kGeminiNpmPackage = "@google/gemini-cli";
	constexpr const char* kCodexNpmPackage = "@openai/codex";
	constexpr const char* kCodexPreferredVersion = "0.124.0";
	constexpr const char* kCodexFallbackVersion = "0.123.0";

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
		task.worker = std::make_unique<std::jthread>(
		    [command, state](std::stop_token stop_token)
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

	bool IsSafeVersionToken(const std::string& value)
	{
		if (value.empty() || value.size() > 80 || value.front() == '-')
		{
			return false;
		}
		for (const char ch : value)
		{
			const bool safe =
			    (ch >= 'a' && ch <= 'z') ||
			    (ch >= 'A' && ch <= 'Z') ||
			    (ch >= '0' && ch <= '9') ||
			    ch == '.' ||
			    ch == '_' ||
			    ch == '-';
			if (!safe)
			{
				return false;
			}
		}
		return true;
	}

	std::string ProviderTitle(const uam::AppState& app, const std::string& provider_id)
	{
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, provider_id); profile != nullptr && !profile->title.empty())
		{
			return profile->title;
		}
		if (provider_id == kCodexProviderId)
		{
			return "Codex CLI";
		}
		return "Gemini CLI";
	}

	bool ProviderHasActiveRuntimeWork(const uam::AppState& app, const std::string& provider_id)
	{
		for (const auto& session : app.acp_sessions)
		{
			if (session != nullptr &&
			    session->provider_id == provider_id &&
			    (session->processing ||
			     session->waiting_for_permission ||
			     session->waiting_for_user_input ||
			     session->initialize_request_id != 0 ||
			     session->session_setup_request_id != 0 ||
			     session->prompt_request_id != 0 ||
			     session->cancel_request_id != 0))
			{
				return true;
			}
		}

		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal == nullptr || !terminal->running || !CliTerminalLifecycleIsProcessing(*terminal))
			{
				continue;
			}

			const std::string chat_id = terminal->attached_chat_id.empty() ? terminal->frontend_chat_id : terminal->attached_chat_id;
			const auto chat_it = std::find_if(app.chats.begin(), app.chats.end(), [&chat_id](const ChatSession& chat) { return chat.id == chat_id; });
			if (chat_it != app.chats.end() && chat_it->provider_id == provider_id)
			{
				return true;
			}
		}

		return false;
	}

	std::string NormalizeProviderId(const std::string& provider_id)
	{
		const std::string trimmed = TrimAscii(provider_id);
		if (trimmed == kCodexProviderId)
		{
			return kCodexProviderId;
		}
		return kGeminiProviderId;
	}

} // namespace

void ProviderCliCompatibilityService::StartVersionCheck(uam::AppState& app, const bool force) const
{
	StartProviderVersionCheck(app, kGeminiProviderId, force);
}

void ProviderCliCompatibilityService::StartProviderVersionCheck(uam::AppState& app, const std::string& provider_id, const bool force) const
{
	const std::string normalized_provider_id = NormalizeProviderId(provider_id);
	if (app.runtime_cli_version_check_task.running)
	{
		return;
	}

	if (!force && app.runtime_cli_version_checked && app.runtime_cli_version_provider_id == normalized_provider_id)
	{
		return;
	}

	const std::string command = VersionProbeCommandForProvider(normalized_provider_id);
	if (command.empty())
	{
		app.runtime_cli_version_message = "Provider CLI version checks are not supported for this provider.";
		return;
	}

	app.runtime_cli_version_provider_id = normalized_provider_id;
	StartAsyncCommandTask(app.runtime_cli_version_check_task, command);
	app.runtime_cli_version_message = "Checking installed " + ProviderTitle(app, normalized_provider_id) + " version...";
}

void ProviderCliCompatibilityService::StartPinToSupported(uam::AppState& app) const
{
	std::string error;
	(void)StartInstallProviderVersion(app, kGeminiProviderId, PreferredVersionForProvider(kGeminiProviderId), &error);
	if (!error.empty())
	{
		app.status_line = error;
	}
}

bool ProviderCliCompatibilityService::StartInstallProviderVersion(uam::AppState& app, const std::string& provider_id, const std::string& version, std::string* error_out) const
{
	const std::string normalized_provider_id = NormalizeProviderId(provider_id);
	const std::string trimmed_version = TrimAscii(version);
	if (app.runtime_cli_pin_task.running)
	{
		if (error_out != nullptr)
		{
			*error_out = "A provider CLI install is already running.";
		}
		return false;
	}
	if (app.runtime_cli_version_check_task.running)
	{
		if (error_out != nullptr)
		{
			*error_out = "A provider CLI version check is already running.";
		}
		return false;
	}
	if (ProviderProfileStore::FindById(app.provider_profiles, normalized_provider_id) == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Unsupported provider: " + normalized_provider_id;
		}
		return false;
	}
	if (!IsSupportedVersionForProvider(normalized_provider_id, trimmed_version))
	{
		if (error_out != nullptr)
		{
			*error_out = "Unsupported CLI version: " + trimmed_version;
		}
		return false;
	}
	if (ProviderHasActiveRuntimeWork(app, normalized_provider_id))
	{
		if (error_out != nullptr)
		{
			*error_out = "Cannot install a provider CLI version while that provider is processing.";
		}
		return false;
	}

	const std::string command = InstallCommandForProviderVersion(normalized_provider_id, trimmed_version);
	if (command.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Provider CLI installs are not supported for this provider.";
		}
		return false;
	}

	app.runtime_cli_pin_provider_id = normalized_provider_id;
	app.runtime_cli_selected_version = trimmed_version;
	StartAsyncCommandTask(app.runtime_cli_pin_task, command);
	app.runtime_cli_pin_output.clear();
	app.status_line = "Running " + ProviderTitle(app, normalized_provider_id) + " install command...";
	return true;
}

void ProviderCliCompatibilityService::Poll(uam::AppState& app) const
{
	std::string output;

	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_version_check_task, output))
	{
		const std::string provider_id = NormalizeProviderId(app.runtime_cli_version_provider_id);
		app.runtime_cli_version_checked = true;
		app.runtime_cli_version_raw_output = output;
		app.runtime_cli_installed_version.clear();
		app.runtime_cli_version_supported = false;

		const std::optional<std::string> parsed = ExtractSemverVersion(output);

		if (parsed.has_value())
		{
			app.runtime_cli_installed_version = parsed.value();
			app.runtime_cli_version_supported = IsSupportedVersionForProvider(provider_id, app.runtime_cli_installed_version);

			if (app.runtime_cli_version_supported)
			{
				app.runtime_cli_version_message = ProviderTitle(app, provider_id) + " version is supported.";
			}
			else
			{
				app.runtime_cli_version_message = "Installed " + ProviderTitle(app, provider_id) + " version is not in the curated supported list.";
			}
		}
		else
		{
			const std::string lowered = TrimAscii(output);

			if (lowered.find("not found") != std::string::npos || lowered.find("not recognized") != std::string::npos)
			{
				app.runtime_cli_version_message = ProviderTitle(app, provider_id) + " is not installed or not on PATH.";
			}
			else
			{
				app.runtime_cli_version_message = "Could not parse " + ProviderTitle(app, provider_id) + " version output.";
			}
		}
	}

	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_pin_task, output))
	{
		const std::string provider_id = NormalizeProviderId(app.runtime_cli_pin_provider_id);
		app.runtime_cli_pin_output = output;

		if (OutputContainsNonZeroExit(output))
		{
			app.status_line = "Provider CLI update command failed. Review output in Settings.";
			app.runtime_cli_version_message = "Update command failed.";
		}
		else
		{
			app.status_line = "Provider CLI update completed. Re-checking installed version.";
			StartProviderVersionCheck(app, provider_id, true);
		}
	}
}

std::vector<CliProviderVersionOption> ProviderCliCompatibilityService::SupportedVersionsForProvider(const std::string& provider_id) const
{
	const std::string normalized_provider_id = NormalizeProviderId(provider_id);
	std::vector<CliProviderVersionOption> versions;
	if (normalized_provider_id == kCodexProviderId)
	{
		versions.push_back({kCodexPreferredVersion, true});
		versions.push_back({kCodexFallbackVersion, false});
		return versions;
	}

	for (const std::string_view version : uam::SupportedGeminiCliVersions())
	{
		const std::string text(version);
		versions.push_back({text, text == uam::PreferredGeminiCliVersion()});
	}
	return versions;
}

std::string ProviderCliCompatibilityService::PreferredVersionForProvider(const std::string& provider_id) const
{
	const std::string normalized_provider_id = NormalizeProviderId(provider_id);
	if (normalized_provider_id == kCodexProviderId)
	{
		return kCodexPreferredVersion;
	}
	return std::string(uam::PreferredGeminiCliVersion());
}

bool ProviderCliCompatibilityService::IsSupportedVersionForProvider(const std::string& provider_id, const std::string& version) const
{
	const std::string normalized_provider_id = NormalizeProviderId(provider_id);
	const std::string trimmed_version = TrimAscii(version);
	if (!IsSafeVersionToken(trimmed_version))
	{
		return false;
	}
	if (normalized_provider_id == kCodexProviderId)
	{
		return trimmed_version == kCodexPreferredVersion || trimmed_version == kCodexFallbackVersion;
	}
	return uam::IsSupportedGeminiCliVersion(trimmed_version);
}

std::string ProviderCliCompatibilityService::VersionProbeCommandForProvider(const std::string& provider_id) const
{
	const std::string normalized_provider_id = NormalizeProviderId(provider_id);
	if (normalized_provider_id == kCodexProviderId)
	{
		return "codex --version";
	}
	return kRuntimeVersionProbeCommand;
}

std::string ProviderCliCompatibilityService::InstallCommandForProviderVersion(const std::string& provider_id, const std::string& version) const
{
	const std::string normalized_provider_id = NormalizeProviderId(provider_id);
	const std::string trimmed_version = TrimAscii(version);
	if (!IsSupportedVersionForProvider(normalized_provider_id, trimmed_version))
	{
		return "";
	}
	const std::string package_name = normalized_provider_id == kCodexProviderId ? kCodexNpmPackage : kGeminiNpmPackage;
	return "npm install -g " + package_name + "@" + trimmed_version;
}

std::string BuildCliProviderVersionProbeCommandForTests(const std::string& provider_id)
{
	return ProviderCliCompatibilityService().VersionProbeCommandForProvider(provider_id);
}

std::string BuildCliProviderInstallCommandForTests(const std::string& provider_id, const std::string& version)
{
	return ProviderCliCompatibilityService().InstallCommandForProviderVersion(provider_id, version);
}
