#include "common/config/settings_store.h"

#include "common/config/editor_file_associations.h"
#include "common/config/approval_modes.h"
#include "common/config/line_value_codec.h"
#include "common/config/settings_normalization.h"
#include "common/paths/path_utils.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	constexpr char kSettingsEntryDelimiter = ';';
	constexpr char kSettingsFieldDelimiter = ',';
	constexpr std::string_view kSettingsEntryDelimiterText = ";";
	constexpr std::string_view kSettingsFieldDelimiterText = ",";
	constexpr std::string_view kProviderCliRuntimeBackend = "provider-cli";

constexpr std::string_view kActiveProviderIdKey = "active_provider_id";
	constexpr std::string_view kProviderYoloModeKey = "provider_yolo_mode";
	constexpr std::string_view kProviderExtraFlagsKey = "provider_extra_flags";
	constexpr std::string_view kRuntimeBackendKey = "runtime_backend";
	constexpr std::string_view kCliIdleTimeoutSecondsKey = "cli_idle_timeout_seconds";
	constexpr std::string_view kCenterViewModeKey = "center_view_mode";
	constexpr std::string_view kUiThemeKey = "ui_theme";
	constexpr std::string_view kConfirmDeleteChatKey = "confirm_delete_chat";
	constexpr std::string_view kConfirmDeleteFolderKey = "confirm_delete_folder";
	constexpr std::string_view kRememberLastChatKey = "remember_last_chat";
	constexpr std::string_view kLastSelectedChatIdKey = "last_selected_chat_id";
	constexpr std::string_view kUiScaleMultiplierKey = "ui_scale_multiplier";
	constexpr std::string_view kSidebarWidthKey = "sidebar_width";
	constexpr std::string_view kWindowWidthKey = "window_width";
	constexpr std::string_view kWindowHeightKey = "window_height";
	constexpr std::string_view kWindowMaximizedKey = "window_maximized";
	constexpr std::string_view kMemoryEnabledDefaultKey = "memory_enabled_default";
	constexpr std::string_view kMemoryIdleDelaySecondsKey = "memory_idle_delay_seconds";
	constexpr std::string_view kMemoryRecallBudgetBytesKey = "memory_recall_budget_bytes";
	constexpr std::string_view kMemoryWorkerBindingsKey = "memory_worker_bindings";
	constexpr std::string_view kDefaultNewChatProviderIdKey = "default_new_chat_provider_id";
	constexpr std::string_view kProviderChatDefaultsKey = "provider_chat_defaults";
	constexpr std::string_view kMarkdownStoreDirectoryKey = "markdown_store_directory";
	constexpr std::string_view kDefaultEditorPresetIdKey = "default_editor_preset_id";
	constexpr std::string_view kEditorDefaultGroupsVersionKey = "editor_default_groups_version";
	constexpr std::string_view kEditorFileAssociationsKey = "editor_file_associations";

	constexpr std::string_view kLegacyGeminiYoloModeKey = "gemini_yolo_mode";
	constexpr std::string_view kLegacyGeminiExtraFlagsKey = "gemini_extra_flags";

	std::string NormalizeProviderId(std::string_view value);

	void WriteRawSetting(std::ostringstream& lines, std::string_view key, std::string_view value)
	{
		lines << key << '=' << value << '\n';
	}

	template <typename Value> void WriteSettingValue(std::ostringstream& lines, std::string_view key, const Value& value)
	{
		lines << key << '=' << value << '\n';
	}

	void WriteEncodedSetting(std::ostringstream& lines, std::string_view key, std::string_view value)
	{
		WriteRawSetting(lines, key, uam::EncodeLineValue(value));
	}

	void WriteBoolSetting(std::ostringstream& lines, std::string_view key, bool value)
	{
		WriteRawSetting(lines, key, value ? "1" : "0");
	}

	bool TryNormalizeMemoryWorkerBinding(std::string_view chat_provider_id,
	                                     const MemoryWorkerBinding& binding,
	                                     std::string& normalized_chat_provider_id,
	                                     MemoryWorkerBinding& normalized_binding)
	{
		normalized_chat_provider_id = NormalizeProviderId(chat_provider_id);
		normalized_binding.worker_provider_id = NormalizeProviderId(binding.worker_provider_id);
		normalized_binding.worker_model_id = uam::strings::Trim(binding.worker_model_id);
		return !normalized_chat_provider_id.empty() && !normalized_binding.worker_provider_id.empty();
	}

	bool BoolFieldOr(const std::vector<std::string_view>& fields, std::size_t index, bool fallback)
	{
		return index < fields.size() ? uam::parse::BoolOr(fields[index], fallback) : fallback;
	}

	std::string EncodeMemoryWorkerBindings(const std::map<std::string, MemoryWorkerBinding>& bindings)
	{
		std::vector<std::string> encoded_entries;
		encoded_entries.reserve(bindings.size());
		for (const auto& entry : bindings)
		{
			std::string chat_provider_id;
			MemoryWorkerBinding binding;
			if (!TryNormalizeMemoryWorkerBinding(entry.first, entry.second, chat_provider_id, binding))
			{
				continue;
			}
			encoded_entries.push_back(uam::EncodeLineValueFields({
			    chat_provider_id,
			    binding.worker_provider_id,
			    binding.worker_model_id,
			}, kSettingsFieldDelimiterText));
		}
		return uam::strings::JoinNonEmpty(encoded_entries, kSettingsEntryDelimiterText);
	}

	void DecodeMemoryWorkerBindings(std::string_view value, std::map<std::string, MemoryWorkerBinding>& bindings)
	{
		bindings.clear();
		for (const std::string_view encoded_entry : uam::SplitLineValueFields(value, kSettingsEntryDelimiter))
		{
			const std::vector<std::string_view> fields = uam::SplitLineValueFields(encoded_entry, kSettingsFieldDelimiter);
			if (fields.size() < 2)
			{
				continue;
			}

			std::string chat_provider_id;
			MemoryWorkerBinding binding;
			binding.worker_provider_id = uam::DecodedLineFieldOr(fields, 1, "");
			binding.worker_model_id = uam::DecodedLineFieldOr(fields, 2, "");
			if (!TryNormalizeMemoryWorkerBinding(uam::DecodedLineFieldOr(fields, 0, ""), binding, chat_provider_id, binding))
			{
				continue;
			}

			bindings[chat_provider_id] = std::move(binding);
		}
	}

	void NormalizeMemoryWorkerBindings(std::map<std::string, MemoryWorkerBinding>& bindings)
	{
		std::map<std::string, MemoryWorkerBinding> normalized;
		for (const auto& entry : bindings)
		{
			std::string chat_provider_id;
			MemoryWorkerBinding binding;
			if (!TryNormalizeMemoryWorkerBinding(entry.first, entry.second, chat_provider_id, binding))
			{
				continue;
			}
			normalized[chat_provider_id] = std::move(binding);
		}
		bindings = std::move(normalized);
	}

	ProviderChatDefaults NormalizeProviderChatDefaults(ProviderChatDefaults defaults)
	{
		defaults.model_id = uam::strings::Trim(defaults.model_id);
		defaults.approval_mode = uam::approval_modes::NormalizePersistedProviderDefaultApprovalMode(defaults.approval_mode);
		defaults.reasoning_effort = uam::codex::NormalizeReasoningEffort(defaults.reasoning_effort);
		defaults.service_tier = uam::codex::NormalizeServiceTier(defaults.service_tier);
		return defaults;
	}

	bool TryNormalizeProviderChatDefaults(std::string_view provider_id,
	                                      const ProviderChatDefaults& defaults,
	                                      std::string& normalized_provider_id,
	                                      ProviderChatDefaults& normalized_defaults)
	{
		normalized_provider_id = NormalizeProviderId(provider_id);
		if (normalized_provider_id.empty())
		{
			return false;
		}

		normalized_defaults = NormalizeProviderChatDefaults(defaults);
		return true;
	}

	void NormalizeProviderChatDefaultsByProvider(std::map<std::string, ProviderChatDefaults>& defaults_by_provider)
	{
		std::map<std::string, ProviderChatDefaults> normalized;
		for (const auto& entry : defaults_by_provider)
		{
			std::string provider_id;
			ProviderChatDefaults defaults;
			if (!TryNormalizeProviderChatDefaults(entry.first, entry.second, provider_id, defaults))
			{
				continue;
			}
			normalized[provider_id] = std::move(defaults);
		}
		defaults_by_provider = std::move(normalized);
	}

	std::string EncodeProviderChatDefaults(const std::map<std::string, ProviderChatDefaults>& defaults_by_provider)
	{
		std::vector<std::string> encoded_entries;
		encoded_entries.reserve(defaults_by_provider.size());
		for (const auto& entry : defaults_by_provider)
		{
			std::string provider_id;
			ProviderChatDefaults defaults;
			if (!TryNormalizeProviderChatDefaults(entry.first, entry.second, provider_id, defaults))
			{
				continue;
			}
			encoded_entries.push_back(uam::EncodeLineValueFields({
			    provider_id,
			    defaults.model_id,
			    defaults.approval_mode,
			    defaults.auto_approve_commands ? "1" : "0",
			    defaults.memory_enabled ? "1" : "0",
			    defaults.reasoning_effort,
			    defaults.service_tier,
			}, kSettingsFieldDelimiterText));
		}
		return uam::strings::JoinNonEmpty(encoded_entries, kSettingsEntryDelimiterText);
	}

	void DecodeProviderChatDefaults(std::string_view value, std::map<std::string, ProviderChatDefaults>& defaults_by_provider)
	{
		defaults_by_provider.clear();
		for (const std::string_view encoded_entry : uam::SplitLineValueFields(value, kSettingsEntryDelimiter))
		{
			const std::vector<std::string_view> fields = uam::SplitLineValueFields(encoded_entry, kSettingsFieldDelimiter);
			if (fields.empty())
			{
				continue;
			}

			ProviderChatDefaults defaults;
			defaults.model_id = uam::DecodedLineFieldOr(fields, 1, "");
			defaults.approval_mode = uam::DecodedLineFieldOr(fields, 2, uam::approval_modes::kDefaultApprovalMode);
			defaults.auto_approve_commands = BoolFieldOr(fields, 3, false);
			defaults.memory_enabled = BoolFieldOr(fields, 4, true);
			defaults.reasoning_effort = uam::DecodedLineFieldOr(fields, 5, "");
			defaults.service_tier = uam::DecodedLineFieldOr(fields, 6, "");

			std::string provider_id;
			if (!TryNormalizeProviderChatDefaults(uam::DecodedLineFieldOr(fields, 0, ""), defaults, provider_id, defaults))
			{
				continue;
			}

			defaults_by_provider[provider_id] = std::move(defaults);
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

			associations.reserve(parsed.size());
			for (const nlohmann::json& item : parsed)
			{
				if (!item.is_object())
				{
					continue;
				}

				EditorFileAssociation association;
				association.id = uam::nlohmann_json::TrimmedStringValue(item, {"id"});
				association.name = uam::nlohmann_json::TrimmedStringValue(item, {"name"});
				association.editor_preset_id = uam::nlohmann_json::TrimmedStringValue(item, {"editorPresetId"});
				association.extensions = uam::nlohmann_json::StringArrayField(item, "extensions");
				std::optional<EditorFileAssociation> normalized_association = uam::editor_file_associations::NormalizeEditorFileAssociation(std::move(association));
				if (!normalized_association)
				{
					continue;
				}
				associations.push_back(std::move(*normalized_association));
			}
		}
		catch (const nlohmann::json::exception&)
		{
			associations.clear();
		}
	}

	std::string NormalizeProviderId(std::string_view value)
	{
		return provider_build_config::EnabledCliProviderIdOrFirst(value);
	}

	void EnsureMemoryWorkerBinding(AppSettings& settings, std::string_view provider_id)
	{
		const std::string provider_key(provider_id);
		if (!settings.memory_worker_bindings.contains(provider_key))
		{
			settings.memory_worker_bindings[provider_key] = MemoryWorkerBinding{NormalizeProviderId(provider_key), ""};
		}
	}

	void EnsureProviderChatDefaults(AppSettings& settings, std::string_view provider_id)
	{
		const std::string provider_key(provider_id);
		if (!settings.provider_chat_defaults.contains(provider_key))
		{
			settings.provider_chat_defaults[provider_key] = ProviderChatDefaults{"", uam::approval_modes::kDefaultApprovalMode, false, settings.memory_enabled_default, "", ""};
			return;
		}

		settings.provider_chat_defaults[provider_key] = NormalizeProviderChatDefaults(settings.provider_chat_defaults[provider_key]);
	}

	void ClampSettings(AppSettings& settings)
	{
		settings.active_provider_id = provider_build_config::EnabledCliProviderIdOrFirst(settings.active_provider_id);
		settings.default_new_chat_provider_id = provider_build_config::EnabledCliProviderIdOrFirst(uam::strings::NonEmptyOrFallback(settings.default_new_chat_provider_id, settings.active_provider_id));
		settings.runtime_backend = kProviderCliRuntimeBackend;
		settings.gemini_yolo_mode = settings.provider_yolo_mode;
		settings.gemini_extra_flags = settings.provider_extra_flags;
		settings.cli_idle_timeout_seconds = std::clamp(settings.cli_idle_timeout_seconds, uam::settings::kMinCliIdleTimeoutSeconds, uam::settings::kMaxCliIdleTimeoutSeconds);
		settings.ui_theme = uam::settings::NormalizeThemeId(settings.ui_theme);
		uam::settings::ClampWindowSettings(settings);
		uam::settings::ClampMemorySettings(settings);
		settings.default_editor_preset_id = uam::editor_file_associations::NormalizeEditorPresetId(settings.default_editor_preset_id);
		uam::editor_file_associations::NormalizeEditorFileAssociations(settings.editor_file_associations);
		uam::editor_file_associations::AppendMissingDefaultEditorGroups(settings);
		NormalizeMemoryWorkerBindings(settings.memory_worker_bindings);
		NormalizeProviderChatDefaultsByProvider(settings.provider_chat_defaults);

		for (const char* provider_id : uam::provider_ids::kAllCliProviderIds)
		{
			EnsureMemoryWorkerBinding(settings, provider_id);
			EnsureProviderChatDefaults(settings, provider_id);
		}

		if (!settings.remember_last_chat)
		{
			settings.last_selected_chat_id.clear();
		}
		else
		{
			settings.last_selected_chat_id = uam::strings::Trim(settings.last_selected_chat_id);
		}
	}

} // namespace

