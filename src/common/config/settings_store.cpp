#include "common/config/settings_store.h"

#include "common/config/line_value_codec.h"
#include "common/paths/app_paths.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace
{

	bool WriteTextFile(const std::filesystem::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);

		if (!out.good())
		{
			return false;
		}

		out << content;
		return out.good();
	}

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

	std::string NormalizeRuntimeBackendId(std::string value)
	{
		value = ToLower(value);

		if (value == "ollama-engine")
		{
			return "ollama-engine";
		}

		return "provider-cli";
	}

	std::string NormalizeVectorDbBackendId(std::string value)
	{
		value = ToLower(value);

		if (value == "none")
		{
			return "none";
		}

		return "ollama-engine";
	}

} // namespace

bool SettingsStore::Save(const std::filesystem::path& settings_file, const AppSettings& settings, const CenterViewMode center_view_mode)
{
	std::error_code ec;
	std::filesystem::create_directories(settings_file.parent_path(), ec);
	std::ostringstream lines;
	const std::string runtime_backend_compat = (ToLower(settings.active_provider_id) == "ollama-engine") ? "ollama-engine" : "provider-cli";
	lines << "active_provider_id=" << uam::EncodeLineValue(settings.active_provider_id) << '\n';
	lines << "provider_command_template=" << uam::EncodeLineValue(settings.provider_command_template) << '\n';
	lines << "provider_yolo_mode=" << (settings.provider_yolo_mode ? "1" : "0") << '\n';
	lines << "provider_extra_flags=" << uam::EncodeLineValue(settings.provider_extra_flags) << '\n';
	lines << "runtime_backend=" << NormalizeRuntimeBackendId(runtime_backend_compat) << '\n';
	lines << "selected_model_id=" << uam::EncodeLineValue(settings.selected_model_id) << '\n';
	lines << "models_folder_directory=" << uam::EncodeLineValue(settings.models_folder_directory) << '\n';
	lines << "vector_db_backend=" << NormalizeVectorDbBackendId(settings.vector_db_backend) << '\n';
	lines << "selected_vector_model_id=" << uam::EncodeLineValue(settings.selected_vector_model_id) << '\n';
	lines << "vector_database_name_override=" << uam::EncodeLineValue(settings.vector_database_name_override) << '\n';
	lines << "cli_idle_timeout_seconds=" << settings.cli_idle_timeout_seconds << '\n';
	lines << "prompt_profile_root_path=" << uam::EncodeLineValue(settings.prompt_profile_root_path) << '\n';
	lines << "default_prompt_profile_id=" << uam::EncodeLineValue(settings.default_prompt_profile_id) << '\n';
	// Backward-compatible legacy keys.
	lines << "gemini_command_template=" << uam::EncodeLineValue(settings.provider_command_template) << '\n';
	lines << "gemini_yolo_mode=" << (settings.provider_yolo_mode ? "1" : "0") << '\n';
	lines << "gemini_extra_flags=" << uam::EncodeLineValue(settings.provider_extra_flags) << '\n';
	lines << "gemini_global_root_path=" << uam::EncodeLineValue(settings.prompt_profile_root_path) << '\n';
	lines << "default_gemini_template_id=" << uam::EncodeLineValue(settings.default_prompt_profile_id) << '\n';
	lines << "rag_enabled=" << (settings.rag_enabled ? "1" : "0") << '\n';
	lines << "rag_top_k=" << settings.rag_top_k << '\n';
	lines << "rag_max_snippet_chars=" << settings.rag_max_snippet_chars << '\n';
	lines << "rag_max_file_bytes=" << settings.rag_max_file_bytes << '\n';
	lines << "rag_scan_max_tokens=" << settings.rag_scan_max_tokens << '\n';
	lines << "rag_project_source_directory=" << uam::EncodeLineValue(settings.rag_project_source_directory) << '\n';
	lines << "center_view_mode=" << ViewModeToString(center_view_mode) << '\n';
	lines << "ui_theme=" << uam::EncodeLineValue(NormalizeThemeId(settings.ui_theme)) << '\n';
	lines << "confirm_delete_chat=" << (settings.confirm_delete_chat ? "1" : "0") << '\n';
	lines << "confirm_delete_folder=" << (settings.confirm_delete_folder ? "1" : "0") << '\n';
	lines << "remember_last_chat=" << (settings.remember_last_chat ? "1" : "0") << '\n';
	lines << "last_selected_chat_id=" << uam::EncodeLineValue(settings.last_selected_chat_id) << '\n';
	lines << "ui_scale_multiplier=" << settings.ui_scale_multiplier << '\n';
	lines << "window_width=" << settings.window_width << '\n';
	lines << "window_height=" << settings.window_height << '\n';
	lines << "window_maximized=" << (settings.window_maximized ? "1" : "0") << '\n';
	return WriteTextFile(settings_file, lines.str());
}

