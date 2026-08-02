#include "provider_cli_compatibility_service.h"

#include "app/provider_resolution_service.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/state/app_state.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "core/gemini_cli_compat.h"

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
	constexpr const char* kCodexPreferredVersion = "0.124.0";
	constexpr const char* kCodexFallbackVersion = "0.123.0";
	constexpr const char* kCopilotMinimumVersion = "1.0.60";
	constexpr const char* kLatestVersion = "latest";
	constexpr const char* kDefaultProviderCliPolicyId = uam::provider_ids::kGeminiCli;
	constexpr const char* kCommandFailurePrefix = "Failed to run command: ";
	constexpr const char* kProviderCliTimedOutSuffix = "\n\n[Provider CLI command timed out]";
	constexpr const char* kProviderCliCanceledSuffix = "\n\n[Provider CLI command canceled]";
	constexpr const char* kProviderCliExitCodeMarker = "[Provider CLI exited with code ";
	constexpr int kProviderCliVersionProbeTimeoutMs = 30000;
	constexpr std::string_view kProviderCliPathMarker = "[UAM CLI PATH] ";
	constexpr auto kSafeVersionTokenPunctuation = std::to_array<char>({
	    '.',
	    '_',
	    '-',
	});

	enum class CliVersionPolicy
	{
		GeminiCurated,
		FixedPreferredAndFallback,
		MinimumSemver,
		AnySafeToken,
	};

	struct ProviderCliPolicy
	{
		std::string_view provider_id;
		std::string_view npm_package;
		std::string_view fallback_title;
		std::string_view executable_name;
		std::string_view version_probe_command;
		std::string_view homebrew_package;
		std::string_view winget_package;
		bool homebrew_cask = false;
		const char* preferred_version = nullptr;
		const char* fallback_version = nullptr;
		const char* minimum_version = nullptr;
		CliVersionPolicy version_policy = CliVersionPolicy::AnySafeToken;
	};

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

	constexpr auto kProviderCliPolicies = std::to_array<ProviderCliPolicy>({
	    ProviderCliPolicy{
	        .provider_id = uam::provider_ids::kGeminiCli,
	        .npm_package = "@google/gemini-cli",
	        .fallback_title = "Gemini CLI",
	        .executable_name = "gemini",
	        .version_probe_command = "gemini --version",
	        .homebrew_package = "gemini-cli",
	        .version_policy = CliVersionPolicy::GeminiCurated,
	    },
	    ProviderCliPolicy{
	        .provider_id = uam::provider_ids::kCodexCli,
	        .npm_package = "@openai/codex",
	        .fallback_title = "Codex CLI",
	        .executable_name = "codex",
	        .version_probe_command = "codex --version",
	        .homebrew_package = "codex",
	        .homebrew_cask = true,
	        .preferred_version = kCodexPreferredVersion,
	        .fallback_version = kCodexFallbackVersion,
	        .version_policy = CliVersionPolicy::FixedPreferredAndFallback,
	    },
	    ProviderCliPolicy{
	        .provider_id = uam::provider_ids::kClaudeCli,
	        .npm_package = "@anthropic-ai/claude-code",
	        .fallback_title = "Claude Code",
	        .executable_name = "claude",
	        .version_probe_command = "claude --version",
	        .homebrew_package = "claude-code",
	        .homebrew_cask = true,
	        .preferred_version = kLatestVersion,
	    },
	    ProviderCliPolicy{
	        .provider_id = uam::provider_ids::kOpenCodeCli,
	        .npm_package = "opencode-ai",
	        .fallback_title = "OpenCode",
	        .executable_name = "opencode",
	        .version_probe_command = "opencode --version",
	        .homebrew_package = "opencode",
	        .preferred_version = kLatestVersion,
	    },
	    ProviderCliPolicy{
	        .provider_id = uam::provider_ids::kCopilotCli,
	        .npm_package = "@github/copilot",
	        .fallback_title = "GitHub Copilot CLI",
	        .executable_name = "copilot",
	        .version_probe_command = "copilot --version",
	        .homebrew_package = "copilot-cli",
	        .winget_package = "GitHub.Copilot",
	        .homebrew_cask = true,
	        .preferred_version = kLatestVersion,
	        .minimum_version = kCopilotMinimumVersion,
	        .version_policy = CliVersionPolicy::MinimumSemver,
	    },
	});

	const ProviderCliPolicy* FindProviderCliPolicy(std::string_view normalized_provider_id)
	{
		const auto it = std::ranges::find_if(kProviderCliPolicies, [normalized_provider_id](const ProviderCliPolicy& policy) { return policy.provider_id == normalized_provider_id; });
		return it == kProviderCliPolicies.end() ? nullptr : &*it;
	}

	const ProviderCliPolicy& ProviderCliPolicyOrDefault(std::string_view normalized_provider_id)
	{
		if (const ProviderCliPolicy* policy = FindProviderCliPolicy(normalized_provider_id))
		{
			return *policy;
		}
		if (const ProviderCliPolicy* policy = FindProviderCliPolicy(kDefaultProviderCliPolicyId))
		{
			return *policy;
		}
		return kProviderCliPolicies.front();
	}

	void StartAsyncCommandTask(uam::AsyncCommandTask& task, const std::string& command, int timeout_ms = -1)
	{
		uam::ResetAsyncCommandTask(task);
		task.running = true;
		task.command_preview = command;
		task.state = std::make_shared<AsyncProcessTaskState>();
		std::shared_ptr<AsyncProcessTaskState> state = task.state;
		task.worker = std::make_unique<std::jthread>(
		    [command, timeout_ms, state](std::stop_token stop_token)
		    {
			    state->result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, timeout_ms, stop_token);

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
				    else if (state->result.exit_code != 0)
				    {
					    state->result.output += "\n\n" + std::string(kProviderCliExitCodeMarker) + std::to_string(state->result.exit_code) + "]";
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
			uam::ResetAsyncCommandTask(task);
			output_out.clear();
			return true;
		}

		if (!task.state->completed.load(std::memory_order_acquire))
		{
			return false;
		}

		output_out = std::move(task.state->result.output);
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
		return uam::strings::StartsWith(output, kCommandFailurePrefix) || OutputContainsNonZeroExit(output) || uam::strings::Contains(output, kProviderCliTimedOutSuffix) || uam::strings::Contains(output, kProviderCliCanceledSuffix);
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

	std::string NormalizeProviderCliPolicyIdOrDefault(std::string_view provider_id)
	{
		const std::string normalized = uam::provider_ids::NormalizeCliProviderAlias(provider_id);
		if (!normalized.empty())
		{
			return normalized;
		}

		return kDefaultProviderCliPolicyId;
	}

	ResolvedProviderCliPolicy ResolveProviderCliPolicyOrDefault(std::string_view provider_id)
	{
		const std::string normalized_provider_id = NormalizeProviderCliPolicyIdOrDefault(provider_id);
		return {normalized_provider_id, ProviderCliPolicyOrDefault(normalized_provider_id)};
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

	bool VersionMatchesFixedProviderPolicy(const ProviderCliPolicy& policy, std::string_view version)
	{
		return version == policy.preferred_version ||
		       (policy.fallback_version != nullptr && version == policy.fallback_version);
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

	std::string InstallMethodFromProbeOutput(std::string_view output)
	{
		const std::size_t marker = output.find(kProviderCliPathMarker);
		if (marker == std::string_view::npos)
		{
			return "npm";
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
		return "npm";
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

	std::string BuildInstallAwareProbeCommand(const ProviderCliPolicy& policy)
	{
#if defined(__APPLE__)
		std::string command = "uam_cli_path=$(command -v " + std::string(policy.executable_name) + " 2>/dev/null || true); ";
		command += "uam_cli_target=$(readlink \"$uam_cli_path\" 2>/dev/null || true); ";
		command += "printf '[UAM CLI PATH] %s|%s\\n' \"$uam_cli_path\" \"$uam_cli_target\"; ";
		command += policy.version_probe_command;
		return command;
#elif defined(_WIN32)
		std::string command;
		if (policy.provider_id == uam::provider_ids::kCopilotCli)
		{
			command = "where pwsh >nul 2>nul || (echo GitHub Copilot CLI requires PowerShell 6 or newer; pwsh was not found. & exit /b 1) & ";
		}
		command += "for /f \"delims=\" %I in ('where " + std::string(policy.executable_name) + " 2^>nul') do @echo [UAM CLI PATH] %I^| & " + std::string(policy.version_probe_command);
		return command;
#else
		return std::string(policy.version_probe_command);
#endif
	}

	std::string ProviderTitle(const uam::AppState& app, std::string_view provider_id)
	{
		const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicyOrDefault(provider_id);
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, resolved.provider_id); profile != nullptr && !profile->title.empty())
		{
			return profile->title;
		}
		return std::string(resolved.policy.fallback_title);
	}

	bool AcpSessionHasProviderInstallBlockingWork(const uam::AppState& app, const uam::AcpSessionState& session)
	{
		if (uam::provider_ids::IsCliProviderAliasOf(session.provider_id, uam::provider_ids::kCopilotCli) &&
		    uam::AcpSessionHasDeferredUserQueueOnly(session) &&
		    !CopilotLaunchBlockReason(app).empty())
		{
			return false;
		}
		return uam::AcpSessionHasBlockingRuntimeWork(session);
	}

	bool ProviderHasActiveRuntimeWork(const uam::AppState& app, std::string_view provider_id)
	{
		const bool acp_has_work = std::ranges::any_of(app.acp_sessions, [&app, provider_id](const auto& session) {
			return session != nullptr &&
			       uam::provider_ids::IsCliProviderAliasOf(session->provider_id, provider_id) &&
			       AcpSessionHasProviderInstallBlockingWork(app, *session);
		});
		if (acp_has_work)
		{
			return true;
		}

		return std::ranges::any_of(app.cli_terminals, [&app, provider_id](const auto& terminal) {
			if (terminal == nullptr || !terminal->running || !uam::CliTerminalHasActiveTurn(*terminal))
			{
				return false;
			}

			const ChatSession* chat = uam::FindChatForCliTerminal(app, *terminal);
			if (chat == nullptr)
			{
				return false;
			}

			const ProviderProfile& terminal_provider = ProviderResolutionService().ProviderForChatOrDefault(app, *chat);
			return uam::provider_ids::IsCliProviderAliasOf(terminal_provider.id, provider_id);
		});
	}

} // namespace

void ProviderCliCompatibilityService::StartVersionCheck(uam::AppState& app, bool force) const
{
	if (app.runtime_cli_version_check_task.running)
	{
		return;
	}
	app.runtime_cli_version_check_queue.clear();
	for (const ProviderProfile& profile : app.provider_profiles)
	{
		const std::string provider_id = NormalizeProviderCliPolicyIdOrDefault(profile.id);
		if (FindProviderCliPolicy(provider_id) == nullptr || std::ranges::find(app.runtime_cli_version_check_queue, provider_id) != app.runtime_cli_version_check_queue.end())
		{
			continue;
		}
		if (!force)
		{
			const auto existing = app.runtime_cli_versions_by_provider_id.find(provider_id);
			if (existing != app.runtime_cli_versions_by_provider_id.end() && existing->second.checked)
			{
				continue;
			}
		}
		app.runtime_cli_version_check_queue.push_back(provider_id);
	}
	if (!app.runtime_cli_version_check_queue.empty())
	{
		const std::string provider_id = app.runtime_cli_version_check_queue.front();
		app.runtime_cli_version_check_queue.pop_front();
		StartProviderVersionCheck(app, provider_id, true);
	}
}

void ProviderCliCompatibilityService::StartProviderVersionCheck(uam::AppState& app, std::string_view provider_id, bool force) const
{
	const std::string normalized_provider_id = NormalizeProviderCliPolicyIdOrDefault(provider_id);
	if (app.runtime_cli_version_check_task.running)
	{
		return;
	}

	const auto existing_state = app.runtime_cli_versions_by_provider_id.find(normalized_provider_id);
	if (!force && existing_state != app.runtime_cli_versions_by_provider_id.end() && existing_state->second.checked)
	{
		return;
	}

	const ProviderCliPolicy& policy = ProviderCliPolicyOrDefault(normalized_provider_id);
	const std::string command = BuildInstallAwareProbeCommand(policy);
	if (command.empty())
	{
		app.runtime_cli_versions_by_provider_id[normalized_provider_id].message = "Provider CLI version checks are not supported for this provider.";
		return;
	}

	app.runtime_cli_version_provider_id = normalized_provider_id;
	app.runtime_cli_versions_by_provider_id[normalized_provider_id].message = ProviderTitleMessage("Checking installed ", ProviderTitle(app, normalized_provider_id), " version...");
	StartAsyncCommandTask(app.runtime_cli_version_check_task, command, kProviderCliVersionProbeTimeoutMs);
}

void ProviderCliCompatibilityService::StartPinToSupported(uam::AppState& app) const
{
	std::string error;
	(void)StartInstallProviderVersion(app, kDefaultProviderCliPolicyId, PreferredVersionForProvider(kDefaultProviderCliPolicyId), &error);
	if (!error.empty())
	{
		app.status_line = error;
	}
}

bool ProviderCliCompatibilityService::StartInstallProviderVersion(uam::AppState& app, std::string_view provider_id, std::string_view version, std::string* error_out) const
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
	if (ProviderHasActiveRuntimeWork(app, resolved.provider_id))
	{
		return FailProviderCliInstall(error_out, "Cannot install a provider CLI version while that provider is processing.");
	}

	uam::CliProviderVersionState& provider_state = app.runtime_cli_versions_by_provider_id[resolved.provider_id];
	const std::string command = BuildInstallCommand(*resolved.policy, trimmed_version, provider_state.install_method);
	if (command.empty())
	{
		return FailProviderCliInstall(error_out, "Provider CLI installs are not supported for this provider.");
	}

	app.runtime_cli_pin_provider_id = resolved.provider_id;
	provider_state.selected_version.assign(trimmed_version);
	provider_state.install_command = command;
	provider_state.install_output.clear();
	provider_state.last_install_status = "running";
	StartAsyncCommandTask(app.runtime_cli_pin_task, command);
	app.status_line = ProviderTitleMessage("Running ", ProviderTitle(app, resolved.provider_id), " install command...");
	return true;
}

void ProviderCliCompatibilityService::Poll(uam::AppState& app) const
{
	std::string output;

	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_version_check_task, output))
	{
		const std::string provider_id = NormalizeProviderCliPolicyIdOrDefault(app.runtime_cli_version_provider_id);
		uam::CliProviderVersionState& provider_state = app.runtime_cli_versions_by_provider_id[provider_id];
		provider_state.install_method = InstallMethodFromProbeOutput(output);
		output = StripProbePathLine(std::move(output));
		provider_state.checked = true;
		provider_state.raw_output = output;
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
			provider_state.supported = IsSupportedVersionForProvider(provider_id, provider_state.installed_version);

			if (provider_state.supported)
			{
				provider_state.message = ProviderTitleMessage("", ProviderTitle(app, provider_id), " version is supported.");
			}
			else
			{
				provider_state.message = ProviderTitleMessage("Installed ", ProviderTitle(app, provider_id), " version is not in the curated supported list.");
			}
		}
		else
		{
			provider_state.message = UnparsedVersionOutputMessage(ProviderTitle(app, provider_id), output);
		}

		while (!app.runtime_cli_version_check_queue.empty() && !app.runtime_cli_version_check_task.running)
		{
			const std::string queued_provider_id = app.runtime_cli_version_check_queue.front();
			app.runtime_cli_version_check_queue.pop_front();
			StartProviderVersionCheck(app, queued_provider_id, true);
		}
	}

	if (TryConsumeAsyncCommandTaskOutput(app.runtime_cli_pin_task, output))
	{
		const std::string provider_id = NormalizeProviderCliPolicyIdOrDefault(app.runtime_cli_pin_provider_id);
		uam::CliProviderVersionState& provider_state = app.runtime_cli_versions_by_provider_id[provider_id];
		provider_state.install_output = output;

		if (OutputIndicatesCommandFailure(output))
		{
			app.status_line = "Provider CLI update command failed. Review output in Settings.";
			provider_state.message = "Update command failed.";
			provider_state.last_install_status = "failed";
		}
		else
		{
			app.status_line = "Provider CLI update completed. Re-checking installed version.";
			provider_state.message = app.status_line;
			provider_state.last_install_status = "succeeded";
			StartProviderVersionCheck(app, provider_id, true);
		}
	}
}

