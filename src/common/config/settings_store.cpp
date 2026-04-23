#include "common/config/settings_store.h"

#include "common/config/line_value_codec.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/io_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace
{

	std::string ReadTextFile(const std::filesystem::path& path)
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

	std::string ToLower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	bool ParseBool(const std::string& value, const bool fallback)
	{
		const std::string lowered = ToLower(value);
		if (lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes")
		{
			return true;
		}
		if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no")
		{
			return false;
		}
		return fallback;
	}

	int ParseInt(const std::string& value, const int fallback)
	{
		try
		{
			return std::stoi(value);
		}
		catch (...)
		{
			return fallback;
		}
	}

	float ParseFloat(const std::string& value, const float fallback)
	{
		try
		{
			return std::stof(value);
		}
		catch (...)
		{
			return fallback;
		}
	}

	std::string NormalizeProviderId(const std::string& value);

	std::vector<std::string> Split(const std::string& value, const char delimiter)
	{
		std::vector<std::string> parts;
		std::string current;
		std::istringstream stream(value);
		while (std::getline(stream, current, delimiter))
		{
			parts.push_back(current);
		}
		return parts;
	}

	std::string EncodeMemoryWorkerBindings(const std::map<std::string, MemoryWorkerBinding>& bindings)
	{
		std::ostringstream out;
		bool first = true;
		for (const auto& entry : bindings)
		{
			if (entry.first.empty() || entry.second.worker_provider_id.empty())
			{
				continue;
			}
			if (!first)
			{
				out << ';';
			}
			out << uam::EncodeLineValue(entry.first) << ','
			    << uam::EncodeLineValue(entry.second.worker_provider_id) << ','
			    << uam::EncodeLineValue(entry.second.worker_model_id);
			first = false;
		}
		return out.str();
	}

	void DecodeMemoryWorkerBindings(const std::string& value, std::map<std::string, MemoryWorkerBinding>& bindings)
	{
		bindings.clear();
		for (const std::string& encoded_entry : Split(value, ';'))
		{
			const std::vector<std::string> fields = Split(encoded_entry, ',');
			if (fields.size() < 2)
			{
				continue;
			}

			const std::string chat_provider_id = uam::DecodeLineValue(fields[0]);
			const std::string worker_provider_id = NormalizeProviderId(uam::DecodeLineValue(fields[1]));
			const std::string worker_model_id = fields.size() >= 3 ? uam::DecodeLineValue(fields[2]) : "";
			if (chat_provider_id.empty() || worker_provider_id.empty())
			{
				continue;
			}

			bindings[chat_provider_id] = MemoryWorkerBinding{worker_provider_id, worker_model_id};
		}
	}

	std::string NormalizeThemeId(std::string value)
	{
		value = ToLower(value);
		if (value == "light")
		{
			return "light";
		}
		if (value == "system")
		{
			return "system";
		}
		return "dark";
	}

	std::string NormalizeProviderId(const std::string& value)
	{
		const std::string lowered = ToLower(value);
#if UAM_ENABLE_RUNTIME_CODEX_CLI
		if (lowered == "codex" || lowered == "codex-cli")
		{
			return "codex-cli";
		}
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		if (lowered == "claude" || lowered == "claude-code" || lowered == "claude-cli")
		{
			return "claude-cli";
		}
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		if (lowered == "gemini" || lowered == "gemini-cli")
		{
			return "gemini-cli";
		}
#endif
		return provider_build_config::FirstEnabledProviderId();
	}

	void ClampSettings(AppSettings& settings)
	{
		settings.active_provider_id = NormalizeProviderId(settings.active_provider_id);
		settings.runtime_backend = "provider-cli";
		settings.provider_command_template = settings.provider_command_template.empty()
			? "gemini {resume} {flags} {prompt}"
			: settings.provider_command_template;
		settings.gemini_command_template = settings.provider_command_template;
		settings.gemini_yolo_mode = settings.provider_yolo_mode;
		settings.gemini_extra_flags = settings.provider_extra_flags;
		settings.cli_idle_timeout_seconds = std::clamp(settings.cli_idle_timeout_seconds, 30, 3600);
		settings.ui_theme = NormalizeThemeId(settings.ui_theme);
		settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
		settings.sidebar_width = std::clamp(settings.sidebar_width, 220.0f, 600.0f);
		settings.window_width = std::clamp(settings.window_width, 960, 8192);
		settings.window_height = std::clamp(settings.window_height, 620, 8192);
		settings.memory_idle_delay_seconds = std::clamp(settings.memory_idle_delay_seconds, 30, 3600);
		settings.memory_recall_budget_bytes = std::clamp(settings.memory_recall_budget_bytes, 512, 8192);

		for (const std::string& provider_id : {std::string("gemini-cli"), std::string("codex-cli"), std::string("claude-cli")})
		{
			if (settings.memory_worker_bindings.find(provider_id) == settings.memory_worker_bindings.end())
			{
				settings.memory_worker_bindings[provider_id] = MemoryWorkerBinding{NormalizeProviderId(provider_id), ""};
			}
		}

		if (!settings.remember_last_chat)
		{
			settings.last_selected_chat_id.clear();
		}
	}

} // namespace

bool SettingsStore::Save(const std::filesystem::path& settings_file, const AppSettings& settings, const CenterViewMode center_view_mode)
{
	std::error_code ec;
	std::filesystem::create_directories(settings_file.parent_path(), ec);

	AppSettings normalized = settings;
	ClampSettings(normalized);

	std::ostringstream lines;
	lines << "active_provider_id=" << uam::EncodeLineValue(normalized.active_provider_id) << '\n';
	lines << "provider_command_template=" << uam::EncodeLineValue(normalized.provider_command_template) << '\n';
	lines << "provider_yolo_mode=" << (normalized.provider_yolo_mode ? "1" : "0") << '\n';
	lines << "provider_extra_flags=" << uam::EncodeLineValue(normalized.provider_extra_flags) << '\n';
	lines << "runtime_backend=provider-cli\n";
	lines << "cli_idle_timeout_seconds=" << normalized.cli_idle_timeout_seconds << '\n';
	lines << "center_view_mode=" << ViewModeToString(center_view_mode) << '\n';
	lines << "ui_theme=" << uam::EncodeLineValue(normalized.ui_theme) << '\n';
	lines << "confirm_delete_chat=" << (normalized.confirm_delete_chat ? "1" : "0") << '\n';
	lines << "confirm_delete_folder=" << (normalized.confirm_delete_folder ? "1" : "0") << '\n';
	lines << "remember_last_chat=" << (normalized.remember_last_chat ? "1" : "0") << '\n';
	lines << "last_selected_chat_id=" << uam::EncodeLineValue(normalized.last_selected_chat_id) << '\n';
	lines << "ui_scale_multiplier=" << normalized.ui_scale_multiplier << '\n';
	lines << "sidebar_width=" << normalized.sidebar_width << '\n';
	lines << "window_width=" << normalized.window_width << '\n';
	lines << "window_height=" << normalized.window_height << '\n';
	lines << "window_maximized=" << (normalized.window_maximized ? "1" : "0") << '\n';
	lines << "memory_enabled_default=" << (normalized.memory_enabled_default ? "1" : "0") << '\n';
	lines << "memory_idle_delay_seconds=" << normalized.memory_idle_delay_seconds << '\n';
	lines << "memory_recall_budget_bytes=" << normalized.memory_recall_budget_bytes << '\n';
	lines << "memory_worker_bindings=" << EncodeMemoryWorkerBindings(normalized.memory_worker_bindings) << '\n';
	return uam::io::WriteTextFile(settings_file, lines.str());
}

void SettingsStore::Load(const std::filesystem::path& settings_file, AppSettings& settings, CenterViewMode& center_view_mode)
{
	if (!std::filesystem::exists(settings_file))
	{
		ClampSettings(settings);
		center_view_mode = CenterViewMode::CliConsole;
		return;
	}

	const std::string text = ReadTextFile(settings_file);
	std::istringstream lines(text);
	std::string line;
	bool has_provider_command_template = false;

	while (std::getline(lines, line))
	{
		const auto equals_at = line.find('=');
		if (equals_at == std::string::npos)
		{
			continue;
		}

		const std::string key = line.substr(0, equals_at);
		const std::string value = line.substr(equals_at + 1);
		const std::string decoded_value = uam::DecodeLineValue(value);

		if (key == "active_provider_id")
		{
			settings.active_provider_id = decoded_value;
		}
		else if (key == "provider_command_template")
		{
			settings.provider_command_template = decoded_value;
			has_provider_command_template = true;
		}
		else if (key == "provider_yolo_mode")
		{
			settings.provider_yolo_mode = ParseBool(value, settings.provider_yolo_mode);
		}
		else if (key == "provider_extra_flags")
		{
			settings.provider_extra_flags = decoded_value;
		}
		else if (key == "gemini_command_template")
		{
			settings.gemini_command_template = decoded_value;
		}
		else if (key == "gemini_yolo_mode")
		{
			settings.gemini_yolo_mode = ParseBool(value, settings.gemini_yolo_mode);
		}
		else if (key == "gemini_extra_flags")
		{
			settings.gemini_extra_flags = decoded_value;
		}
		else if (key == "cli_idle_timeout_seconds")
		{
			settings.cli_idle_timeout_seconds = ParseInt(value, settings.cli_idle_timeout_seconds);
		}
		else if (key == "center_view_mode")
		{
			center_view_mode = ViewModeFromString(value);
		}
		else if (key == "ui_theme")
		{
			settings.ui_theme = NormalizeThemeId(decoded_value);
		}
		else if (key == "confirm_delete_chat")
		{
			settings.confirm_delete_chat = ParseBool(value, settings.confirm_delete_chat);
		}
		else if (key == "confirm_delete_folder")
		{
			settings.confirm_delete_folder = ParseBool(value, settings.confirm_delete_folder);
		}
		else if (key == "remember_last_chat")
		{
			settings.remember_last_chat = ParseBool(value, settings.remember_last_chat);
		}
		else if (key == "last_selected_chat_id")
		{
			settings.last_selected_chat_id = decoded_value;
		}
		else if (key == "ui_scale_multiplier")
		{
			settings.ui_scale_multiplier = ParseFloat(value, settings.ui_scale_multiplier);
		}
		else if (key == "sidebar_width")
		{
			settings.sidebar_width = ParseFloat(value, settings.sidebar_width);
		}
		else if (key == "window_width")
		{
			settings.window_width = ParseInt(value, settings.window_width);
		}
		else if (key == "window_height")
		{
			settings.window_height = ParseInt(value, settings.window_height);
		}
		else if (key == "window_maximized")
		{
			settings.window_maximized = ParseBool(value, settings.window_maximized);
		}
		else if (key == "memory_enabled_default")
		{
			settings.memory_enabled_default = ParseBool(value, settings.memory_enabled_default);
		}
		else if (key == "memory_idle_delay_seconds")
		{
			settings.memory_idle_delay_seconds = ParseInt(value, settings.memory_idle_delay_seconds);
		}
		else if (key == "memory_recall_budget_bytes")
		{
			settings.memory_recall_budget_bytes = ParseInt(value, settings.memory_recall_budget_bytes);
		}
		else if (key == "memory_worker_bindings")
		{
			DecodeMemoryWorkerBindings(decoded_value, settings.memory_worker_bindings);
		}
	}

	if (!has_provider_command_template && !settings.gemini_command_template.empty())
	{
		settings.provider_command_template = settings.gemini_command_template;
	}
	settings.provider_yolo_mode = settings.provider_yolo_mode || settings.gemini_yolo_mode;
	if (settings.provider_extra_flags.empty())
	{
		settings.provider_extra_flags = settings.gemini_extra_flags;
	}

	center_view_mode = CenterViewMode::CliConsole;
	ClampSettings(settings);
}