bool SettingsStore::Save(const std::filesystem::path& settings_file, const AppSettings& settings, CenterViewMode center_view_mode)
{
	uam::paths::CreateDirectoriesNoThrow(settings_file.parent_path());

	AppSettings normalized = settings;
	ClampSettings(normalized);

	std::ostringstream lines;
	WriteEncodedSetting(lines, kActiveProviderIdKey, normalized.active_provider_id);
	WriteBoolSetting(lines, kProviderYoloModeKey, normalized.provider_yolo_mode);
	WriteEncodedSetting(lines, kProviderExtraFlagsKey, normalized.provider_extra_flags);
	WriteRawSetting(lines, kRuntimeBackendKey, kProviderCliRuntimeBackend);
	WriteSettingValue(lines, kCliIdleTimeoutSecondsKey, normalized.cli_idle_timeout_seconds);
	WriteRawSetting(lines, kCenterViewModeKey, ViewModeToString(center_view_mode));
	WriteEncodedSetting(lines, kUiThemeKey, normalized.ui_theme);
	WriteBoolSetting(lines, kConfirmDeleteChatKey, normalized.confirm_delete_chat);
	WriteBoolSetting(lines, kConfirmDeleteFolderKey, normalized.confirm_delete_folder);
	WriteBoolSetting(lines, kRememberLastChatKey, normalized.remember_last_chat);
	WriteEncodedSetting(lines, kLastSelectedChatIdKey, normalized.last_selected_chat_id);
	WriteSettingValue(lines, kUiScaleMultiplierKey, normalized.ui_scale_multiplier);
	WriteSettingValue(lines, kSidebarWidthKey, normalized.sidebar_width);
	WriteSettingValue(lines, kWindowWidthKey, normalized.window_width);
	WriteSettingValue(lines, kWindowHeightKey, normalized.window_height);
	WriteBoolSetting(lines, kWindowMaximizedKey, normalized.window_maximized);
	WriteBoolSetting(lines, kMemoryEnabledDefaultKey, normalized.memory_enabled_default);
	WriteSettingValue(lines, kMemoryIdleDelaySecondsKey, normalized.memory_idle_delay_seconds);
	WriteSettingValue(lines, kMemoryRecallBudgetBytesKey, normalized.memory_recall_budget_bytes);
	WriteRawSetting(lines, kMemoryWorkerBindingsKey, EncodeMemoryWorkerBindings(normalized.memory_worker_bindings));
	WriteEncodedSetting(lines, kDefaultNewChatProviderIdKey, normalized.default_new_chat_provider_id);
	WriteRawSetting(lines, kProviderChatDefaultsKey, EncodeProviderChatDefaults(normalized.provider_chat_defaults));
	WriteEncodedSetting(lines, kMarkdownStoreDirectoryKey, normalized.markdown_store_directory);
	WriteEncodedSetting(lines, kDefaultEditorPresetIdKey, normalized.default_editor_preset_id);
	WriteSettingValue(lines, kEditorDefaultGroupsVersionKey, normalized.editor_default_groups_version);
	WriteRawSetting(lines, kEditorFileAssociationsKey, EncodeEditorFileAssociations(normalized.editor_file_associations));
	return uam::io::WriteTextFile(settings_file, lines.str());
}

