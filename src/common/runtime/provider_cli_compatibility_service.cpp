#include "provider_cli_compatibility_service.h"

#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "common/platform/platform_services.h"
#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "remote/runner_client.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/state/app_state.h"
#include "common/utils/range_utils.h"
#include "common/utils/base64.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	constexpr const char* kLatestVersion = "latest";
	constexpr const char* kCommandFailurePrefix = "Failed to run command: ";
	constexpr const char* kProviderCliTimedOutSuffix = "\n\n[Provider CLI command timed out]";
	constexpr const char* kProviderCliCanceledSuffix = "\n\n[Provider CLI command canceled]";
	constexpr const char* kProviderCliFailureMarker = "[Provider CLI command failed: ";
	constexpr const char* kProviderCliExitCodeMarker = "[Provider CLI exited with code ";
	constexpr int kProviderCliVersionProbeTimeoutMs = 30000;
	constexpr int kProviderCliInstallTimeoutMs = 15 * 60 * 1000;
	constexpr std::string_view kProviderCliPathMarker = "[UAM CLI PATH] ";
	constexpr auto kSafeVersionTokenPunctuation = std::to_array<char>({
	    '.',
	    '_',
	    '-',
	});

	struct ResolvedProviderCliPolicy
	{
		std::string provider_id;
		const ProviderCliPolicy& policy;
	};

	struct OptionalProviderCliPolicy
	{
		std::string provider_id;
		const ProviderCliPolicy* policy = nullptr;
	};

	const ProviderCliPolicy* FindProviderCliPolicy(std::string_view provider_id)
	{
		return ProviderRuntimeRegistry::ResolveById(provider_id).CliVersionPolicy();
	}

	const ProviderCliPolicy& ProviderCliPolicyOrUnsupported(std::string_view provider_id)
	{
		if (const ProviderCliPolicy* policy = FindProviderCliPolicy(provider_id))
		{
			return *policy;
		}
		static constexpr ProviderCliPolicy unsupported;
		return unsupported;
	}

	std::string LocalPlatform()
	{
#if defined(_WIN32)
		return "windows";
#elif defined(__APPLE__)
		return "macos";
#else
		return "linux";
#endif
	}

	bool IsReadyCliHost(const ExecutionHost& host)
	{
		return host.id == uam::execution_hosts::kLocalHostId ||
		       (host.transport == "ssh" && host.runner_status == "ready" &&
		        host.runner_protocol_version >= 3 && !host.runner_version.empty() &&
		        uam::execution_hosts::IsPortableId(host.id) &&
		        uam::execution_hosts::IsSafeSshAlias(host.ssh_alias) &&
		        uam::execution_hosts::IsSafeRunnerDirectory(host.runner_directory) &&
		        (host.platform == "windows" || host.platform == "macos" || host.platform == "linux"));
	}

	std::string BuildInstallAwareProbeCommand(const ProviderCliPolicy& policy, std::string_view platform);
	bool ValidateRemoteInstallProbe(std::string_view previous, std::string_view current, std::string_view platform, std::string* error);

	ProcessExecutionResult RunCliCommand(const ExecutionHost& host, const std::string& provider_id,
	    const std::string& command, bool installing, int timeout_ms, std::stop_token stop_token,
	    const std::string& previous_probe, std::optional<bool>* remote_connected_out)
	{
		if (remote_connected_out != nullptr) *remote_connected_out = std::nullopt;
		IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
		if (host.id == uam::execution_hosts::kLocalHostId)
			return service.ExecuteCommand(command, timeout_ms, stop_token);
		ProcessExecutionResult result;
		if (stop_token.stop_requested())
		{
			result.canceled = true;
			result.error = "Remote CLI command was canceled.";
			return result;
		}
		uam::remote::RunnerClient client(service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version,
		        host.runner_directory, host.runner_protocol_version),
		    host.runner_version, host.runner_protocol_version);
		const auto finish = [&]() -> ProcessExecutionResult
		{
			if (remote_connected_out != nullptr && !result.canceled)
				*remote_connected_out = client.IsConnected();
			return std::move(result);
		};
		uam::remote::DirectoryListing home;
		if (!client.ListDirectories({}, home, &result.error)) return finish();
		if (!uam::execution_hosts::IsAbsoluteRemotePath(host.platform, home.directory))
		{
			result.error = "The SSH helper returned an invalid home directory.";
			return finish();
		}
		if (installing)
		{
			const ProviderCliPolicy* policy = FindProviderCliPolicy(provider_id);
			if (policy == nullptr) { result.error = "The provider update policy is unavailable."; return finish(); }
			const std::string probe_command = BuildInstallAwareProbeCommand(*policy, host.platform);
			const std::vector<std::string> probe_argv = host.platform == "windows"
			    ? std::vector<std::string>{"cmd.exe", "/d", "/s", "/c", probe_command}
			    : std::vector<std::string>{"sh", "-lc", probe_command};
			result = client.ExecuteCommand("cli-check-" + service.GenerateUuid(), uam::paths::PathFromUtf8(home.directory), probe_argv, kProviderCliVersionProbeTimeoutMs, stop_token);
			if (!result.ok || result.exit_code != 0 || result.output_truncated || stop_token.stop_requested())
			{
				if (result.error.empty()) result.error = "Could not recheck the installed CLI. No update was started.";
				result.ok = false;
				return finish();
			}
			if (!ValidateRemoteInstallProbe(previous_probe, result.output, host.platform, &result.error))
			{
				result.ok = false;
				return finish();
			}
		}
		const std::vector<std::string> argv = host.platform == "windows"
		    ? std::vector<std::string>{"cmd.exe", "/d", "/s", "/c", command}
		    : std::vector<std::string>{"sh", "-lc", command};
		// The fixed install ID excludes a second installer while an uncertain old lease remains.
		const std::string process_id = installing ? "cli-update-" + provider_id : "cli-check-" + service.GenerateUuid();
		result = client.ExecuteCommand(process_id, uam::paths::PathFromUtf8(home.directory), argv, timeout_ms, stop_token);
		return finish();
	}

	void StartAsyncCommandTask(uam::AsyncCommandTask& task, const ExecutionHost& host, const std::string& provider_id, const std::string& command, bool installing, int timeout_ms, const std::string& previous_probe = {})
	{
		uam::ResetAsyncCommandTask(task);
		task.running = true;
		task.execution_host = host;
		task.command_preview = command;
		task.state = std::make_shared<AsyncProcessTaskState>();
		std::shared_ptr<AsyncProcessTaskState> state = task.state;
		task.worker = std::make_unique<std::jthread>(
		    [host, provider_id, command, installing, timeout_ms, previous_probe, state](std::stop_token stop_token)
		    {
			    state->result = RunCliCommand(host, provider_id, command, installing, timeout_ms,
			        stop_token, previous_probe, &state->remote_helper_connected);

			    if (!state->result.error.empty() && state->result.output.empty())
			    {
				    std::string message;
				    message.reserve(std::string_view(kCommandFailurePrefix).size() + command.size() + 2 + state->result.error.size());
				    message.append(kCommandFailurePrefix);
				    message.append(command);
				    message.append("\n\n");
				    message.append(state->result.error);
				    state->result.output = std::move(message);
			    }
			    else
			    {
				    if (state->result.output.empty())
				    {
					    state->result.output = "(Provider CLI returned no output.)";
				    }

				    if (state->result.timed_out)
				    {
					    state->result.output += kProviderCliTimedOutSuffix;
				    }
				    else if (state->result.canceled)
				    {
					    state->result.output += kProviderCliCanceledSuffix;
				    }
				    else if (!state->result.error.empty())
				    {
					    state->result.output += "\n\n" + std::string(kProviderCliFailureMarker) + state->result.error + "]";
				    }
				    else if (state->result.exit_code != 0)
				    {
					    state->result.output += "\n\n" + std::string(kProviderCliExitCodeMarker) + std::to_string(state->result.exit_code) + "]";
				    }
			    }

			    state->completed.store(true, std::memory_order_release);
		    });
	}

	bool TryConsumeAsyncCommandTaskOutput(uam::AsyncCommandTask& task, std::string& output_out,
	    std::optional<bool>* remote_connected_out)
	{
		if (remote_connected_out != nullptr) *remote_connected_out = std::nullopt;
		if (!task.running)
		{
			return false;
		}

		if (task.state == nullptr)
		{
			uam::ResetAsyncCommandTask(task);
			output_out.clear();
			return true;
		}

		if (!task.state->completed.load(std::memory_order_acquire))
		{
			return false;
		}

		output_out = std::move(task.state->result.output);
		if (remote_connected_out != nullptr)
			*remote_connected_out = task.state->remote_helper_connected;
		uam::ResetAsyncCommandTask(task);
		return true;
	}

	bool ConsumeDigits(std::string_view text, std::size_t& offset)
	{
		const std::size_t start = offset;
		while (offset < text.size() && uam::strings::IsAsciiDigit(static_cast<unsigned char>(text[offset])))
		{
			++offset;
		}

		return offset > start;
	}

	bool ConsumeDot(std::string_view text, std::size_t& offset)
	{
		if (offset >= text.size() || text[offset] != '.')
		{
			return false;
		}

		++offset;
		return true;
	}

	std::optional<std::string> ExtractSemverVersion(std::string_view text)
	{
		for (std::size_t start = 0; start < text.size(); ++start)
		{
			if (!uam::strings::IsAsciiDigit(static_cast<unsigned char>(text[start])))
			{
				continue;
			}

			std::size_t end = start;
			const bool has_semver = ConsumeDigits(text, end) && ConsumeDot(text, end) && ConsumeDigits(text, end) && ConsumeDot(text, end) && ConsumeDigits(text, end);
			if (has_semver)
			{
				return std::string(text.substr(start, end - start));
			}
		}

		return std::nullopt;
	}

	int CompareSemverComponent(std::string_view lhs, std::string_view rhs)
	{
		lhs.remove_prefix(std::min(lhs.find_first_not_of('0'), lhs.size()));
		rhs.remove_prefix(std::min(rhs.find_first_not_of('0'), rhs.size()));
		if (lhs.size() != rhs.size())
		{
			return lhs.size() < rhs.size() ? -1 : 1;
		}
		return lhs.compare(rhs);
	}

	bool SemverAtLeast(std::string_view version, std::string_view minimum)
	{
		const std::optional<std::string> parsed_version = ExtractSemverVersion(version);
		const std::optional<std::string> parsed_minimum = ExtractSemverVersion(minimum);
		if (!parsed_version || !parsed_minimum)
		{
			return false;
		}

		std::size_t version_start = 0;
		std::size_t minimum_start = 0;
		for (int component = 0; component < 3; ++component)
		{
			const std::size_t version_end = parsed_version->find('.', version_start);
			const std::size_t minimum_end = parsed_minimum->find('.', minimum_start);
			const int comparison = CompareSemverComponent(std::string_view(*parsed_version).substr(version_start, version_end - version_start), std::string_view(*parsed_minimum).substr(minimum_start, minimum_end - minimum_start));
			if (comparison != 0)
			{
				return comparison > 0;
			}
			version_start = version_end == std::string::npos ? parsed_version->size() : version_end + 1;
			minimum_start = minimum_end == std::string::npos ? parsed_minimum->size() : minimum_end + 1;
		}
		return true;
	}

	bool OutputContainsNonZeroExit(std::string_view output)
	{
		return uam::strings::Contains(output, kProviderCliExitCodeMarker);
	}

	bool OutputIndicatesCommandFailure(std::string_view output)
	{
		return uam::strings::StartsWith(output, kCommandFailurePrefix) || OutputContainsNonZeroExit(output) || uam::strings::Contains(output, kProviderCliTimedOutSuffix) || uam::strings::Contains(output, kProviderCliCanceledSuffix) || uam::strings::Contains(output, kProviderCliFailureMarker);
	}

	bool OutputIndicatesCommandMissing(std::string_view output)
	{
		constexpr auto kMissingCommandNeedles = std::to_array<std::string_view>({
		    "not found",
		    "not recognized",
		    "no such file or directory",
		});
		return uam::strings::ContainsAnyCaseInsensitive(output, kMissingCommandNeedles);
	}

	std::string ProviderTitleMessage(std::string_view prefix, std::string_view provider_title, std::string_view suffix)
	{
		std::string message;
		message.reserve(prefix.size() + provider_title.size() + suffix.size());
		message.append(prefix);
		message.append(provider_title);
		message.append(suffix);
		return message;
	}

	std::string UnparsedVersionOutputMessage(std::string_view provider_title, std::string_view output)
	{
		if (OutputIndicatesCommandMissing(output))
		{
			return ProviderTitleMessage("", provider_title, " is not installed or not on PATH.");
		}
		return ProviderTitleMessage("Could not parse ", provider_title, " version output.");
	}

	bool FailProviderCliInstall(std::string* error_out, std::string message)
	{
		if (error_out != nullptr)
		{
			*error_out = std::move(message);
		}
		return false;
	}

	bool IsSafeVersionToken(std::string_view value)
	{
		if (value.empty() || value.size() > 80 || value.front() == '-')
		{
			return false;
		}
		return std::ranges::all_of(value, [](char ch) {
			return uam::strings::IsAsciiAlnum(static_cast<unsigned char>(ch)) ||
			       uam::ranges::Contains(kSafeVersionTokenPunctuation, ch);
		});
	}

	ResolvedProviderCliPolicy ResolveProviderCliPolicy(std::string_view provider_id)
	{
		const std::string normalized_provider_id = uam::provider_ids::CanonicalCliProviderLookupId(provider_id);
		return {normalized_provider_id, ProviderCliPolicyOrUnsupported(normalized_provider_id)};
	}

	OptionalProviderCliPolicy ResolveKnownProviderCliPolicy(std::string_view provider_id)
	{
		const std::string normalized_provider_id = uam::provider_ids::NormalizeCliProviderAlias(provider_id);
		if (normalized_provider_id.empty())
		{
			return {uam::provider_ids::CanonicalCliProviderLookupId(provider_id), nullptr};
		}

		return {normalized_provider_id, FindProviderCliPolicy(normalized_provider_id)};
	}

	std::string BuildNpmGlobalInstallCommand(std::string_view package_name, std::string_view version)
	{
		constexpr std::string_view kNpmGlobalInstallPrefix = "npm install -g ";
		std::string command;
		command.reserve(kNpmGlobalInstallPrefix.size() + package_name.size() + 1 + version.size());
		command.append(kNpmGlobalInstallPrefix);
		command.append(package_name);
		command.push_back('@');
		command.append(version);
		return command;
	}

	std::string BuildHomebrewUpgradeCommand(const ProviderCliPolicy& policy)
	{
		if (policy.homebrew_package.empty())
		{
			return "";
		}
		return policy.homebrew_cask
		           ? "brew upgrade --cask " + std::string(policy.homebrew_package)
		           : "brew upgrade " + std::string(policy.homebrew_package);
	}

	std::string BuildWingetUpgradeCommand(const ProviderCliPolicy& policy, std::string_view version)
	{
		if (policy.winget_package.empty())
		{
			return "";
		}
		std::string command = "winget upgrade --id " + std::string(policy.winget_package) + " --exact --source winget --accept-source-agreements --accept-package-agreements";
		if (version != kLatestVersion)
		{
			command += " --version " + std::string(version);
		}
		return command;
	}

	std::string BuildInstallCommand(const ProviderCliPolicy& policy, std::string_view version, std::string_view install_method)
	{
		if (install_method == "homebrew-formula" || install_method == "homebrew-cask")
		{
			return BuildHomebrewUpgradeCommand(policy);
		}
		if (install_method == "winget")
		{
			return BuildWingetUpgradeCommand(policy, version);
		}
		return BuildNpmGlobalInstallCommand(policy.npm_package, version);
	}

	std::string InstallMethodFromProbeOutput(std::string_view output, bool remote = false)
	{
		const std::size_t marker = output.find(kProviderCliPathMarker);
		if (marker == std::string_view::npos)
		{
			return remote ? "unknown" : "npm";
		}
		const std::size_t line_end = output.find('\n', marker);
		const std::string_view path_line = output.substr(marker, line_end == std::string_view::npos ? output.size() - marker : line_end - marker);
		if (uam::strings::Contains(path_line, "Caskroom/"))
		{
			return "homebrew-cask";
		}
		if (uam::strings::Contains(path_line, "Cellar/"))
		{
			return "homebrew-formula";
		}
		if (uam::strings::ContainsCaseInsensitive(path_line, "\\WinGet\\") || uam::strings::ContainsCaseInsensitive(path_line, "/WinGet/"))
		{
			return "winget";
		}
		return !remote || uam::strings::Contains(path_line, "node_modules/") || uam::strings::Contains(path_line, "node_modules\\") ? "npm" : "unknown";
	}

	std::string_view ProbeIdentity(std::string_view output)
	{
		const std::size_t marker = output.find(kProviderCliPathMarker);
		if (marker == std::string_view::npos) return {};
		const std::size_t begin = marker + kProviderCliPathMarker.size();
		return uam::strings::TrimAsciiView(output.substr(begin, output.find('\n', begin) - begin));
	}

	bool ValidateRemoteInstallProbe(std::string_view previous, std::string_view current, std::string_view platform, std::string* error)
	{
		const std::string_view identity = ProbeIdentity(previous);
		const std::size_t separator = identity.find('|');
		const std::string method = InstallMethodFromProbeOutput(previous, true);
		if (identity.empty() || separator == std::string_view::npos ||
		    !uam::execution_hosts::IsAbsoluteRemotePath(platform, identity.substr(0, separator)) ||
		    method == "unknown" || identity != ProbeIdentity(current) ||
		    method != InstallMethodFromProbeOutput(current, true) ||
		    OutputIndicatesCommandFailure(current))
		{
			return FailProviderCliInstall(error, "The CLI installation changed or could not be identified. Check this machine again before updating. No update was started.");
		}
		return true;
	}

	std::string StripProbePathLine(std::string output)
	{
		for (std::size_t marker = output.find(kProviderCliPathMarker); marker != std::string::npos; marker = output.find(kProviderCliPathMarker))
		{
			const std::size_t line_end = output.find('\n', marker);
			output.erase(marker, line_end == std::string::npos ? output.size() - marker : line_end - marker + 1);
		}
		return output;
	}

	std::string BuildInstallAwareProbeCommand(const ProviderCliPolicy& policy, std::string_view platform)
	{
		if (policy.version_probe_command.empty() || policy.executable_name.empty())
		{
			return "";
		}
		if (platform != "windows")
		{
		std::string command = "uam_cli_path=$(command -v " + std::string(policy.executable_name) + " 2>/dev/null || true); ";
		command += "uam_cli_target=$(readlink \"$uam_cli_path\" 2>/dev/null || true); ";
		command += "case \"$uam_cli_target\" in *node_modules/" + std::string(policy.npm_package) + "/*) "
		    "uam_npm_prefix=$(npm prefix -g 2>/dev/null); "
		    "if [ \"$uam_cli_path\" != \"$uam_npm_prefix/bin/" + std::string(policy.executable_name) + "\" ]; then uam_cli_target=''; fi;; esac; ";
		command += "printf '[UAM CLI PATH] %s|%s\\n' \"$uam_cli_path\" \"$uam_cli_target\"; ";
		command += policy.version_probe_command;
		return command;
		}
		{
			std::string package_path(policy.npm_package);
			std::ranges::replace(package_path, '/', '\\');
			// Only npm's real global shim plus its installed package identifies npm ownership.
			std::string script =
			    "$ErrorActionPreference='Stop';$p=(Get-Command '" + std::string(policy.executable_name) +
			    "' -CommandType Application -ErrorAction SilentlyContinue|Select-Object -First 1).Source;";
			if (policy.provider_id == uam::provider_ids::kCopilotCli)
			{
				script += "if(-not $p){Write-Output '[UAM CLI PATH] |';Write-Output 'copilot was not found on PATH';exit 0};"
				           "if(-not(Get-Command 'pwsh' -ErrorAction SilentlyContinue)){"
				           "Write-Output 'GitHub Copilot CLI requires PowerShell 6 or newer; pwsh was not found.';exit 1};";
			}
			script += "$t='';try{if($p -and [IO.Path]::GetExtension($p) -eq '.cmd'){"
			    "$prefix=(& npm.cmd prefix -g 2>$null|Select-Object -Last 1);"
			    "if($LASTEXITCODE -eq 0 -and $prefix -and "
			    "[IO.Path]::GetDirectoryName($p).TrimEnd('\\') -eq $prefix.Trim().TrimEnd('\\')){"
			    "$pkg=Join-Path $prefix.Trim() 'node_modules\\" + package_path + "';"
			    "if((Test-Path -LiteralPath (Join-Path $pkg 'package.json')) -and "
			    "(Get-Content -LiteralPath $p -Raw).Contains('node_modules\\" + package_path + "\\')){$t=$pkg}}}}catch{};"
			    "Write-Output ('[UAM CLI PATH] '+$p+'|'+$t);"
			    "& cmd.exe /d /c '" + std::string(policy.version_probe_command) + "';exit $LASTEXITCODE";
			std::string utf16_le;
			utf16_le.reserve(script.size() * 2);
			for (const char character : script)
			{
				utf16_le.push_back(character);
				utf16_le.push_back('\0');
			}
			return "powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand " + uam::base64::Encode(utf16_le);
		}
	}

	std::string ProviderTitle(const uam::AppState& app, std::string_view provider_id)
	{
		const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicy(provider_id);
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, resolved.provider_id); profile != nullptr && !profile->title.empty())
		{
			return profile->title;
		}
		return std::string(resolved.policy.fallback_title.empty() ? std::string_view(resolved.provider_id) : resolved.policy.fallback_title);
	}

	bool AcpSessionHasProviderInstallBlockingWork(const uam::AppState& app, const uam::AcpSessionState& session, bool local)
	{
		if (local && uam::provider_ids::IsCliProviderAliasOf(session.provider_id, uam::provider_ids::kCopilotCli) &&
		    uam::AcpSessionHasDeferredUserQueueOnly(session) &&
		    !ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli).LocalCliCompatibilityError(app).empty())
		{
			return false;
		}
		return uam::AcpSessionHasBlockingRuntimeWork(session);
	}

	bool ProviderHasActiveRuntimeWork(const uam::AppState& app, std::string_view provider_id, std::string_view host_id = "local")
	{
		const bool acp_has_work = std::ranges::any_of(app.acp_sessions, [&app, provider_id, host_id](const auto& session) {
			if (session == nullptr || !uam::provider_ids::IsCliProviderAliasOf(session->provider_id, provider_id)) return false;
			const ChatSession* chat = ChatDomainService().FindChatById(app, session->chat_id);
			if (chat == nullptr)
			{
				const auto found = std::ranges::find(app.model_discovery_chats, session->chat_id, &ChatSession::id);
				if (found != app.model_discovery_chats.end()) chat = &*found;
			}
			return (chat == nullptr || chat->execution_host_id == host_id) &&
			       AcpSessionHasProviderInstallBlockingWork(app, *session, chat != nullptr && chat->execution_host_id == "local");
		});
		if (acp_has_work)
		{
			return true;
		}

		return std::ranges::any_of(app.cli_terminals, [&app, provider_id, host_id](const auto& terminal) {
			if (terminal == nullptr || (!terminal->native_session_setup_cancel &&
			    (!terminal->running || !uam::CliTerminalHasActiveTurn(*terminal))))
			{
				return false;
			}

			const ChatSession* chat = uam::FindChatForCliTerminal(app, *terminal);
			if (chat == nullptr || chat->execution_host_id != host_id)
			{
				return false;
			}

			const ProviderProfile& terminal_provider = ProviderResolutionService().ProviderForChatOrDefault(app, *chat);
			return uam::provider_ids::IsCliProviderAliasOf(terminal_provider.id, provider_id);
		});
	}

} // namespace