std::vector<CliProviderVersionOption> ProviderCliCompatibilityService::SupportedVersionsForProvider(std::string_view provider_id) const
{
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicyOrDefault(provider_id);
	std::vector<CliProviderVersionOption> versions;
	if (resolved.policy.version_policy == CliVersionPolicy::GeminiCurated)
	{
		versions.reserve(uam::SupportedGeminiCliVersions().size());
		for (std::string_view version : uam::SupportedGeminiCliVersions())
		{
			const std::string text(version);
			versions.push_back({text, text == uam::PreferredGeminiCliVersion()});
		}
		return versions;
	}
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
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicyOrDefault(provider_id);
	if (resolved.policy.version_policy == CliVersionPolicy::GeminiCurated)
	{
		return std::string(uam::PreferredGeminiCliVersion());
	}
	return resolved.policy.preferred_version == nullptr ? std::string() : std::string(resolved.policy.preferred_version);
}

bool ProviderCliCompatibilityService::IsSupportedVersionForProvider(std::string_view provider_id, std::string_view version) const
{
	const ResolvedProviderCliPolicy resolved = ResolveProviderCliPolicyOrDefault(provider_id);
	std::string_view trimmed_version = uam::strings::TrimAsciiView(version);
	if (!IsSafeVersionToken(trimmed_version))
	{
		return false;
	}

	switch (resolved.policy.version_policy)
	{
		case CliVersionPolicy::GeminiCurated:
			return uam::IsSupportedGeminiCliVersion(trimmed_version);
		case CliVersionPolicy::FixedPreferredAndFallback:
			return VersionMatchesFixedProviderPolicy(resolved.policy, trimmed_version);
	case CliVersionPolicy::MinimumSemver:
		return trimmed_version == kLatestVersion || (resolved.policy.minimum_version != nullptr && SemverAtLeast(trimmed_version, resolved.policy.minimum_version));
	case CliVersionPolicy::AnySafeToken:
		return true;
	}
	return false;
}

