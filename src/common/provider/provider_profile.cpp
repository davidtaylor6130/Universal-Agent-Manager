#include "common/provider/provider_profile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace
{
	namespace fs = std::filesystem;

	fs::path ProviderFilePath(const fs::path& data_root)
	{
		return data_root / "providers.txt";
	}

	bool WriteTextFile(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);

		if (!out.good())
		{
			return false;
		}

		out << content;
		return out.good();
	}

	std::string ReadTextFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);

		if (!in.good())
		{
			return "";
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	std::string Trim(const std::string& value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	std::vector<std::string> SplitCsv(const std::string& csv)
	{
		std::vector<std::string> out;
		std::istringstream in(csv);
		std::string token;

		while (std::getline(in, token, ','))
		{
			token = Trim(token);

			if (!token.empty())
			{
				out.push_back(token);
			}
		}

		return out;
	}

	std::string JoinCsv(const std::vector<std::string>& values)
	{
		std::ostringstream out;
		bool first = true;

		for (const std::string& value : values)
		{
			if (!first)
			{
				out << ",";
			}

			out << value;
			first = false;
		}

		return out.str();
	}

	bool IsSameId(const std::string& lhs, const std::string& rhs)
	{
		auto lower = [](std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return value;
		};

		return lower(lhs) == lower(rhs);
	}

	std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	bool ParseBool(const std::string& value)
	{
		std::string lowered = LowerAscii(Trim(value));
		return lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes";
	}

} // namespace

ProviderProfile ProviderProfileStore::DefaultGeminiProfile()
{
	ProviderProfile profile;
	profile.id = "gemini-structured";
	profile.title = "Gemini (Structured)";
	profile.execution_mode = "cli";
	profile.output_mode = "structured";
	profile.command_template = "gemini {resume} {flags} -p {prompt}";
	profile.interactive_command = "gemini";
	profile.supports_interactive = false;
	profile.supports_resume = true;
	profile.resume_argument = "-r";
	profile.history_adapter = "gemini-cli-json";
	profile.prompt_bootstrap = "gemini-at-path";
	profile.prompt_bootstrap_path = "@.gemini/gemini.md";
	profile.user_message_types = {"user"};
	profile.assistant_message_types = {"assistant", "model", "gemini"};
	return profile;
}

std::vector<ProviderProfile> ProviderProfileStore::BuiltInProfiles()
{
	std::vector<ProviderProfile> profiles;
	profiles.push_back(DefaultGeminiProfile());

	ProviderProfile gemini_cli;
	gemini_cli.id = "gemini-cli";
	gemini_cli.title = "Gemini CLI";
	gemini_cli.execution_mode = "cli";
	gemini_cli.output_mode = "cli";
	gemini_cli.command_template = "gemini {resume} {flags} {prompt}";
	gemini_cli.interactive_command = "gemini";
	gemini_cli.supports_interactive = true;
	gemini_cli.supports_resume = true;
	gemini_cli.resume_argument = "-r";
	gemini_cli.history_adapter = "gemini-cli-json";
	gemini_cli.prompt_bootstrap = "gemini-at-path";
	gemini_cli.prompt_bootstrap_path = "@.gemini/gemini.md";
	gemini_cli.user_message_types = {"user"};
	gemini_cli.assistant_message_types = {"assistant", "model", "gemini"};
	profiles.push_back(std::move(gemini_cli));

	ProviderProfile codex;
	codex.id = "codex-cli";
	codex.title = "OpenAI Codex CLI";
	codex.execution_mode = "cli";
	codex.output_mode = "cli";
	codex.command_template = "codex exec {flags} {prompt}";
	codex.interactive_command = "codex";
	codex.supports_interactive = true;
	codex.supports_resume = false;
	codex.resume_argument.clear();
	codex.history_adapter = "local-only";
	codex.prompt_bootstrap = "prepend";
	codex.user_message_types = {"user"};
	codex.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(codex));

	ProviderProfile claude;
	claude.id = "claude-cli";
	claude.title = "Claude CLI";
	claude.execution_mode = "cli";
	claude.output_mode = "cli";
	claude.command_template = "claude {flags} {prompt}";
	claude.interactive_command = "claude";
	claude.supports_interactive = true;
	claude.supports_resume = false;
	claude.resume_argument.clear();
	claude.history_adapter = "local-only";
	claude.prompt_bootstrap = "prepend";
	claude.user_message_types = {"user", "human"};
	claude.assistant_message_types = {"assistant", "model"};
	profiles.push_back(std::move(claude));

	ProviderProfile opencode;
	opencode.id = "opencode-cli";
	opencode.title = "OpenCode CLI";
	opencode.execution_mode = "cli";
	opencode.output_mode = "cli";
	opencode.command_template = "opencode {flags} {prompt}";
	opencode.interactive_command = "opencode";
	opencode.supports_interactive = true;
	opencode.supports_resume = false;
	opencode.resume_argument.clear();
	opencode.history_adapter = "local-only";
	opencode.prompt_bootstrap = "prepend";
	opencode.user_message_types = {"user"};
	opencode.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(opencode));

	ProviderProfile opencode_local;
	opencode_local.id = "opencode-local";
	opencode_local.title = "OpenCode (Fully Local)";
	opencode_local.execution_mode = "cli";
	opencode_local.output_mode = "cli";
	opencode_local.command_template = "opencode {flags} {prompt}";
	opencode_local.interactive_command = "opencode";
	opencode_local.supports_interactive = true;
	opencode_local.supports_resume = false;
	opencode_local.resume_argument.clear();
	opencode_local.history_adapter = "local-only";
	opencode_local.prompt_bootstrap = "prepend";
	opencode_local.user_message_types = {"user"};
	opencode_local.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(opencode_local));

	ProviderProfile ollama_engine;
	ollama_engine.id = "ollama-engine";
	ollama_engine.title = "Ollama Engine (Local)";
	ollama_engine.execution_mode = "internal-engine";
	ollama_engine.output_mode = "structured";
	ollama_engine.command_template.clear();
	ollama_engine.interactive_command.clear();
	ollama_engine.supports_interactive = false;
	ollama_engine.supports_resume = false;
	ollama_engine.resume_argument.clear();
	ollama_engine.history_adapter = "local-only";
	ollama_engine.prompt_bootstrap = "prepend";
	ollama_engine.user_message_types = {"user"};
	ollama_engine.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(ollama_engine));

	return profiles;
}