std::string ProviderCliLaunchBlockReason(const uam::AppState& app, std::string_view provider_id, std::string_view execution_host_id)
{
	if (!app.runtime_cli_pin_task.running ||
	    !uam::provider_ids::IsCliProviderAliasOf(provider_id, app.runtime_cli_pin_provider_id)) return {};
	const std::string_view requested_host = execution_host_id.empty() ? uam::execution_hosts::kLocalHostId : execution_host_id;
	const std::string_view installer_host = app.runtime_cli_pin_task.execution_host.id.empty()
	    ? uam::execution_hosts::kLocalHostId : std::string_view(app.runtime_cli_pin_task.execution_host.id);
	return requested_host == installer_host
	    ? "This provider is being updated on this machine. Retry when the update finishes." : "";
}

std::string CliProviderVersionStateKey(std::string_view provider_id, std::string_view execution_host_id)
{
	const std::string normalized = uam::provider_ids::CanonicalCliProviderLookupId(provider_id);
	return execution_host_id.empty() || execution_host_id == uam::execution_hosts::kLocalHostId
	    ? normalized : std::string(execution_host_id) + "/" + normalized;
}

void ProviderCliCompatibilityService::StartVersionCheck(uam::AppState& app, bool force, bool include_remote) const
{
	if (app.runtime_cli_version_check_task.running || app.runtime_cli_pin_task.running) return;
	app.runtime_cli_version_check_queue.clear();
	std::vector<ExecutionHost> hosts{uam::execution_hosts::LocalHost()};
	if (include_remote)
	{
		for (const ExecutionHost& host : app.settings.execution_hosts)
			if (host.id != uam::execution_hosts::kLocalHostId && IsReadyCliHost(host)) hosts.push_back(host);
	}
	for (const ExecutionHost& host : hosts)
	{
		for (const ProviderProfile& profile : app.provider_profiles)
		{
			const std::string provider_id = uam::provider_ids::CanonicalCliProviderLookupId(profile.id);
			const std::pair<std::string, std::string> target{provider_id, host.id};
			if (FindProviderCliPolicy(provider_id) == nullptr ||
			    std::ranges::find(app.runtime_cli_version_check_queue, target) != app.runtime_cli_version_check_queue.end()) continue;
			uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(provider_id, host.id)];
			if (!force && state.checked && uam::execution_hosts::SameConnection(state.execution_host, host)) continue;
			if (!uam::execution_hosts::SameConnection(state.execution_host, host)) state = {};
			state.provider_id = provider_id;
			state.execution_host = host;
			state.check_error.clear();
			state.message = ProviderTitleMessage("Waiting to check installed ", ProviderTitle(app, provider_id), " version...");
			app.runtime_cli_version_check_queue.push_back(target);
		}
	}
	while (!app.runtime_cli_version_check_queue.empty() && !app.runtime_cli_version_check_task.running)
	{
		const std::pair<std::string, std::string> target = app.runtime_cli_version_check_queue.front();
		app.runtime_cli_version_check_queue.pop_front();
		StartProviderVersionCheck(app, target.first, true, target.second);
	}
}

