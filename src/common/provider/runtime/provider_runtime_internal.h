#pragma once

#include "common/chat/chat_repository.h"
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

	inline std::size_t PermissionBypassFlagSpan(const std::vector<std::string>& args, std::size_t index)
	{
		if (index >= args.size()) return 0;
		const std::string option = uam::strings::ToLowerAscii(uam::strings::Trim(args[index]));
		constexpr auto standalone = std::to_array<std::string_view>({
		    "--allow-all", "--allow-all-paths", "--allow-all-tools", "--dangerously-disable-sandbox",
		    "--dangerously-skip-permissions", "--full-auto", "--yolo", "--auto",
		});
		if (uam::ranges::Contains(standalone, std::string_view(option))) return 1;
		constexpr auto assignments = std::to_array<std::string_view>({
		    "--approval-mode=yolo", "--ask-for-approval=never", "--permission-mode=bypasspermissions",
		    "--sandbox=danger-full-access",
		});
		if (uam::ranges::Contains(assignments, std::string_view(option))) return 1;
		if (index + 1 >= args.size()) return 0;
		const std::string value = uam::strings::ToLowerAscii(uam::strings::Trim(args[index + 1]));
		if ((option == "--approval-mode" && value == "yolo") ||
		    ((option == "--ask-for-approval" || option == "-a") && value == "never") ||
		    (option == "--permission-mode" && value == "bypasspermissions") ||
		    (option == "--sandbox" && value == "danger-full-access") ||
		    (option == "-c" && ((value.find("approval_policy") != std::string::npos && value.find("never") != std::string::npos) ||
		                        (value.find("sandbox_mode") != std::string::npos && value.find("danger-full-access") != std::string::npos))))
		{
			return 2;
		}
		return 0;
	}

	inline bool HasPermissionBypassExtraFlags(const AppSettings& settings)
	{
		const std::vector<std::string> args = uam::command_line::SplitWords(settings.provider_extra_flags);
		for (std::size_t i = 0; i < args.size(); ++i)
		{
			if (PermissionBypassFlagSpan(args, i) != 0) return true;
		}
		return false;
	}

	inline std::vector<std::string> BuildProviderFlagsArgv(const AppSettings& settings)
	{
		const std::vector<std::string> extra_flags = uam::command_line::SplitWords(settings.provider_extra_flags);
		std::vector<std::string> flags;
		flags.reserve(extra_flags.size());
		for (std::size_t i = 0; i < extra_flags.size();)
		{
			const std::size_t rejected_span = PermissionBypassFlagSpan(extra_flags, i);
			if (rejected_span != 0)
			{
				i += rejected_span;
				continue;
			}
			flags.push_back(extra_flags[i++]);
		}
		return flags;
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

	inline std::vector<std::string> BuildFlagsArgv(const AppSettings& settings)
	{
		return BuildProviderFlagsArgv(settings);
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

		AppendArgs(argv, BuildFlagsArgv(settings));

		AppendResumeArgs(argv, profile, chat.native_session_id);

		// Gemini is the only runtime using this generic helper; honor a per-chat model id
		// so interactive CLI launches match the structured path (gemini supports --model/-m).
		AppendTrimmedOptionValue(argv, "--model", chat.model_id);

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