void ProviderProfileStore::EnsureDefaultProfile(std::vector<ProviderProfile>& profiles)
{
	const std::vector<ProviderProfile> built_ins = BuiltInProfiles();

	for (const ProviderProfile& built_in : built_ins)
	{
		bool found = false;

		for (const ProviderProfile& profile : profiles)
		{
			if (IsSameId(profile.id, built_in.id))
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			profiles.push_back(built_in);
		}
	}
}

const ProviderProfile* ProviderProfileStore::FindById(const std::vector<ProviderProfile>& profiles, const std::string& id)
{
	for (const ProviderProfile& profile : profiles)
	{
		if (IsSameId(profile.id, id))
		{
			return &profile;
		}
	}

	return nullptr;
}

ProviderProfile* ProviderProfileStore::FindById(std::vector<ProviderProfile>& profiles, const std::string& id)
{
	for (ProviderProfile& profile : profiles)
	{
		if (IsSameId(profile.id, id))
		{
			return &profile;
		}
	}

	return nullptr;
}

std::vector<ProviderProfile> ProviderProfileStore::Load(const std::filesystem::path& data_root)
{
	std::vector<ProviderProfile> profiles;
	const fs::path file = ProviderFilePath(data_root);

	if (!fs::exists(file))
	{
		profiles = BuiltInProfiles();
		return profiles;
	}

	std::istringstream lines(ReadTextFile(file));
	std::string line;
	ProviderProfile current;
	bool in_profile = false;

	while (std::getline(lines, line))
	{
		if (line == "[provider]")
		{
			if (in_profile && !Trim(current.id).empty())
			{
				profiles.push_back(current);
			}

			current = ProviderProfile{};
			// Clear defaults that should be inferred from built-ins when omitted in legacy files.
			current.execution_mode.clear();
			current.output_mode.clear();
			current.history_adapter.clear();
			current.prompt_bootstrap.clear();
			current.prompt_bootstrap_path.clear();
			in_profile = true;
			continue;
		}

		if (!in_profile || line.empty())
		{
			continue;
		}

		const auto eq = line.find('=');

		if (eq == std::string::npos)
		{
			continue;
		}

		const std::string key = Trim(line.substr(0, eq));
		const std::string value = Trim(line.substr(eq + 1));

		if (key == "id")
		{
			current.id = value;
		}
		else if (key == "title")
		{
			current.title = value;
		}
		else if (key == "execution_mode")
		{
			current.execution_mode = value;
		}
		else if (key == "output_mode")
		{
			current.output_mode = value;
		}
		else if (key == "command_template")
		{
			current.command_template = value;
		}
		else if (key == "interactive_command")
		{
			current.interactive_command = value;
		}
		else if (key == "supports_interactive")
		{
			current.supports_interactive = ParseBool(value);
		}
		else if (key == "supports_resume")
		{
			current.supports_resume = ParseBool(value);
		}
		else if (key == "runtime_flag")
		{
			if (!value.empty())
			{
				current.runtime_flags.push_back(value);
			}
		}
		else if (key == "runtime_flags")
		{
			std::vector<std::string> flags = SplitCsv(value);
			current.runtime_flags.insert(current.runtime_flags.end(), flags.begin(), flags.end());
		}
		else if (key == "resume_argument")
		{
			current.resume_argument = value;
		}
		else if (key == "history_adapter")
		{
			current.history_adapter = value;
		}
		else if (key == "prompt_bootstrap")
		{
			current.prompt_bootstrap = value;
		}
		else if (key == "prompt_bootstrap_path")
		{
			current.prompt_bootstrap_path = value;
		}
		else if (key == "user_types")
		{
			current.user_message_types = SplitCsv(value);
		}
		else if (key == "assistant_types")
		{
			current.assistant_message_types = SplitCsv(value);
		}
	}

	if (in_profile && !Trim(current.id).empty())
	{
		profiles.push_back(current);
	}

	if (profiles.empty())
	{
		profiles = BuiltInProfiles();
	}

	EnsureDefaultProfile(profiles);
	const std::vector<ProviderProfile> built_ins = BuiltInProfiles();
	const auto find_builtin = [&](const std::string& id) -> const ProviderProfile*
	{
		for (const ProviderProfile& built_in : built_ins)
		{
			if (IsSameId(built_in.id, id))
			{
				return &built_in;
			}
		}

		return nullptr;
	};

	for (ProviderProfile& profile : profiles)
	{
		const ProviderProfile* built_in = find_builtin(profile.id);

		if (built_in != nullptr)
		{
			if (Trim(profile.title).empty())
			{
				profile.title = built_in->title;
			}

			if (Trim(profile.execution_mode).empty())
			{
				profile.execution_mode = built_in->execution_mode;
			}

			if (Trim(profile.output_mode).empty())
			{
				profile.output_mode = built_in->output_mode;
			}

			if (Trim(profile.command_template).empty())
			{
				profile.command_template = built_in->command_template;
			}

			if (Trim(profile.interactive_command).empty())
			{
				profile.interactive_command = built_in->interactive_command;
			}

			if (Trim(profile.resume_argument).empty())
			{
				profile.resume_argument = built_in->resume_argument;
			}

			if (Trim(profile.history_adapter).empty())
			{
				profile.history_adapter = built_in->history_adapter;
			}

			if (Trim(profile.prompt_bootstrap).empty())
			{
				profile.prompt_bootstrap = built_in->prompt_bootstrap;
			}

			if (Trim(profile.prompt_bootstrap_path).empty())
			{
				profile.prompt_bootstrap_path = built_in->prompt_bootstrap_path;
			}

			if (profile.user_message_types.empty())
			{
				profile.user_message_types = built_in->user_message_types;
			}

			if (profile.assistant_message_types.empty())
			{
				profile.assistant_message_types = built_in->assistant_message_types;
			}
		}

		if (Trim(profile.execution_mode).empty())
		{
			profile.execution_mode = (built_in != nullptr) ? built_in->execution_mode : "cli";
		}

		if (Trim(profile.output_mode).empty())
		{
			if (built_in != nullptr)
			{
				profile.output_mode = built_in->output_mode;
			}
			else if (Trim(profile.execution_mode) != "cli")
			{
				profile.output_mode = "structured";
			}
			else
			{
				profile.output_mode = profile.supports_interactive ? "cli" : "structured";
			}
		}

		if (Trim(profile.history_adapter).empty())
		{
			profile.history_adapter = (built_in != nullptr) ? built_in->history_adapter : "local-only";
		}

		if (Trim(profile.prompt_bootstrap).empty())
		{
			profile.prompt_bootstrap = (built_in != nullptr) ? built_in->prompt_bootstrap : "prepend";
		}

		if (LowerAscii(Trim(profile.output_mode)) == "structured")
		{
			profile.supports_interactive = false;
		}

		if (Trim(profile.execution_mode) != "cli")
		{
			profile.output_mode = "structured";
			profile.supports_interactive = false;
			profile.supports_resume = false;
		}
	}

	return profiles;
}