bool ProviderCliCompatibilityService::StartProviderVersionCheck(uam::AppState& app, std::string_view provider_id,
    bool force, std::string_view execution_host_id, std::string* error_out) const
{
	const std::string normalized_provider_id = uam::provider_ids::CanonicalCliProviderLookupId(provider_id);
	const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, execution_host_id);
	if (host == nullptr || !IsReadyCliHost(*host))
		return FailProviderCliInstall(error_out, "The selected SSH helper is unavailable. Reconnect it and check again.");
	if (app.runtime_cli_version_check_task.running || app.runtime_cli_pin_task.running)
		return FailProviderCliInstall(error_out, "A provider CLI command is already running.");
	const ProviderCliPolicy* policy = FindProviderCliPolicy(normalized_provider_id);
	if (policy == nullptr)
		return FailProviderCliInstall(error_out, "Provider CLI version checks are not supported for this provider.");
	const std::string key = CliProviderVersionStateKey(normalized_provider_id, host->id);
	uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id[key];
	if (!force && state.checked && uam::execution_hosts::SameConnection(state.execution_host, *host)) return true;
	const std::string command = BuildInstallAwareProbeCommand(*policy, host->id == "local" ? LocalPlatform() : host->platform);
	if (command.empty()) return FailProviderCliInstall(error_out, "Provider CLI version checks are not supported for this provider.");
	if (!uam::execution_hosts::SameConnection(state.execution_host, *host)) state = {};
	state.provider_id = normalized_provider_id;
	state.execution_host = *host;
	state.check_error.clear();
	state.message = ProviderTitleMessage("Checking installed ", ProviderTitle(app, normalized_provider_id), " version...");
	app.runtime_cli_version_provider_id = normalized_provider_id;
	StartAsyncCommandTask(app.runtime_cli_version_check_task, *host, normalized_provider_id, command, false, kProviderCliVersionProbeTimeoutMs);
	return true;
}