void SettingsStore::Load(const std::filesystem::path& settings_file, AppSettings& settings, CenterViewMode& center_view_mode)
{
	if (!std::filesystem::exists(settings_file))
	{
		return;
	}

	std::istringstream lines(ReadTextFile(settings_file));
	std::string line;
	bool has_active_provider_id = false;
	bool has_provider_command_template = false;
	bool has_runtime_backend = false;

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
			has_active_provider_id = true;
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
		else if (key == "runtime_backend")
		{
			settings.runtime_backend = value;
			has_runtime_backend = true;
		}
		else if (key == "selected_model_id")
		{
			settings.selected_model_id = decoded_value;
		}
		else if (key == "models_folder_directory")
		{
			settings.models_folder_directory = decoded_value;
		}
		else if (key == "vector_db_backend")
		{
			settings.vector_db_backend = value;
		}
		else if (key == "selected_vector_model_id")
		{
			settings.selected_vector_model_id = decoded_value;
		}
		else if (key == "vector_database_name_override")
		{
			settings.vector_database_name_override = decoded_value;
		}
		else if (key == "cli_idle_timeout_seconds")
		{
			settings.cli_idle_timeout_seconds = ParseInt(value, settings.cli_idle_timeout_seconds);
		}
		else if (key == "gemini_global_root_path")
		{
			settings.gemini_global_root_path = decoded_value;
		}
		else if (key == "prompt_profile_root_path")
		{
			settings.prompt_profile_root_path = decoded_value;
		}
		else if (key == "default_gemini_template_id")
		{
			settings.default_gemini_template_id = decoded_value;
		}
		else if (key == "default_prompt_profile_id")
		{
			settings.default_prompt_profile_id = decoded_value;
		}
		else if (key == "rag_enabled")
		{
			settings.rag_enabled = ParseBool(value, settings.rag_enabled);
		}
		else if (key == "rag_top_k")
		{
			settings.rag_top_k = ParseInt(value, settings.rag_top_k);
		}
		else if (key == "rag_max_snippet_chars")
		{
			settings.rag_max_snippet_chars = ParseInt(value, settings.rag_max_snippet_chars);
		}
		else if (key == "rag_max_file_bytes")
		{
			settings.rag_max_file_bytes = ParseInt(value, settings.rag_max_file_bytes);
		}
		else if (key == "rag_scan_max_tokens")
		{
			settings.rag_scan_max_tokens = ParseInt(value, settings.rag_scan_max_tokens);
		}
		else if (key == "rag_project_source_directory")
		{
			settings.rag_project_source_directory = decoded_value;
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
	}

	if (!has_provider_command_template)
	{
		settings.provider_command_template = settings.gemini_command_template;
	}

	settings.provider_yolo_mode = settings.provider_yolo_mode || settings.gemini_yolo_mode;

	if (settings.provider_extra_flags.empty())
	{
		settings.provider_extra_flags = settings.gemini_extra_flags;
	}

	if (settings.prompt_profile_root_path.empty())
	{
		settings.prompt_profile_root_path = settings.gemini_global_root_path;
	}

	if (settings.default_prompt_profile_id.empty())
	{
		settings.default_prompt_profile_id = settings.default_gemini_template_id;
	}

	if (settings.provider_command_template.empty())
	{
		settings.provider_command_template = "gemini {resume} {flags} -p {prompt}";
	}

	if (settings.gemini_command_template.empty())
	{
		settings.gemini_command_template = settings.provider_command_template;
	}

	if (!has_active_provider_id && has_runtime_backend && NormalizeRuntimeBackendId(settings.runtime_backend) == "ollama-engine")
	{
		settings.active_provider_id = "ollama-engine";
	}

	if (settings.active_provider_id.empty())
	{
		settings.active_provider_id = "gemini-structured";
	}

	settings.runtime_backend = NormalizeRuntimeBackendId(settings.runtime_backend);
	settings.vector_db_backend = NormalizeVectorDbBackendId(settings.vector_db_backend);

	if (settings.prompt_profile_root_path.empty())
	{
		settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
	}

	settings.gemini_command_template = settings.provider_command_template;
	settings.gemini_yolo_mode = settings.provider_yolo_mode;
	settings.gemini_extra_flags = settings.provider_extra_flags;
	settings.gemini_global_root_path = settings.prompt_profile_root_path;
	settings.default_gemini_template_id = settings.default_prompt_profile_id;
	settings.cli_idle_timeout_seconds = std::clamp(settings.cli_idle_timeout_seconds, 30, 3600);
	settings.rag_top_k = std::clamp(settings.rag_top_k, 1, 20);
	settings.rag_max_snippet_chars = std::clamp(settings.rag_max_snippet_chars, 120, 4000);
	settings.rag_max_file_bytes = std::clamp(settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024);
	settings.rag_scan_max_tokens = std::clamp(settings.rag_scan_max_tokens, 0, 32768);
	settings.ui_theme = NormalizeThemeId(settings.ui_theme);
	settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
	settings.window_width = std::clamp(settings.window_width, 960, 8192);
	settings.window_height = std::clamp(settings.window_height, 620, 8192);

	if (!settings.remember_last_chat)
	{
		settings.last_selected_chat_id.clear();
	}
}
