#include "common/config/settings_store.h"

#include "common/config/line_value_codec.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/io_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	constexpr int kEditorDefaultGroupsVersion = 1;

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

	std::string NormalizeApprovalMode(std::string value)
	{
		value = uam::DecodeLineValue(uam::EncodeLineValue(value));
		if (value == "plan" || value == "acceptEdits")
		{
			return value;
		}
		return "default";
	}

	std::string NormalizeReasoningEffort(std::string value)
	{
		value = ToLower(value);
		if (value == "none" ||
		    value == "minimal" ||
		    value == "low" ||
		    value == "medium" ||
		    value == "high" ||
		    value == "xhigh")
		{
			return value;
		}
		return "";
	}

	std::string NormalizeServiceTier(std::string value)
	{
		value = ToLower(value);
		if (value == "fast" || value == "flex")
		{
			return value;
		}
		return "";
	}

	ProviderChatDefaults NormalizeProviderChatDefaults(ProviderChatDefaults defaults)
	{
		defaults.approval_mode = NormalizeApprovalMode(defaults.approval_mode);
		defaults.reasoning_effort = NormalizeReasoningEffort(defaults.reasoning_effort);
		defaults.service_tier = NormalizeServiceTier(defaults.service_tier);
		return defaults;
	}

	std::string EncodeProviderChatDefaults(const std::map<std::string, ProviderChatDefaults>& defaults_by_provider)
	{
		std::ostringstream out;
		bool first = true;
		for (const auto& entry : defaults_by_provider)
		{
			const std::string provider_id = NormalizeProviderId(entry.first);
			if (provider_id.empty())
			{
				continue;
			}
			const ProviderChatDefaults defaults = NormalizeProviderChatDefaults(entry.second);
			if (!first)
			{
				out << ';';
			}
			out << uam::EncodeLineValue(provider_id) << ','
			    << uam::EncodeLineValue(defaults.model_id) << ','
			    << uam::EncodeLineValue(defaults.approval_mode) << ','
			    << (defaults.auto_approve_commands ? "1" : "0") << ','
			    << (defaults.memory_enabled ? "1" : "0") << ','
			    << uam::EncodeLineValue(defaults.reasoning_effort) << ','
			    << uam::EncodeLineValue(defaults.service_tier);
			first = false;
		}
		return out.str();
	}

	void DecodeProviderChatDefaults(const std::string& value, std::map<std::string, ProviderChatDefaults>& defaults_by_provider)
	{
		defaults_by_provider.clear();
		for (const std::string& encoded_entry : Split(value, ';'))
		{
			const std::vector<std::string> fields = Split(encoded_entry, ',');
			if (fields.empty())
			{
				continue;
			}

			const std::string provider_id = NormalizeProviderId(uam::DecodeLineValue(fields[0]));
			if (provider_id.empty())
			{
				continue;
			}

			ProviderChatDefaults defaults;
			defaults.model_id = fields.size() >= 2 ? uam::DecodeLineValue(fields[1]) : "";
			defaults.approval_mode = fields.size() >= 3 ? uam::DecodeLineValue(fields[2]) : "default";
			defaults.auto_approve_commands = fields.size() >= 4 ? ParseBool(fields[3], false) : false;
			defaults.memory_enabled = fields.size() >= 5 ? ParseBool(fields[4], true) : true;
			defaults.reasoning_effort = fields.size() >= 6 ? uam::DecodeLineValue(fields[5]) : "";
			defaults.service_tier = fields.size() >= 7 ? uam::DecodeLineValue(fields[6]) : "";
			defaults_by_provider[provider_id] = NormalizeProviderChatDefaults(defaults);
		}
	}

	std::string EncodeEditorFileAssociations(const std::vector<EditorFileAssociation>& associations)
	{
		nlohmann::json value = nlohmann::json::array();
		for (const EditorFileAssociation& association : associations)
		{
			if (association.id.empty() || association.name.empty() || association.editor_preset_id.empty() || association.extensions.empty())
			{
				continue;
			}
			value.push_back({
			    {"id", association.id},
			    {"name", association.name},
			    {"extensions", association.extensions},
			    {"editorPresetId", association.editor_preset_id},
			});
		}
		return value.dump();
	}

	void DecodeEditorFileAssociations(const std::string& value, std::vector<EditorFileAssociation>& associations)
	{
		associations.clear();
		try
		{
			const nlohmann::json parsed = nlohmann::json::parse(value);
			if (!parsed.is_array())
			{
				return;
			}

			for (const nlohmann::json& item : parsed)
			{
				if (!item.is_object())
				{
					continue;
				}

				EditorFileAssociation association;
				association.id = item.value("id", "");
				association.name = item.value("name", "");
				association.editor_preset_id = item.value("editorPresetId", "");
				const nlohmann::json extensions = item.value("extensions", nlohmann::json::array());
				if (extensions.is_array())
				{
					for (const nlohmann::json& extension : extensions)
					{
						if (extension.is_string())
						{
							const std::string extension_value = extension.get<std::string>();
							if (!extension_value.empty())
							{
								association.extensions.push_back(extension_value);
							}
						}
					}
				}
				if (association.id.empty() || association.name.empty() || association.editor_preset_id.empty() || association.extensions.empty())
				{
					continue;
				}
				associations.push_back(std::move(association));
			}
		}
		catch (...)
		{
			associations.clear();
		}
	}

	std::vector<EditorFileAssociation> DefaultEditorFileAssociations()
	{
		return {
		    EditorFileAssociation{"cpp", "C++", {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}, "clion"},
		    EditorFileAssociation{"csharp", "C#", {".cs", ".csx", ".csproj", ".sln"}, "rider"},
		    EditorFileAssociation{"python", "Python", {".py", ".pyw", ".ipynb"}, "pycharm"},
		    EditorFileAssociation{"javascript", "JavaScript", {".js", ".mjs", ".cjs"}, "webstorm"},
		    EditorFileAssociation{"react-typescript", "React / TypeScript", {".jsx", ".ts", ".tsx", ".mts", ".cts"}, "webstorm"},
		    EditorFileAssociation{"rust", "Rust", {".rs"}, "rustrover"},
		    EditorFileAssociation{"go", "Go", {".go"}, "goland"},
		    EditorFileAssociation{"java-kotlin", "Java / Kotlin", {".java", ".kt", ".kts"}, "idea"},
		    EditorFileAssociation{"swift-apple", "Swift / Apple", {".swift"}, "xcode"},
		    EditorFileAssociation{"powershell", "PowerShell", {".ps1", ".psm1", ".psd1"}, "vscode"},
		    EditorFileAssociation{"shell", "Bash / Shell", {".sh", ".bash", ".zsh", ".fish"}, "vscode"},
		    EditorFileAssociation{"web-styles", "Web Styles / Templates", {".html", ".css", ".scss", ".sass", ".less"}, "webstorm"},
		};
	}

	bool IsKnownEditorPresetId(const std::string& value)
	{
		return value == "vscode" ||
		       value == "xcode" ||
		       value == "visualstudio" ||
		       value == "clion" ||
		       value == "rider" ||
		       value == "webstorm" ||
		       value == "pycharm" ||
		       value == "idea" ||
		       value == "goland" ||
		       value == "rustrover";
	}

	void NormalizeEditorFileAssociations(std::vector<EditorFileAssociation>& associations)
	{
		std::vector<EditorFileAssociation> normalized;
		for (EditorFileAssociation& association : associations)
		{
			if (association.id.empty() || association.name.empty() || association.extensions.empty())
			{
				continue;
			}
			if (!IsKnownEditorPresetId(association.editor_preset_id))
			{
				association.editor_preset_id = "vscode";
			}
			normalized.push_back(std::move(association));
		}
		associations = std::move(normalized);
	}

	void AppendMissingDefaultEditorGroups(AppSettings& settings)
	{
		if (settings.editor_default_groups_version >= kEditorDefaultGroupsVersion)
		{
			return;
		}

		std::set<std::string> existing_ids;
		for (const EditorFileAssociation& association : settings.editor_file_associations)
		{
			if (!association.id.empty())
			{
				existing_ids.insert(association.id);
			}
		}

		for (const EditorFileAssociation& default_association : DefaultEditorFileAssociations())
		{
			if (existing_ids.insert(default_association.id).second)
			{
				settings.editor_file_associations.push_back(default_association);
			}
		}

		settings.editor_default_groups_version = kEditorDefaultGroupsVersion;
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
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		if (lowered == "opencode" || lowered == "opencode-cli")
		{
			return "opencode-cli";
		}
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
		if (lowered == "copilot" || lowered == "github-copilot" || lowered == "copilot-cli")
		{
			return "copilot-cli";
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
		settings.default_new_chat_provider_id = NormalizeProviderId(settings.default_new_chat_provider_id.empty() ? settings.active_provider_id : settings.default_new_chat_provider_id);
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
		if (settings.default_editor_preset_id.empty())
		{
			settings.default_editor_preset_id = "vscode";
		}
		if (!IsKnownEditorPresetId(settings.default_editor_preset_id))
		{
			settings.default_editor_preset_id = "vscode";
		}
		NormalizeEditorFileAssociations(settings.editor_file_associations);
		AppendMissingDefaultEditorGroups(settings);

		for (const std::string& provider_id : {std::string("gemini-cli"), std::string("codex-cli"), std::string("claude-cli"), std::string("opencode-cli"), std::string("copilot-cli")})
		{
			if (settings.memory_worker_bindings.find(provider_id) == settings.memory_worker_bindings.end())
			{
				settings.memory_worker_bindings[provider_id] = MemoryWorkerBinding{NormalizeProviderId(provider_id), ""};
			}
			if (settings.provider_chat_defaults.find(provider_id) == settings.provider_chat_defaults.end())
			{
				settings.provider_chat_defaults[provider_id] = ProviderChatDefaults{"", "default", false, settings.memory_enabled_default, "", ""};
			}
			else
			{
				settings.provider_chat_defaults[provider_id] = NormalizeProviderChatDefaults(settings.provider_chat_defaults[provider_id]);
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
	lines << "default_new_chat_provider_id=" << uam::EncodeLineValue(normalized.default_new_chat_provider_id) << '\n';
	lines << "provider_chat_defaults=" << EncodeProviderChatDefaults(normalized.provider_chat_defaults) << '\n';
	lines << "markdown_store_directory=" << uam::EncodeLineValue(normalized.markdown_store_directory) << '\n';
	lines << "default_editor_preset_id=" << uam::EncodeLineValue(normalized.default_editor_preset_id) << '\n';
	lines << "editor_default_groups_version=" << normalized.editor_default_groups_version << '\n';
	lines << "editor_file_associations=" << EncodeEditorFileAssociations(normalized.editor_file_associations) << '\n';
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
		else if (key == "default_new_chat_provider_id")
		{
			settings.default_new_chat_provider_id = decoded_value;
		}
		else if (key == "provider_chat_defaults")
		{
			DecodeProviderChatDefaults(decoded_value, settings.provider_chat_defaults);
		}
		else if (key == "markdown_store_directory")
		{
			settings.markdown_store_directory = decoded_value;
		}
		else if (key == "default_editor_preset_id")
		{
			settings.default_editor_preset_id = decoded_value;
		}
		else if (key == "editor_default_groups_version")
		{
			settings.editor_default_groups_version = ParseInt(value, settings.editor_default_groups_version);
		}
		else if (key == "editor_file_associations")
		{
			DecodeEditorFileAssociations(decoded_value, settings.editor_file_associations);
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