bool ProviderCliCompatibilityService::StartInstallProviderVersion(uam::AppState& app, std::string_view provider_id, std::string_view version, std::string* error_out, std::string_view execution_host_id) const
{
	const OptionalProviderCliPolicy resolved = ResolveKnownProviderCliPolicy(provider_id);
	const std::string unsupported_provider_id = uam::strings::NonEmptyOrFallback(resolved.provider_id, uam::strings::TrimAsciiView(provider_id));
	std::string_view trimmed_version = uam::strings::TrimAsciiView(version);
	if (app.runtime_cli_pin_task.running)
	{
		return FailProviderCliInstall(error_out, "A provider CLI install is already running.");
	}
	if (app.runtime_cli_version_check_task.running)
	{
		return FailProviderCliInstall(error_out, "A provider CLI version check is already running.");
	}
	if (resolved.policy == nullptr || ProviderProfileStore::FindById(app.provider_profiles, resolved.provider_id) == nullptr)
	{
		return FailProviderCliInstall(error_out, "Unsupported provider: " + unsupported_provider_id);
	}
	if (!IsSupportedVersionForProvider(resolved.provider_id, trimmed_version))
	{
		constexpr std::string_view kUnsupportedVersionPrefix = "Unsupported CLI version: ";
		std::string message;
		message.reserve(kUnsupportedVersionPrefix.size() + trimmed_version.size());
		message.append(kUnsupportedVersionPrefix);
		message.append(trimmed_version);
		return FailProviderCliInstall(error_out, std::move(message));
	}
	const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, execution_host_id);
	if (host == nullptr || !IsReadyCliHost(*host))
		return FailProviderCliInstall(error_out, "The selected SSH helper is unavailable. Reconnect it and check again.");
	if (ProviderHasActiveRuntimeWork(app, resolved.provider_id, host->id))
	{
		return FailProviderCliInstall(error_out, "Cannot install a provider CLI version while that provider is processing.");
	}

	uam::CliProviderVersionState& provider_state = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(resolved.provider_id, host->id)];
	if (host->id != "local" && (!provider_state.checked || provider_state.installed_version.empty() ||
	    !uam::execution_hosts::SameConnection(provider_state.execution_host, *host)))
		return FailProviderCliInstall(error_out, "Check this machine's installed CLI version before updating.");
	if (host->id != "local" && provider_state.install_method != "npm" && provider_state.install_method != "homebrew-formula" &&
	    provider_state.install_method != "homebrew-cask" && provider_state.install_method != "winget")
		return FailProviderCliInstall(error_out, "The CLI installation method could not be identified. Use its existing installer to update it.");
	if (host->id != "local" &&
	    (provider_state.install_method != InstallMethodFromProbeOutput(provider_state.raw_output, true) ||
	     !ValidateRemoteInstallProbe(provider_state.raw_output, provider_state.raw_output, host->platform, error_out)))
		return FailProviderCliInstall(error_out, "Check this machine's CLI installation again before updating.");
	const std::string command = BuildInstallCommand(*resolved.policy, trimmed_version, provider_state.install_method);
	if (command.empty())
	{
		return FailProviderCliInstall(error_out, "Provider CLI installs are not supported for this provider.");
	}

	provider_state.provider_id = resolved.provider_id;
	provider_state.execution_host = *host;
	app.runtime_cli_pin_provider_id = resolved.provider_id;
	provider_state.selected_version.assign(trimmed_version);
	provider_state.install_command = command;
	provider_state.install_output.clear();
	provider_state.last_install_status = "running";
	StartAsyncCommandTask(app.runtime_cli_pin_task, *host, resolved.provider_id, command, true, kProviderCliInstallTimeoutMs, provider_state.raw_output);
	app.status_line = ProviderTitleMessage("Running ", ProviderTitle(app, resolved.provider_id), " install command...");
	return true;
}

