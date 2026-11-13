#pragma once

#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_profile_constants.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/utils/command_line_words.h"
#include "common/utils/env_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::provider_runtime_internal
{
	inline constexpr std::string_view kReferencedFilesHeading = "\n\nReferenced files:\n";
	inline constexpr const char* kPreserveProviderChildSecretsEnv = "UAM_PRESERVE_PROVIDER_CHILD_SECRETS";

	inline bool PreserveProviderChildSecrets()
	{
		const std::optional<std::string> value = uam::env::GetTrimmedString(kPreserveProviderChildSecretsEnv);
		if (!value)
		{
			return false;
		}
		constexpr auto truthy = std::to_array<std::string_view>({"1", "true", "yes", "on"});
		return uam::strings::ContainsEqualIgnoreCase(truthy, *value);
	}

	inline std::vector<std::pair<std::string, std::string>> ProviderChildEnvironmentOverrides(const ProviderProfile& profile)
	{
		if (PreserveProviderChildSecrets())
		{
			return {};
		}

		const std::string provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(profile.id);
		// OpenCode is intentionally multi-provider. Its selected model may legitimately use
		// any of these credentials, so ownership cannot be narrowed at process launch.
		if (provider_id == uam::provider_ids::kOpenCodeCli)
		{
			return {};
		}

		std::vector<std::pair<std::string, std::string>> overrides;
		const auto scrub_unless = [&](const char* name, std::string_view owner)
		{
			if (provider_id != owner)
			{
				overrides.emplace_back(name, "");
			}
		};
		scrub_unless("OPENAI_API_KEY", uam::provider_ids::kCodexCli);
		scrub_unless("ANTHROPIC_API_KEY", uam::provider_ids::kClaudeCli);
		scrub_unless("GEMINI_API_KEY", uam::provider_ids::kGeminiCli);
		scrub_unless("GOOGLE_API_KEY", uam::provider_ids::kGeminiCli);
		return overrides;
	}

	inline bool AnyTypeMatches(const std::vector<std::string>& types, std::string_view value)
	{
		return uam::strings::ContainsEqualIgnoreCase(types, value);
	}

	inline MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type)
	{
		if (AnyTypeMatches(profile.user_message_types, native_type))
		{
			return MessageRole::User;
		}

		if (AnyTypeMatches(profile.assistant_message_types, native_type))
		{
			return MessageRole::Assistant;
		}

		return MessageRole::System;
	}

	inline std::string JoinFlags(const std::vector<std::string>& flags)
	{
		std::vector<std::string> trimmed_flags;
		trimmed_flags.reserve(flags.size());
		for (const std::string& flag : flags)
		{
			std::string_view trimmed_flag = uam::strings::TrimAsciiView(flag);
			if (!trimmed_flag.empty())
			{
				trimmed_flags.emplace_back(trimmed_flag.data(), trimmed_flag.size());
			}
		}
		return uam::strings::JoinNonEmpty(trimmed_flags, " ");
	}

	inline void PrependProviderExtraFlags(AppSettings& settings, std::string_view flags)
	{
		std::string_view trimmed_flags = uam::strings::TrimAsciiView(flags);
		if (trimmed_flags.empty())
		{
			return;
		}

		std::string_view existing_flags = uam::strings::TrimAsciiView(settings.provider_extra_flags);
		if (existing_flags.empty())
		{
			settings.provider_extra_flags.assign(trimmed_flags);
			return;
		}

		std::string merged_flags;
		merged_flags.reserve(trimmed_flags.size() + 1 + existing_flags.size());
		merged_flags.append(trimmed_flags);
		merged_flags.push_back(' ');
		merged_flags.append(existing_flags);
		settings.provider_extra_flags = std::move(merged_flags);
	}

	inline AppSettings MergeProviderSettings(const ProviderProfile& profile, const AppSettings& settings)
	{
		AppSettings merged = settings;

		const std::string provider_flags = JoinFlags(profile.runtime_flags);
		PrependProviderExtraFlags(merged, provider_flags);

		return merged;
	}

	inline std::string ReplaceAll(std::string src, std::string_view from, std::string_view to)
	{
		if (from.empty())
		{
			return src;
		}

		std::size_t pos = 0;

		while ((pos = src.find(from, pos)) != std::string::npos)
		{
			src.replace(pos, from.size(), to.data(), to.size());
			pos += to.size();
		}

		return src;
	}

	inline std::string JoinShellEscapedArgs(const std::vector<std::string>& args)
	{
		return uam::shell::JoinEscapedArgs(args);
	}

	inline void AppendArgs(std::vector<std::string>& argv, const std::vector<std::string>& args)
	{
		argv.reserve(argv.size() + args.size());
		argv.insert(argv.end(), args.begin(), args.end());
	}

	inline void AppendLiteralArgs(std::vector<std::string>& argv, const std::initializer_list<std::string_view> args)
	{
		argv.reserve(argv.size() + args.size());
		for (std::string_view arg : args)
		{
			argv.emplace_back(arg.data(), arg.size());
		}
	}

	template <std::size_t N> inline void AppendLiteralArgs(std::vector<std::string>& argv, const std::array<std::string_view, N>& args)
	{
		argv.reserve(argv.size() + args.size());
		for (std::string_view arg : args)
		{
			argv.emplace_back(arg.data(), arg.size());
		}
	}

	/// <summary>True when a launch carries Plan or a captured read ceiling; read and goal-review launches are mapped to Plan first.</summary>
	inline bool LaunchHasReadCeiling(const ChatSession& chat)
	{
		return uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode) == uam::approval_modes::kPlanApprovalMode;
	}

	/// <summary>Bypass flags (YOLO equivalents) are always removed; grant flags widen what runs without asking and are removed under a read ceiling.</summary>
	enum class PermissionFlagClass { None, Grant, Bypass };
	struct PermissionFlag
	{
		PermissionFlagClass kind = PermissionFlagClass::None;
		std::size_t span = 0;
	};

	/// <summary>Classifies one provider option across the six CLIs, including --flag=value, split values, short forms and single-dash long forms.</summary>
	inline PermissionFlag ClassifyPermissionFlag(const std::vector<std::string>& args, std::size_t index)
	{
		if (index >= args.size()) return {};
		std::string option = uam::strings::ToLowerAscii(uam::strings::Trim(args[index]));
		if (option.size() < 2 || option[0] != '-' || option == "--") return {};
		std::string inline_value;
		bool has_inline = false;
		// Codex also accepts an attached config value (-csandbox_mode=danger-full-access).
		if (option.size() > 2 && option.starts_with("-c") && option[2] != '-' && option[2] != '=' && option.find('=') != std::string::npos)
		{
			inline_value = option.substr(2);
			option.resize(2);
			has_inline = true;
		}
		else if (const std::size_t equals = option.find('='); equals != std::string::npos)
		{
			inline_value = option.substr(equals + 1);
			option.resize(equals);
			has_inline = true;
		}
		// Short codex options can carry their value attached (-sdanger-full-access, -anever).
		if (!has_inline && option.size() > 2 && option[1] != '-' && (option[1] == 's' || option[1] == 'a') &&
		    option != "-sandbox" && option != "-ask-for-approval")
		{
			const std::string value = option.substr(2);
			if (value == "read-only" || value == "workspace-write" || value == "danger-full-access" ||
			    value == "untrusted" || value == "on-failure" || value == "on-request" || value == "never")
			{
				inline_value = value;
				option.resize(2);
				has_inline = true;
			}
		}
		// Go-style single-dash long flags (Antigravity) mean the same as their double-dash spelling.
		if (option.size() > 2 && option[1] != '-') option.insert(0, "-");
		const bool next_is_value = index + 1 < args.size() && !uam::strings::Trim(args[index + 1]).starts_with('-');
		const std::string value = has_inline ? inline_value : next_is_value ? uam::strings::ToLowerAscii(uam::strings::Trim(args[index + 1])) : std::string{};
		const std::size_t valued_span = has_inline ? 1 : next_is_value ? 2 : 1;
		const auto variadic_span = [&]() -> std::size_t
		{
			if (has_inline) return 1;
			std::size_t end = index + 1;
			while (end < args.size() && !uam::strings::Trim(args[end]).starts_with('-')) ++end;
			return end - index;
		};

		constexpr auto bypass = std::to_array<std::string_view>({
		    "--allow-all", "--allow-all-paths", "--allow-all-tools", "--allow-all-urls", "--dangerously-disable-sandbox",
		    "--dangerously-skip-permissions", "--allow-dangerously-skip-permissions", "--dangerously-bypass-approvals-and-sandbox",
		    "--full-auto", "--yolo", "-y", "--auto",
		});
		if (uam::ranges::Contains(bypass, std::string_view(option))) return {PermissionFlagClass::Bypass, 1};

		if (option == "--approval-mode")
			return {value == "yolo" ? PermissionFlagClass::Bypass : PermissionFlagClass::Grant, valued_span};
		if (option == "--permission-mode")
			return {value == "bypasspermissions" ? PermissionFlagClass::Bypass : PermissionFlagClass::Grant, valued_span};
		if (option == "--ask-for-approval" || option == "-a")
		{
			if (value == "never") return {PermissionFlagClass::Bypass, valued_span};
			if (value == "untrusted" || value == "on-failure" || value == "on-request") return {PermissionFlagClass::Grant, valued_span};
			return {};
		}
		if (option == "--sandbox" || option == "-s")
		{
			// Gemini's boolean --sandbox/-s and Codex read-only only restrict, so they stay.
			if (value == "danger-full-access") return {PermissionFlagClass::Bypass, valued_span};
			if (value == "workspace-write") return {PermissionFlagClass::Grant, valued_span};
			return {};
		}
		if (option == "-c" || option == "--config")
		{
			const bool approval = value.find("approval") != std::string::npos;
			const bool sandbox = value.find("sandbox") != std::string::npos;
			if ((approval && value.find("never") != std::string::npos) || (sandbox && value.find("danger-full-access") != std::string::npos))
				return {PermissionFlagClass::Bypass, valued_span};
			if (approval || sandbox || value.find("permission") != std::string::npos) return {PermissionFlagClass::Grant, valued_span};
			return {};
		}
		constexpr auto valued_grants = std::to_array<std::string_view>({
		    "--settings", "--setting-sources", "--policy", "--admin-policy", "--mode", "--profile",
		});
		if (uam::ranges::Contains(valued_grants, std::string_view(option))) return {PermissionFlagClass::Grant, valued_span};
		constexpr auto variadic_grants = std::to_array<std::string_view>({
		    "--allowedtools", "--allowed-tools", "--allow-tool", "--allow-url",
		});
		if (uam::ranges::Contains(variadic_grants, std::string_view(option))) return {PermissionFlagClass::Grant, variadic_span()};
		// Copilot autopilot approves and implements a plan; assisted approval and hook trust also approve without UAM.
		constexpr auto standalone_grants = std::to_array<std::string_view>({"--autopilot", "--assisted-approval", "--dangerously-bypass-hook-trust"});
		if (uam::ranges::Contains(standalone_grants, std::string_view(option))) return {PermissionFlagClass::Grant, 1};
		return {};
	}

	/// <summary>Inserts launch options before any bare "--", so positional provider data cannot swallow them.</summary>
	inline void InsertBeforePositional(std::vector<std::string>& argv, std::initializer_list<std::string_view> args)
	{
		const auto positional = std::find(argv.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(1, argv.size())), argv.end(), "--");
		std::vector<std::string> values;
		for (std::string_view arg : args) values.emplace_back(arg);
		argv.insert(positional, values.begin(), values.end());
	}

	inline bool HasPermissionBypassExtraFlags(const AppSettings& settings)
	{
		const std::vector<std::string> args = uam::command_line::SplitWords(settings.provider_extra_flags);
		for (std::size_t i = 0; i < args.size() && args[i] != "--"; ++i)
		{
			if (ClassifyPermissionFlag(args, i).kind == PermissionFlagClass::Bypass) return true;
		}
		return false;
	}

	/// <summary>Removes permission-class options before any bare "--"; arguments after it are positional provider data.</summary>
	inline std::vector<std::string> FilterPermissionFlags(const std::vector<std::string>& args, bool read_ceiling)
	{
		std::vector<std::string> flags;
		flags.reserve(args.size());
		for (std::size_t i = 0; i < args.size();)
		{
			if (args[i] == "--")
			{
				flags.insert(flags.end(), args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
				break;
			}
			const PermissionFlag flag = ClassifyPermissionFlag(args, i);
			if (flag.kind == PermissionFlagClass::Bypass || (read_ceiling && flag.kind == PermissionFlagClass::Grant))
			{
				i += flag.span;
				continue;
			}
			flags.push_back(args[i++]);
		}
		return flags;
	}

	inline std::vector<std::string> BuildProviderFlagsArgv(const AppSettings& settings, bool read_ceiling = false)
	{
		return FilterPermissionFlags(uam::command_line::SplitWords(settings.provider_extra_flags), read_ceiling);
	}

	inline std::vector<std::string> ProviderWorkerFlags(const ProviderProfile& profile, const AppSettings& settings)
	{
		(void)profile;
		(void)settings;
		return {};
	}

	inline bool AppendTrimmedOptionValue(std::vector<std::string>& argv, std::string_view option, std::string_view raw_value)
	{
		std::string_view trimmed_option = uam::strings::TrimAsciiView(option);
		std::string_view value = uam::strings::TrimAsciiView(raw_value);
		if (trimmed_option.empty() || value.empty())
		{
			return false;
		}

		argv.emplace_back(trimmed_option.data(), trimmed_option.size());
		argv.emplace_back(value.data(), value.size());
		return true;
	}

	inline bool AppendResumeArgs(std::vector<std::string>& argv, const ProviderProfile& profile, std::string_view native_session_id)
	{
		if (!profile.supports_resume)
		{
			return false;
		}
		return AppendTrimmedOptionValue(argv, profile.resume_argument, native_session_id);
	}

	inline void AppendTrimmedOptionValues(std::vector<std::string>& argv, std::string_view option, const std::vector<std::string>& raw_values)
	{
		for (const std::string& raw_value : raw_values)
		{
			AppendTrimmedOptionValue(argv, option, raw_value);
		}
	}

	inline std::vector<std::string> TrimNonEmptyValues(const std::vector<std::string>& raw_values)
	{
		std::vector<std::string> values;
		values.reserve(raw_values.size());

		for (const std::string& raw_value : raw_values)
		{
			std::string_view value = uam::strings::TrimAsciiView(raw_value);
			if (!value.empty())
			{
				values.emplace_back(value.data(), value.size());
			}
		}

		return values;
	}

	inline std::vector<std::string> SplitInteractiveCommandOrDefault(const ProviderProfile& profile, std::string_view default_command)
	{
		std::string_view configured_command = uam::strings::TrimAsciiView(profile.interactive_command);
		std::string_view command = configured_command.empty() ? default_command : configured_command;
		std::vector<std::string> argv = uam::command_line::SplitWords(command);
		if (argv.empty() && !default_command.empty())
		{
			argv = uam::command_line::SplitWords(default_command);
			if (argv.empty())
			{
				argv.emplace_back(default_command.data(), default_command.size());
			}
		}
		return argv;
	}


	inline std::string BuildPrompt(std::string_view user_prompt, const std::vector<std::string>& files)
	{
		const std::vector<std::string> referenced_files = TrimNonEmptyValues(files);
		std::size_t prompt_size = user_prompt.size();
		if (!referenced_files.empty())
		{
			prompt_size += kReferencedFilesHeading.size();
			for (const std::string& file : referenced_files)
			{
				prompt_size += 2 + file.size() + 1;
			}
		}

		std::string prompt;
		prompt.reserve(prompt_size);
		prompt.append(user_prompt);

		if (!referenced_files.empty())
		{
			prompt.append(kReferencedFilesHeading);

			for (const std::string& file : referenced_files)
			{
				prompt.append("- ");
				prompt.append(file);
				prompt.push_back('\n');
			}
		}

		return prompt;
	}

	inline std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings)
	{
		const std::string default_command = uam::strings::NonEmptyOrFallback(profile.id, "provider-cli");
		std::vector<std::string> argv = SplitInteractiveCommandOrDefault(profile, default_command);

		const bool read_ceiling = LaunchHasReadCeiling(chat);
		AppendArgs(argv, BuildProviderFlagsArgv(settings, read_ceiling));

		AppendResumeArgs(argv, profile, chat.native_session_id);

		// Gemini is the only runtime using this generic helper; honor a per-chat model id
		// so interactive CLI launches match the structured path (gemini supports --model/-m).
		AppendTrimmedOptionValue(argv, "--model", chat.model_id);
		if (read_ceiling) InsertBeforePositional(argv, {"--approval-mode", "plan"});

		return argv;
	}

	inline std::vector<ChatSession> LoadLocalChats(const std::filesystem::path& data_root)
	{
		return ChatRepository::LoadLocalChats(data_root);
	}

	inline bool SaveLocalChat(const std::filesystem::path& data_root, const ChatSession& chat)
	{
		return ChatRepository::SaveChat(data_root, chat);
	}

	inline bool RequestsGeminiJsonHistory(const ProviderProfile& profile)
	{
		return uam::provider_profile_constants::IsGeminiJsonHistoryAdapter(profile.history_adapter);
	}

	inline std::string RuntimeConfigurationError(const ProviderProfile& profile, const IProviderRuntime& runtime)
	{
		if (RequestsGeminiJsonHistory(profile) && !runtime.SupportsGeminiJsonHistory(profile))
		{
			const std::string provider_id = uam::strings::TrimOrFallback(profile.id, runtime.RuntimeId());
			return "Provider '" + provider_id + "' has history_adapter=" + std::string(uam::provider_profile_constants::kHistoryAdapterGeminiCliJson) + ", but the configured runtime does not support Gemini JSON history.";
		}

		return "";
	}

} // namespace uam::provider_runtime_internal