std::string CopilotLaunchBlockReason(const uam::AppState& app)
{
	const auto state_it = app.runtime_cli_versions_by_provider_id.find(uam::provider_ids::kCopilotCli);
	if (state_it == app.runtime_cli_versions_by_provider_id.end())
	{
		return "";
	}

	const uam::CliProviderVersionState& state = state_it->second;
	if (!state.checked)
	{
		return "Checking GitHub Copilot CLI compatibility. Try again in a moment.";
	}
	if (state.supported)
	{
		return "";
	}
	if (!state.installed_version.empty())
	{
		return "GitHub Copilot CLI 1.0.60 or newer is required (installed " + state.installed_version + "). Update it in Settings.";
	}
	return uam::strings::NonEmptyOrFallback(state.message, "GitHub Copilot CLI is not installed or its version could not be determined.") + " Open Settings to check or update it.";
}

std::string ProviderCliCompatibilityService::VersionProbeCommandForProvider(std::string_view provider_id) const
{
	return std::string(ResolveProviderCliPolicyOrDefault(provider_id).policy.version_probe_command);
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
	for (const ProviderCliPolicy& policy : kProviderCliPolicies)
	{
		if (policy.provider_id == provider_id)
		{
			return std::string(policy.npm_package);
		}
	}
	return "";
}