void ProviderCliCompatibilityService::Poll(uam::AppState& app) const
{
	const auto host_matches = [&app](const ExecutionHost& captured)
	{
		const ExecutionHost* current = uam::execution_hosts::Find(app.settings.execution_hosts, captured.id);
		return current != nullptr && uam::execution_hosts::SameConnection(captured, *current);
	};
	const auto apply_remote_health = [&app](const ExecutionHost& captured,
	    const std::optional<bool>& connected)
	{
		if (captured.id == uam::execution_hosts::kLocalHostId || !connected) return;
		auto current = std::ranges::find_if(app.settings.execution_hosts,
		    [&captured](const ExecutionHost& host) { return host.id == captured.id; });
		if (current == app.settings.execution_hosts.end()) return;
		const ExecutionHost previous = *current;
		if (uam::execution_hosts::ApplyHealthObservation(*current, captured, *connected,
		        uam::time::IsoUtcTimestampNow()) && *current != previous)
			app.remote_host_health_changed = true;
	};
	std::erase_if(app.runtime_cli_versions_by_provider_id, [&host_matches](const auto& entry)
	{
		return entry.second.execution_host.id != uam::execution_hosts::kLocalHostId &&
		       !host_matches(entry.second.execution_host);
	});
	std::string output;

	const ExecutionHost check_host = app.runtime_cli_version_check_task.execution_host;
	std::optional<bool> check_connected;
	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_version_check_task, output, &check_connected))
	{
		apply_remote_health(check_host, check_connected);
		const std::string provider_id = uam::provider_ids::CanonicalCliProviderLookupId(app.runtime_cli_version_provider_id);
		if (host_matches(check_host))
		{
			uam::CliProviderVersionState& provider_state = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(provider_id, check_host.id)];
			provider_state.install_method = InstallMethodFromProbeOutput(output, check_host.id != "local");
			const std::string probe_output = output;
			output = StripProbePathLine(std::move(output));
			provider_state.checked = true;
			provider_state.raw_output = check_host.id == "local" ? output : probe_output;
			provider_state.installed_version.clear();
			provider_state.supported = false;

			const bool command_failed = OutputIndicatesCommandFailure(output);

			const std::optional<std::string> parsed = command_failed ? std::nullopt : ExtractSemverVersion(output);

			if (command_failed)
			{
				provider_state.message = provider_id == uam::provider_ids::kCopilotCli && uam::strings::ContainsCaseInsensitive(output, "PowerShell 6 or newer") ? "GitHub Copilot CLI requires PowerShell 6 or newer (pwsh) on Windows." : ProviderTitleMessage("Could not check ", ProviderTitle(app, provider_id), " version.");
			}
			else if (parsed)
			{
				provider_state.installed_version = *parsed;
				const std::string compatibility = CompatibilityStatusForProvider(provider_id, provider_state.installed_version);
				provider_state.supported = compatibility != "known-incompatible" && compatibility != "unavailable";

				if (compatibility == "verified")
				{
					provider_state.message = ProviderTitleMessage("Installed ", ProviderTitle(app, provider_id), " version matches UAM's last verified build.");
				}
				else if (compatibility == "untested-newer")
				{
					provider_state.message = ProviderTitleMessage("Installed ", ProviderTitle(app, provider_id), " version is newer than UAM's last verified build; it remains available for use.");
				}
				else if (compatibility == "untested")
				{
					provider_state.message = ProviderTitleMessage("Installed ", ProviderTitle(app, provider_id), " version has not been verified by this UAM build; it remains available for use.");
				}
				else if (compatibility == "provider-managed")
				{
					provider_state.message = ProviderTitleMessage("Installed ", ProviderTitle(app, provider_id), " compatibility is managed by the provider.");
				}
				else
				{
					provider_state.message = ProviderTitleMessage("Installed ", ProviderTitle(app, provider_id), " version is known to be incompatible with this UAM build.");
				}
			}
			else
			{
				provider_state.message = UnparsedVersionOutputMessage(ProviderTitle(app, provider_id), output);
			}
			const bool confirmed_missing = ProbeIdentity(probe_output).starts_with('|') && OutputIndicatesCommandMissing(output);
			provider_state.check_error = (command_failed || !parsed) && !confirmed_missing ? provider_state.message : "";
		}

		while (!app.runtime_cli_version_check_queue.empty() && !app.runtime_cli_version_check_task.running)
		{
			const std::pair<std::string, std::string> target = app.runtime_cli_version_check_queue.front();
			app.runtime_cli_version_check_queue.pop_front();
			const auto queued = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(target.first, target.second));
			if (queued == app.runtime_cli_versions_by_provider_id.end() || !host_matches(queued->second.execution_host)) continue;
			StartProviderVersionCheck(app, target.first, true, target.second);
		}
	}

	const ExecutionHost install_host = app.runtime_cli_pin_task.execution_host;
	std::optional<bool> install_connected;
	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_pin_task, output, &install_connected))
	{
		apply_remote_health(install_host, install_connected);
		if (!host_matches(install_host))
		{
			app.status_line = "Machine configuration changed. Check its CLI version again.";
			return;
		}
		const std::string provider_id = uam::provider_ids::CanonicalCliProviderLookupId(app.runtime_cli_pin_provider_id);
		uam::CliProviderVersionState& provider_state = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(provider_id, install_host.id)];
		provider_state.install_output = output;

		if (OutputIndicatesCommandFailure(output))
		{
			app.status_line = "Provider CLI update command failed. Review its output in Updates.";
			provider_state.message = "Update command failed.";
			provider_state.last_install_status = "failed";
		}
		else
		{
			app.status_line = "Provider CLI update completed. Re-checking installed version.";
			provider_state.message = app.status_line;
			provider_state.last_install_status = "succeeded";
			StartProviderVersionCheck(app, provider_id, true, install_host.id);
		}
	}
}