bool ProviderProfileStore::Save(const std::filesystem::path& data_root, const std::vector<ProviderProfile>& profiles)
{
	std::error_code ec;
	fs::create_directories(data_root, ec);

	std::ostringstream out;

	for (const ProviderProfile& profile : profiles)
	{
		if (Trim(profile.id).empty())
		{
			continue;
		}

		out << "[provider]\n";
		out << "id=" << profile.id << "\n";
		out << "title=" << profile.title << "\n";
		out << "execution_mode=" << profile.execution_mode << "\n";
		out << "output_mode=" << profile.output_mode << "\n";
		out << "command_template=" << profile.command_template << "\n";
		out << "interactive_command=" << profile.interactive_command << "\n";
		out << "supports_interactive=" << (profile.supports_interactive ? "1" : "0") << "\n";
		out << "supports_resume=" << (profile.supports_resume ? "1" : "0") << "\n";
		out << "runtime_flags=" << JoinCsv(profile.runtime_flags) << "\n";
		out << "resume_argument=" << profile.resume_argument << "\n";
		out << "history_adapter=" << profile.history_adapter << "\n";
		out << "prompt_bootstrap=" << profile.prompt_bootstrap << "\n";
		out << "prompt_bootstrap_path=" << profile.prompt_bootstrap_path << "\n";
		out << "user_types=" << JoinCsv(profile.user_message_types) << "\n";
		out << "assistant_types=" << JoinCsv(profile.assistant_message_types) << "\n\n";
	}

	return WriteTextFile(ProviderFilePath(data_root), out.str());
}