void SettingsStore::Load(const std::filesystem::path& settings_file, AppSettings& settings, CenterViewMode& center_view_mode)
{
	std::string text;
	if (!uam::io::TryReadTextFile(settings_file, text))
	{
		ClampSettings(settings);
		center_view_mode = CenterViewMode::CliConsole;
		return;
	}

	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line))
	{
		const auto equals_at = line.find('=');
		if (equals_at == std::string::npos)
		{
			continue;
		}

		const std::string_view key(line.data(), equals_at);
		const std::string_view value(line.data() + equals_at + 1, line.size() - equals_at - 1);
		const std::string decoded_value = uam::DecodeLineValue(value);

		if (key == kActiveProviderIdKey)
		{
			settings.active_provider_id = decoded_value;
		}
		else if (key == kProviderYoloModeKey)
		{
			settings.provider_yolo_mode = uam::parse::BoolOr(value, settings.provider_yolo_mode);
		}
		else if (key == kProviderExtraFlagsKey)
		{
			settings.provider_extra_flags = decoded_value;
		}
		else if (key == kLegacyGeminiYoloModeKey)
		{
			settings.gemini_yolo_mode = uam::parse::BoolOr(value, settings.gemini_yolo_mode);
		}
		else if (key == kLegacyGeminiExtraFlagsKey)
		{
			settings.gemini_extra_flags = decoded_value;
		}
		else if (key == kCliIdleTimeoutSecondsKey)
		{
			settings.cli_idle_timeout_seconds = uam::parse::IntOr(value, settings.cli_idle_timeout_seconds);
		}
		else if (key == kCenterViewModeKey)
		{
			center_view_mode = ViewModeFromString(value);
		}
		else if (key == kUiThemeKey)
		{
			settings.ui_theme = uam::settings::NormalizeThemeId(decoded_value);
		}
		else if (key == kConfirmDeleteChatKey)
		{
			settings.confirm_delete_chat = uam::parse::BoolOr(value, settings.confirm_delete_chat);
		}
		else if (key == kConfirmDeleteFolderKey)
		{
			settings.confirm_delete_folder = uam::parse::BoolOr(value, settings.confirm_delete_folder);
		}
		else if (key == kRememberLastChatKey)
		{
			settings.remember_last_chat = uam::parse::BoolOr(value, settings.remember_last_chat);
		}
		else if (key == kLastSelectedChatIdKey)
		{
			settings.last_selected_chat_id = decoded_value;
		}
		else if (key == kUiScaleMultiplierKey)
		{
			settings.ui_scale_multiplier = uam::parse::FloatOr(value, settings.ui_scale_multiplier);
		}
		else if (key == kSidebarWidthKey)
		{
			settings.sidebar_width = uam::parse::FloatOr(value, settings.sidebar_width);
		}
		else if (key == kWindowWidthKey)
		{
			settings.window_width = uam::parse::IntOr(value, settings.window_width);
		}
		else if (key == kWindowHeightKey)
		{
			settings.window_height = uam::parse::IntOr(value, settings.window_height);
		}
		else if (key == kWindowMaximizedKey)
		{
			settings.window_maximized = uam::parse::BoolOr(value, settings.window_maximized);
		}
		else if (key == kMemoryEnabledDefaultKey)
		{
			settings.memory_enabled_default = uam::parse::BoolOr(value, settings.memory_enabled_default);
		}
		else if (key == kMemoryIdleDelaySecondsKey)
		{
			settings.memory_idle_delay_seconds = uam::parse::IntOr(value, settings.memory_idle_delay_seconds);
		}
		else if (key == kMemoryRecallBudgetBytesKey)
		{
			settings.memory_recall_budget_bytes = uam::parse::IntOr(value, settings.memory_recall_budget_bytes);
		}
		else if (key == kMemoryWorkerBindingsKey)
		{
			DecodeMemoryWorkerBindings(decoded_value, settings.memory_worker_bindings);
		}
		else if (key == kDefaultNewChatProviderIdKey)
		{
			settings.default_new_chat_provider_id = decoded_value;
		}
		else if (key == kProviderChatDefaultsKey)
		{
			DecodeProviderChatDefaults(decoded_value, settings.provider_chat_defaults);
		}
		else if (key == kMarkdownStoreDirectoryKey)
		{
			settings.markdown_store_directory = decoded_value;
		}
		else if (key == kDefaultEditorPresetIdKey)
		{
			settings.default_editor_preset_id = decoded_value;
		}
		else if (key == kEditorDefaultGroupsVersionKey)
		{
			settings.editor_default_groups_version = uam::parse::IntOr(value, settings.editor_default_groups_version);
		}
		else if (key == kEditorFileAssociationsKey)
		{
			DecodeEditorFileAssociations(decoded_value, settings.editor_file_associations);
		}
	}

	settings.provider_yolo_mode = settings.provider_yolo_mode || settings.gemini_yolo_mode;
	if (settings.provider_extra_flags.empty())
	{
		settings.provider_extra_flags = settings.gemini_extra_flags;
	}

	center_view_mode = CenterViewMode::CliConsole;
	ClampSettings(settings);
}