std::vector<CliProviderVersionOption> ProviderCliCompatibilityService::SupportedVersionsForProvider(std::string_view provider_id) const
{
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicy(provider_id);
	std::vector<CliProviderVersionOption> versions;
	if (resolved.policy.preferred_version != nullptr)
	{
		versions.reserve(resolved.policy.fallback_version == nullptr ? 1 : 2);
		versions.push_back({resolved.policy.preferred_version, true});
	}
	if (resolved.policy.fallback_version != nullptr)
	{
		const bool duplicate = resolved.policy.preferred_version != nullptr && std::string_view(resolved.policy.fallback_version) == std::string_view(resolved.policy.preferred_version);
		if (!duplicate)
		{
			versions.push_back({resolved.policy.fallback_version, false});
		}
	}
	return versions;
}

std::string ProviderCliCompatibilityService::PreferredVersionForProvider(std::string_view provider_id) const
{
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicy(provider_id);
	return resolved.policy.preferred_version == nullptr ? std::string() : std::string(resolved.policy.preferred_version);
}

bool ProviderCliCompatibilityService::IsSupportedVersionForProvider(std::string_view provider_id, std::string_view version) const
{
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicy(provider_id);
	std::string_view trimmed_version = uam::strings::TrimAsciiView(version);
	if (resolved.policy.provider_id.empty() || !IsSafeVersionToken(trimmed_version))
	{
		return false;
	}

	switch (resolved.policy.version_policy)
	{
		case ProviderCliVersionPolicy::MinimumSemver:
			return trimmed_version == kLatestVersion || (resolved.policy.minimum_version != nullptr && SemverAtLeast(trimmed_version, resolved.policy.minimum_version));
		case ProviderCliVersionPolicy::AnySafeToken:
			return true;
	}
	return false;
}

