#pragma once

#include "common/chat/chat_repository.h"
#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_profile_constants.h"
#include "common/provider/provider_runtime.h"
#include "common/utils/command_line_words.h"
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
	inline constexpr std::string_view kGenericYoloFlag = "--yolo";
	inline constexpr std::string_view kReferencedFilesHeading = "\n\nReferenced files:\n";

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

	inline AppSettings MergeProviderSettingsWithoutGenericYolo(const ProviderProfile& profile, const AppSettings& settings)
	{
		AppSettings merged = MergeProviderSettings(profile, settings);
		merged.provider_yolo_mode = false;
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

	inline std::vector<std::string> BuildProviderFlagsArgv(const AppSettings& settings, std::string_view yolo_flag)
	{
		const std::vector<std::string> extra_flags = uam::command_line::SplitWords(settings.provider_extra_flags);
		std::string_view trimmed_yolo_flag = uam::strings::TrimAsciiView(yolo_flag);
		std::vector<std::string> flags;
		flags.reserve(extra_flags.size() + (settings.provider_yolo_mode && !trimmed_yolo_flag.empty() ? 1 : 0));

		if (settings.provider_yolo_mode && !trimmed_yolo_flag.empty())
		{
			flags.emplace_back(trimmed_yolo_flag.data(), trimmed_yolo_flag.size());
		}

		AppendArgs(flags, extra_flags);
		return flags;
	}

	inline std::vector<std::string> ProviderWorkerFlags(const ProviderProfile& profile, const AppSettings& settings)
	{
		const AppSettings provider_settings = MergeProviderSettingsWithoutGenericYolo(profile, settings);
		return BuildProviderFlagsArgv(provider_settings, "");
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
		return BuildProviderFlagsArgv(settings, kGenericYoloFlag);
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
