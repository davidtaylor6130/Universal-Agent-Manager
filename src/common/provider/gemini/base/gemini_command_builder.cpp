#include "common/provider/gemini/base/gemini_command_builder.h"
#include "common/utils/command_line_words.h"

#include <sstream>

namespace
{

	std::string ReplaceAll(std::string src, const std::string& from, const std::string& to)
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

	std::string ShellEscape(const std::string& value)
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

	std::string JoinShellEscapedFiles(const std::vector<std::string>& files)
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

} // namespace

std::string GeminiCommandBuilder::BuildPrompt(const std::string& user_prompt, const std::vector<std::string>& files)
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

std::vector<std::string> GeminiCommandBuilder::BuildFlagsArgv(const AppSettings& settings)
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

std::string GeminiCommandBuilder::BuildFlagsShell(const AppSettings& settings)
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

std::string GeminiCommandBuilder::BuildCommand(const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id)
{
	std::string command = settings.provider_command_template.empty() ? "gemini -r \"{resume}\" {flags} -p {prompt}" : settings.provider_command_template;
	const bool has_prompt_placeholder = (command.find("{prompt}") != std::string::npos);
	const bool has_files_placeholder = (command.find("{files}") != std::string::npos);
	const bool has_resume_placeholder = (command.find("{resume}") != std::string::npos);
	const bool has_flags_placeholder = (command.find("{flags}") != std::string::npos);
	const bool has_model_placeholder = (command.find("{model}") != std::string::npos);

	const std::string resume_fragment = resume_session_id.empty() ? "" : (ShellEscape(resume_session_id));
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

std::vector<std::string> GeminiCommandBuilder::BuildInteractiveArgv(const ChatSession& chat, const AppSettings& settings)
{
	std::vector<std::string> argv;
	argv.push_back("gemini");
	const std::vector<std::string> flags = BuildFlagsArgv(settings);
	argv.insert(argv.end(), flags.begin(), flags.end());

	if (chat.uses_native_session && !chat.native_session_id.empty())
	{
		argv.push_back("-r");
		argv.push_back(chat.native_session_id);
	}

	return argv;
}