std::string ProviderCliCompatibilityService::CompatibilityStatusForProvider(std::string_view provider_id, std::string_view version) const
{
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicy(provider_id);
	const std::string_view trimmed_version = uam::strings::TrimAsciiView(version);
	if (trimmed_version.empty())
	{
		return "unavailable";
	}
	if (resolved.policy.provider_managed)
	{
		return "provider-managed";
	}
	if (!IsSupportedVersionForProvider(resolved.provider_id, trimmed_version))
	{
		return "known-incompatible";
	}
	if (resolved.policy.fallback_version == nullptr)
	{
		return "untested";
	}
	if (trimmed_version == resolved.policy.fallback_version)
	{
		return "verified";
	}
	return SemverAtLeast(trimmed_version, resolved.policy.fallback_version) ? "untested-newer" : "untested";
}

std::string ProviderCliCompatibilityService::VerifiedVersionForProvider(std::string_view provider_id) const
{
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicy(provider_id);
	return resolved.policy.fallback_version == nullptr ? std::string() : std::string(resolved.policy.fallback_version);
}

std::string ProviderCliCompatibilityService::VerifiedAtForProvider(std::string_view provider_id) const
{
	const ProviderCliPolicy& policy = ResolveProviderCliPolicy(provider_id).policy;
	return policy.fallback_version == nullptr || policy.verified_at == nullptr ? std::string() : std::string(policy.verified_at);
}



