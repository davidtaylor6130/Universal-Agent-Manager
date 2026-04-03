#pragma once

#include "common/chat/chat_repository.h"
#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/utils/command_line_words.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#ifndef UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
#define UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED 1
#endif

#ifndef UAM_ENABLE_RUNTIME_GEMINI_CLI
#define UAM_ENABLE_RUNTIME_GEMINI_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_CODEX_CLI
#define UAM_ENABLE_RUNTIME_CODEX_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_CLAUDE_CLI
#define UAM_ENABLE_RUNTIME_CLAUDE_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_CLI
#define UAM_ENABLE_RUNTIME_OPENCODE_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
#define UAM_ENABLE_RUNTIME_OPENCODE_LOCAL 1
#endif

#ifndef UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
#define UAM_ENABLE_RUNTIME_OLLAMA_ENGINE 1
#endif

namespace provider_runtime_internal
{

	inline bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < lhs.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
			{
				return false;
			}
		}

		return true;
	}

	inline std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	inline bool AnyTypeMatches(const std::vector<std::string>& types, const std::string& value)
	{
		for (const std::string& candidate : types)
		{
			if (EqualsIgnoreCase(candidate, value))
			{
				return true;
			}
		}

		return false;
	}

	inline MessageRole RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type)
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
		std::ostringstream out;
		bool first = true;

		for (const std::string& flag : flags)
		{
			if (flag.empty())
			{
				continue;
			}

			if (!first)
			{
				out << ' ';
			}

			out << flag;
			first = false;
		}

		return out.str();
	}

	inline AppSettings MergeProviderSettings(const ProviderProfile& profile, const AppSettings& settings)
	{
		AppSettings merged = settings;

		if (!profile.command_template.empty())
		{
			merged.provider_command_template = profile.command_template;
		}

		const std::string provider_flags = JoinFlags(profile.runtime_flags);

		if (!provider_flags.empty() && merged.provider_extra_flags.empty())
		{
			merged.provider_extra_flags = provider_flags;
		}
		else if (!provider_flags.empty())
		{
			merged.provider_extra_flags = provider_flags + " " + merged.provider_extra_flags;
		}

		return merged;
	}

	inline std::string ReplaceAll(std::string src, const std::string& from, const std::string& to)
	{
		if (from.empty())
		{
			return src;
		}

		std::size_t pos = 0;

		while ((pos = src.find(from, pos)) != std::string::npos)
		{
			src.replace(pos, from.size(), to);
			pos += to.size();
		}

		return src;
	}

	inline std::string ShellEscape(const std::string& value)
	{
#if defined(_WIN32)
		std::string escaped = "\"";

		for (const char ch : value)
		{
			if (ch == '"')
			{
				escaped += "\"\"";
			}
			else if (ch == '%')
			{
				escaped += "%%";
			}
			else if (ch == '\r' || ch == '\n')
			{
				escaped.push_back(' ');
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		escaped.push_back('"');
		return escaped;
#else
		std::string escaped = "'";

		for (const char ch : value)
		{
			if (ch == '\'')
			{
				escaped += "'\\''";
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		escaped.push_back('\'');
		return escaped;
#endif
	}

	inline std::vector<std::string> BuildFlagsArgv(const AppSettings& settings)
	{
		std::vector<std::string> flags;

		if (settings.provider_yolo_mode)
		{
			flags.push_back("--yolo");
		}

		const std::vector<std::string> extra_flags = SplitCommandLineWords(settings.provider_extra_flags);
		flags.insert(flags.end(), extra_flags.begin(), extra_flags.end());
		return flags;
	}

	inline std::string BuildFlagsShell(const AppSettings& settings)
	{
		const std::vector<std::string> flags = BuildFlagsArgv(settings);
		std::ostringstream out;
		bool first = true;

		for (const std::string& flag : flags)
		{
			if (!first)
			{
				out << ' ';
			}

			out << ShellEscape(flag);
			first = false;
		}

		return out.str();
	}

	inline std::string JoinShellEscapedFiles(const std::vector<std::string>& files)
	{
		std::ostringstream out;
		bool first = true;

		for (const std::string& file : files)
		{
			if (!first)
			{
				out << ' ';
			}

			out << ShellEscape(file);
			first = false;
		}

		return out.str();
	}

	inline std::string BuildPrompt(const std::string& user_prompt, const std::vector<std::string>& files)
	{
		std::ostringstream prompt;
		prompt << user_prompt;

		if (!files.empty())
		{
			prompt << "\n\nReferenced files:\n";

			for (const std::string& file : files)
			{
				prompt << "- " << file << "\n";
			}
		}

		return prompt.str();
	}

	inline std::string BuildCommandFromTemplate(const AppSettings& settings,
	                                            const std::string& prompt,
	                                            const std::vector<std::string>& files,
	                                            const std::string& resume_session_id,
	                                            const std::string& default_template)
	{
		std::string command = settings.provider_command_template.empty() ? default_template : settings.provider_command_template;
		const bool has_prompt_placeholder = (command.find("{prompt}") != std::string::npos);
		const bool has_files_placeholder = (command.find("{files}") != std::string::npos);
		const bool has_resume_placeholder = (command.find("{resume}") != std::string::npos);
		const bool has_flags_placeholder = (command.find("{flags}") != std::string::npos);
		const bool has_model_placeholder = (command.find("{model}") != std::string::npos);

		const std::string resume_fragment = resume_session_id.empty() ? "" : ShellEscape(resume_session_id);
		const std::string flags_fragment = BuildFlagsShell(settings);
		const std::string model_fragment = settings.selected_model_id.empty() ? "" : ShellEscape(settings.selected_model_id);
		const std::string files_fragment = JoinShellEscapedFiles(files);
		const std::string prompt_fragment = ShellEscape(prompt);

		command = ReplaceAll(command, "{prompt}", prompt_fragment);
		command = ReplaceAll(command, "{files}", files_fragment);
		command = ReplaceAll(command, "{resume}", resume_fragment);
		command = ReplaceAll(command, "{flags}", flags_fragment);
		command = ReplaceAll(command, "{model}", model_fragment);

		if (!has_prompt_placeholder && !prompt_fragment.empty())
		{
			command += " ";
			command += prompt_fragment;
		}

		if (!has_files_placeholder && !files_fragment.empty())
		{
			command += " ";
			command += files_fragment;
		}

		if (!has_resume_placeholder && !resume_fragment.empty())
		{
			command += " ";
			command += resume_fragment;
		}

		if (!has_flags_placeholder && !flags_fragment.empty())
		{
			command += " ";
			command += flags_fragment;
		}

		if (!has_model_placeholder && !model_fragment.empty())
		{
			command += " ";
			command += model_fragment;
		}

		return command;
	}

	inline std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings)
	{
		std::vector<std::string> argv = SplitCommandLineWords(profile.interactive_command);

		if (argv.empty())
		{
			argv.push_back(profile.id.empty() ? "provider-cli" : profile.id);
		}

		const std::vector<std::string> flags = BuildFlagsArgv(settings);
		argv.insert(argv.end(), flags.begin(), flags.end());

		if (profile.supports_resume && chat.uses_native_session && !chat.native_session_id.empty() && !profile.resume_argument.empty())
		{
			argv.push_back(profile.resume_argument);
			argv.push_back(chat.native_session_id);
		}

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
		return EqualsIgnoreCase(profile.history_adapter, "gemini-cli-json");
	}

	inline std::string RuntimeConfigurationError(const ProviderProfile& profile, const IProviderRuntime& runtime)
	{
		if (RequestsGeminiJsonHistory(profile) && !runtime.SupportsGeminiJsonHistory(profile))
		{
			const std::string provider_id = profile.id.empty() ? std::string(runtime.RuntimeId()) : profile.id;
			return "Provider '" + provider_id + "' has history_adapter=gemini-cli-json, but only gemini-structured and gemini-cli support Gemini JSON history.";
		}

		return "";
	}

} // namespace provider_runtime_internal