std::string ProviderCliCompatibilityService::VersionProbeCommandForProvider(std::string_view provider_id) const
{
	return std::string(ResolveProviderCliPolicy(provider_id).policy.version_probe_command);
}

std::string ProviderCliCompatibilityService::InstallCommandForProviderVersion(std::string_view provider_id, std::string_view version) const
{
	const OptionalProviderCliPolicy resolved = ResolveKnownProviderCliPolicy(provider_id);
	std::string_view trimmed_version = uam::strings::TrimAsciiView(version);
	if (resolved.policy == nullptr || !IsSupportedVersionForProvider(resolved.provider_id, trimmed_version))
	{
		return "";
	}
	return BuildNpmGlobalInstallCommand(resolved.policy->npm_package, trimmed_version);
}

std::string BuildCliProviderVersionProbeCommandForTests(std::string_view provider_id)
{
	return ProviderCliCompatibilityService().VersionProbeCommandForProvider(provider_id);
}

std::string BuildCliProviderInstallCommandForTests(std::string_view provider_id, std::string_view version)
{
	return ProviderCliCompatibilityService().InstallCommandForProviderVersion(provider_id, version);
}

std::string BuildCliProviderInstallCommandForMethodForTests(std::string_view provider_id, std::string_view version, std::string_view install_method)
{
	const OptionalProviderCliPolicy resolved = ResolveKnownProviderCliPolicy(provider_id);
	const std::string_view trimmed_version = uam::strings::TrimAsciiView(version);
	if (resolved.policy == nullptr || !ProviderCliCompatibilityService().IsSupportedVersionForProvider(resolved.provider_id, trimmed_version))
	{
		return "";
	}
	return BuildInstallCommand(*resolved.policy, trimmed_version, install_method);
}

std::string ExtractCliProviderSemverVersionForTests(std::string_view output)
{
	const std::optional<std::string> parsed = ExtractSemverVersion(output);
	return parsed.value_or("");
}

std::string ExtractCliProviderInstallMethodForTests(std::string_view output)
{
	return InstallMethodFromProbeOutput(output);
}

bool CliProviderVersionOutputIndicatesMissingCommandForTests(std::string_view output)
{
	return OutputIndicatesCommandMissing(output);
}

bool ProviderCliInstallBlockedByActiveRuntimeForTests(const uam::AppState& app, std::string_view provider_id)
{
	return ProviderHasActiveRuntimeWork(app, provider_id);
}

std::string GetNpmPackageNameForProvider(std::string_view provider_id)
{
	if (const ProviderCliPolicy* policy = FindProviderCliPolicy(provider_id))
	{
		return std::string(policy->npm_package);
	}
	return "";
}

bool ValidateRemoteCliInstallProbeForTests(std::string_view previous, std::string_view current, std::string_view platform, std::string* error)
{
	return ValidateRemoteInstallProbe(previous, current, platform, error);
}

std::string BuildInstallAwareCliProbeForTests(std::string_view provider_id, std::string_view platform)
{
	return BuildInstallAwareProbeCommand(ResolveProviderCliPolicy(provider_id).policy, platform);
}
