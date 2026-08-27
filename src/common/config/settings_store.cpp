#include "common/config/settings_store.h"

#include "common/config/editor_file_associations.h"
#include "common/config/approval_modes.h"
#include "common/config/line_value_codec.h"
#include "common/config/mcp_server_config.h"
#include "common/config/provider_chat_defaults.h"
#include "common/config/settings_normalization.h"
#include "common/memory/memory_levels.h"
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
	constexpr std::string_view kSettingsFormatVersionKey = "settings_format_version";
	constexpr std::string_view kSettingsCompleteKey = "settings_complete";

constexpr std::string_view kActiveProviderIdKey = "active_provider_id";
	constexpr std::string_view kProviderYoloModeKey = "provider_yolo_mode";
	constexpr std::string_view kProviderExtraFlagsKey = "provider_extra_flags";
	constexpr std::string_view kCliIdleTimeoutSecondsKey = "cli_idle_timeout_seconds";
	constexpr std::string_view kActiveTurnInactivityTimeoutSecondsKey = "active_turn_inactivity_timeout_seconds";
	constexpr std::string_view kAcpSetupInactivityTimeoutSecondsKey = "acp_setup_inactivity_timeout_seconds";
	constexpr std::string_view kUiThemeKey = "ui_theme";
	constexpr std::string_view kShowProviderIconsInSidebarKey = "show_provider_icons_in_sidebar";
	constexpr std::string_view kShowWorktreePathInSidebarKey = "show_worktree_path_in_sidebar";
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
	constexpr std::string_view kMemoryLevelDefaultKey = "memory_level_default";
	constexpr std::string_view kMemoryIdleDelaySecondsKey = "memory_idle_delay_seconds";
	constexpr std::string_view kMemoryRecallBudgetBytesKey = "memory_recall_budget_bytes";
	constexpr std::string_view kGoalMaxLoopIterationsKey = "goal_max_loop_iterations";
	constexpr std::string_view kAcpTurnOutputLimitMiBKey = "acp_turn_output_limit_mib";
	constexpr std::string_view kUpdateChecksEnabledKey = "update_checks_enabled";
	constexpr std::string_view kUpdateLastCheckedAtKey = "update_last_checked_at";
	constexpr std::string_view kDismissedUpdateVersionsKey = "dismissed_update_versions";
	constexpr std::string_view kMemoryWorkerBindingsKey = "memory_worker_bindings";
	constexpr std::string_view kPermissionReviewerProviderIdKey = "permission_reviewer_provider_id";
	constexpr std::string_view kPermissionReviewerModelIdKey = "permission_reviewer_model_id";
	constexpr std::string_view kDefaultNewChatProviderIdKey = "default_new_chat_provider_id";
	constexpr std::string_view kProviderChatDefaultsKey = "provider_chat_defaults";
	constexpr std::string_view kMarkdownStoreDirectoryKey = "markdown_store_directory";
	constexpr std::string_view kDefaultEditorPresetIdKey = "default_editor_preset_id";
	constexpr std::string_view kEditorDefaultGroupsVersionKey = "editor_default_groups_version";
	constexpr std::string_view kEditorFileAssociationsKey = "editor_file_associations";
	constexpr std::string_view kMcpServersKey = "mcp_servers";
	constexpr std::string_view kFavoriteUamAgentIdsKey = "favorite_uam_agent_ids";
	constexpr std::string_view kUamAgentCycleShortcutKey = "uam_agent_cycle_shortcut";

	constexpr std::string_view kLegacyGeminiYoloModeKey = "gemini_yolo_mode";
	constexpr std::string_view kLegacyGeminiExtraFlagsKey = "gemini_extra_flags";

	std::string NormalizeProviderId(std::string_view value);

	bool IsValidSettingsText(std::string_view text)
	{
		bool saw_setting = false;
		bool saw_active_provider = false;
		bool saw_format_version = false;
		bool saw_complete = false;
		std::istringstream lines{std::string(text)};
		std::string line;
		while (std::getline(lines, line))
		{
			const std::string_view trimmed = uam::strings::TrimAsciiView(line);
			if (trimmed.empty())
			{
				continue;
			}
			const std::size_t equals_at = trimmed.find('=');
			if (equals_at == std::string_view::npos)
			{
				return false;
			}
			const std::string_view key = trimmed.substr(0, equals_at);
			const std::string_view value = trimmed.substr(equals_at + 1);
			saw_setting = true;
			if (key == kSettingsFormatVersionKey)
			{
				saw_format_version = value == "1";
				if (!saw_format_version)
				{
					return false;
				}
			}
			else if (key == kSettingsCompleteKey)
			{
				saw_complete = value == "1";
			}
			else if (key == kActiveProviderIdKey)
			{
				saw_active_provider = !uam::strings::IsBlank(uam::DecodeLineValue(value));
			}
		}
		return saw_setting && (!saw_format_version || (saw_active_provider && saw_complete));
	}

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

	std::string EncodeDismissedUpdateVersions(const std::map<std::string, std::string>& versions)
	{
		nlohmann::json encoded = nlohmann::json::object();
		for (const auto& [id, version] : versions)
		{
			const std::string safe_id = uam::strings::SafeLine(id, 128, true);
			const std::string safe_version = uam::strings::SafeLine(version, 128, true);
			if (!safe_id.empty() && !safe_version.empty())
			{
				encoded[safe_id] = safe_version;
			}
		}
		return encoded.dump();
	}

	void DecodeDismissedUpdateVersions(std::string_view value, std::map<std::string, std::string>& versions)
	{
		versions.clear();
		const nlohmann::json parsed = nlohmann::json::parse(value, nullptr, false);
		if (!parsed.is_object())
		{
			return;
		}
		for (auto it = parsed.begin(); it != parsed.end(); ++it)
		{
			if (!it.value().is_string())
			{
				continue;
			}
			const std::string safe_id = uam::strings::SafeLine(it.key(), 128, true);
			const std::string safe_version = uam::strings::SafeLine(it.value().get<std::string>(), 128, true);
			if (!safe_id.empty() && !safe_version.empty())
			{
				versions[safe_id] = safe_version;
			}
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

	ProviderChatDefaults NormalizeProviderChatDefaults(ProviderChatDefaults defaults, std::string_view provider_id)
	{
		defaults.model_id = uam::strings::Trim(defaults.model_id);
		defaults.reviewer_model_id = uam::strings::Trim(defaults.reviewer_model_id);
		if (!uam::provider_chat_defaults::IsAllowedModelId(defaults.reviewer_model_id)) defaults.reviewer_model_id.clear();
		defaults.feature_preference = defaults.feature_preference == "provider" ? "provider" : "uam";
		defaults.approval_mode = uam::approval_modes::NormalizePersistedProviderDefaultApprovalMode(defaults.approval_mode);
		defaults.command_safety_tier = uam::command_safety::NormalizeTier(defaults.command_safety_tier);
		defaults.reasoning_effort = uam::provider_chat_defaults::NormalizeReasoningEffort(provider_id, defaults.reasoning_effort);
		defaults.service_tier = uam::codex::NormalizeServiceTier(defaults.service_tier);
		defaults.memory_level = uam::memory_levels::Normalize(defaults.memory_level, defaults.memory_enabled);
		defaults.memory_enabled = uam::memory_levels::IsEnabled(defaults.memory_level);
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

		normalized_defaults = NormalizeProviderChatDefaults(defaults, normalized_provider_id);
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
			    defaults.command_safety_tier,
			    defaults.memory_enabled ? "1" : "0",
			    defaults.reasoning_effort,
			    defaults.service_tier,
			    defaults.memory_level,
			    defaults.small_model_mode ? "1" : "0",
			    defaults.reviewer_model_id,
			    defaults.feature_preference,
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
			const std::string legacy_or_tier = uam::DecodedLineFieldOr(fields, 3, "off");
			defaults.command_safety_tier = legacy_or_tier == "1" ? "yolo" : legacy_or_tier == "0" ? "aiReview" : legacy_or_tier;
			defaults.memory_enabled = BoolFieldOr(fields, 4, true);
			defaults.reasoning_effort = uam::DecodedLineFieldOr(fields, 5, "");
			defaults.service_tier = uam::DecodedLineFieldOr(fields, 6, "");
			defaults.memory_level = uam::memory_levels::Normalize(uam::DecodedLineFieldOr(fields, 7, ""), defaults.memory_enabled);
			defaults.small_model_mode = BoolFieldOr(fields, 8, false);
			defaults.reviewer_model_id = uam::DecodedLineFieldOr(fields, 9, "");
			defaults.feature_preference = uam::DecodedLineFieldOr(fields, 10, "uam");

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

	std::string EncodeFavoriteUamAgentIds(const std::vector<std::string>& ids)
	{
		return nlohmann::json(ids).dump();
	}

	void DecodeFavoriteUamAgentIds(std::string_view value, std::vector<std::string>& ids)
	{
		ids.clear();
		const nlohmann::json parsed = nlohmann::json::parse(value, nullptr, false);
		if (!parsed.is_array()) return;
		for (const nlohmann::json& item : parsed)
		{
			if (item.is_string()) ids.push_back(item.get_ref<const std::string&>());
		}
	}

	std::string NormalizeProviderId(std::string_view value)
	{
		return uam::provider_ids::NormalizeCliProviderAlias(value);
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
			settings.provider_chat_defaults[provider_key] = ProviderChatDefaults{"", uam::approval_modes::kDefaultApprovalMode, "off", settings.memory_enabled_default, "", "", settings.memory_level_default};
			return;
		}

		settings.provider_chat_defaults[provider_key] = NormalizeProviderChatDefaults(settings.provider_chat_defaults[provider_key], provider_key);
	}

	void ClampSettings(AppSettings& settings)
	{
		settings.active_provider_id = uam::strings::NonEmptyOrFallback(NormalizeProviderId(settings.active_provider_id), provider_build_config::FirstEnabledProviderId());
		settings.default_new_chat_provider_id = uam::strings::NonEmptyOrFallback(
		    NormalizeProviderId(settings.default_new_chat_provider_id), settings.active_provider_id);
		uam::settings::ClampRuntimeTimeoutSettings(settings);
		settings.ui_theme = uam::settings::NormalizeThemeId(settings.ui_theme);
		uam::settings::ClampWindowSettings(settings);
		uam::settings::ClampMemorySettings(settings);
		settings.memory_level_default = uam::memory_levels::Normalize(settings.memory_level_default, settings.memory_enabled_default);
		settings.memory_enabled_default = uam::memory_levels::IsEnabled(settings.memory_level_default);
		uam::settings::ClampGoalSettings(settings);
		uam::settings::ClampAcpOutputSettings(settings);
		settings.default_editor_preset_id = uam::editor_file_associations::NormalizeEditorPresetId(settings.default_editor_preset_id);
		uam::editor_file_associations::NormalizeEditorFileAssociations(settings.editor_file_associations);
		if (!uam::mcp_server_config::NormalizeAndValidate(settings.mcp_servers)) settings.mcp_servers.clear();
		uam::settings::NormalizeUamAgentPreferences(settings);
		uam::editor_file_associations::AppendMissingDefaultEditorGroups(settings);
		NormalizeMemoryWorkerBindings(settings.memory_worker_bindings);
		settings.permission_reviewer_provider_id = NormalizeProviderId(settings.permission_reviewer_provider_id);
		settings.permission_reviewer_model_id = uam::strings::Trim(settings.permission_reviewer_model_id);
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

bool SettingsStore::Save(const std::filesystem::path& settings_file, const AppSettings& settings)
{
	uam::paths::CreateDirectoriesNoThrow(settings_file.parent_path());

	AppSettings normalized = settings;
	ClampSettings(normalized);

	std::ostringstream lines;
	WriteRawSetting(lines, kSettingsFormatVersionKey, "1");
	WriteEncodedSetting(lines, kActiveProviderIdKey, normalized.active_provider_id);
	WriteEncodedSetting(lines, kProviderExtraFlagsKey, normalized.provider_extra_flags);
	WriteSettingValue(lines, kCliIdleTimeoutSecondsKey, normalized.cli_idle_timeout_seconds);
	WriteSettingValue(lines, kActiveTurnInactivityTimeoutSecondsKey, normalized.active_turn_inactivity_timeout_seconds);
	WriteSettingValue(lines, kAcpSetupInactivityTimeoutSecondsKey, normalized.acp_setup_inactivity_timeout_seconds);
	WriteEncodedSetting(lines, kUiThemeKey, normalized.ui_theme);
	WriteBoolSetting(lines, kShowProviderIconsInSidebarKey, normalized.show_provider_icons_in_sidebar);
	WriteBoolSetting(lines, kShowWorktreePathInSidebarKey, normalized.show_worktree_path_in_sidebar);
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
	WriteEncodedSetting(lines, kMemoryLevelDefaultKey, normalized.memory_level_default);
	WriteSettingValue(lines, kMemoryIdleDelaySecondsKey, normalized.memory_idle_delay_seconds);
	WriteSettingValue(lines, kMemoryRecallBudgetBytesKey, normalized.memory_recall_budget_bytes);
	WriteSettingValue(lines, kGoalMaxLoopIterationsKey, normalized.goal_max_loop_iterations);
	WriteSettingValue(lines, kAcpTurnOutputLimitMiBKey, normalized.acp_turn_output_limit_mib);
	WriteBoolSetting(lines, kUpdateChecksEnabledKey, normalized.update_checks_enabled);
	WriteEncodedSetting(lines, kUpdateLastCheckedAtKey, normalized.update_last_checked_at);
	WriteEncodedSetting(lines, kDismissedUpdateVersionsKey, EncodeDismissedUpdateVersions(normalized.dismissed_update_versions));
	WriteRawSetting(lines, kMemoryWorkerBindingsKey, EncodeMemoryWorkerBindings(normalized.memory_worker_bindings));
	WriteEncodedSetting(lines, kPermissionReviewerProviderIdKey, normalized.permission_reviewer_provider_id);
	WriteEncodedSetting(lines, kPermissionReviewerModelIdKey, normalized.permission_reviewer_model_id);
	WriteEncodedSetting(lines, kDefaultNewChatProviderIdKey, normalized.default_new_chat_provider_id);
	WriteRawSetting(lines, kProviderChatDefaultsKey, EncodeProviderChatDefaults(normalized.provider_chat_defaults));
	WriteEncodedSetting(lines, kMarkdownStoreDirectoryKey, normalized.markdown_store_directory);
	WriteEncodedSetting(lines, kDefaultEditorPresetIdKey, normalized.default_editor_preset_id);
	WriteSettingValue(lines, kEditorDefaultGroupsVersionKey, normalized.editor_default_groups_version);
	WriteRawSetting(lines, kEditorFileAssociationsKey, EncodeEditorFileAssociations(normalized.editor_file_associations));
	WriteEncodedSetting(lines, kMcpServersKey, uam::mcp_server_config::Serialize(normalized.mcp_servers).dump());
	WriteEncodedSetting(lines, kFavoriteUamAgentIdsKey, EncodeFavoriteUamAgentIds(normalized.favorite_uam_agent_ids));
	WriteEncodedSetting(lines, kUamAgentCycleShortcutKey, normalized.uam_agent_cycle_shortcut);
	WriteRawSetting(lines, kSettingsCompleteKey, "1");
	return uam::io::WriteTextFileWithBackup(settings_file, lines.str());
}

SettingsLoadResult SettingsStore::Load(const std::filesystem::path& settings_file, AppSettings& settings)
{
	SettingsLoadResult result;
	bool legacy_yolo = false;
	std::string legacy_extra_flags;
	std::string text;
	const bool primary_exists = uam::paths::PathExistsNoThrow(settings_file);
	const bool primary_valid = uam::io::TryReadTextFile(settings_file, text) && IsValidSettingsText(text);
	if (!primary_valid)
	{
		const std::filesystem::path backup = uam::io::MakeBackupPath(settings_file);
		const bool backup_exists = uam::paths::PathExistsNoThrow(backup);
		std::string backup_text;
		if (uam::io::TryReadTextFile(backup, backup_text) && IsValidSettingsText(backup_text))
		{
			text = std::move(backup_text);
			result.recovered_from_backup = true;
			result.warning = "Recovered settings from the validated backup because the primary settings file was missing or invalid.";
		}
		else
		{
			ClampSettings(settings);
			result.unrecovered_error = primary_exists || backup_exists;
			if (result.unrecovered_error)
			{
				result.warning = "The primary settings file and its recovery backup are invalid. UAM did not overwrite either file.";
			}
			return result;
		}
	}
	result.loaded = true;

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
			legacy_yolo = uam::parse::BoolOr(value, legacy_yolo);
		}
		else if (key == kProviderExtraFlagsKey)
		{
			settings.provider_extra_flags = decoded_value;
		}
		else if (key == kLegacyGeminiYoloModeKey)
		{
			legacy_yolo = uam::parse::BoolOr(value, legacy_yolo);
		}
		else if (key == kLegacyGeminiExtraFlagsKey)
		{
			legacy_extra_flags = decoded_value;
		}
		else if (key == kCliIdleTimeoutSecondsKey)
		{
			settings.cli_idle_timeout_seconds = uam::parse::IntOr(value, settings.cli_idle_timeout_seconds);
		}
		else if (key == kActiveTurnInactivityTimeoutSecondsKey)
		{
			settings.active_turn_inactivity_timeout_seconds = uam::parse::IntOr(value, settings.active_turn_inactivity_timeout_seconds);
		}
		else if (key == kAcpSetupInactivityTimeoutSecondsKey)
		{
			settings.acp_setup_inactivity_timeout_seconds = uam::parse::IntOr(value, settings.acp_setup_inactivity_timeout_seconds);
		}
		else if (key == kUiThemeKey)
		{
			settings.ui_theme = uam::settings::NormalizeThemeId(decoded_value);
		}
		else if (key == kShowProviderIconsInSidebarKey)
		{
			settings.show_provider_icons_in_sidebar = uam::parse::BoolOr(value, settings.show_provider_icons_in_sidebar);
		}
		else if (key == kShowWorktreePathInSidebarKey)
		{
			settings.show_worktree_path_in_sidebar = uam::parse::BoolOr(value, settings.show_worktree_path_in_sidebar);
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
		else if (key == kMemoryLevelDefaultKey)
		{
			settings.memory_level_default = uam::memory_levels::Normalize(uam::DecodeLineValue(value), settings.memory_enabled_default);
		}
		else if (key == kMemoryIdleDelaySecondsKey)
		{
			settings.memory_idle_delay_seconds = uam::parse::IntOr(value, settings.memory_idle_delay_seconds);
		}
		else if (key == kMemoryRecallBudgetBytesKey)
		{
			settings.memory_recall_budget_bytes = uam::parse::IntOr(value, settings.memory_recall_budget_bytes);
		}
		else if (key == kGoalMaxLoopIterationsKey)
		{
			settings.goal_max_loop_iterations = uam::parse::IntOr(value, settings.goal_max_loop_iterations);
		}
		else if (key == kAcpTurnOutputLimitMiBKey)
		{
			settings.acp_turn_output_limit_mib = uam::parse::IntOr(value, settings.acp_turn_output_limit_mib);
		}
		else if (key == kUpdateChecksEnabledKey)
		{
			settings.update_checks_enabled = uam::parse::BoolOr(value, settings.update_checks_enabled);
		}
		else if (key == kUpdateLastCheckedAtKey)
		{
			settings.update_last_checked_at = decoded_value;
		}
		else if (key == kDismissedUpdateVersionsKey)
		{
			DecodeDismissedUpdateVersions(decoded_value, settings.dismissed_update_versions);
		}
		else if (key == kMemoryWorkerBindingsKey)
		{
			DecodeMemoryWorkerBindings(decoded_value, settings.memory_worker_bindings);
		}
		else if (key == kPermissionReviewerProviderIdKey)
		{
			settings.permission_reviewer_provider_id = decoded_value;
		}
		else if (key == kPermissionReviewerModelIdKey)
		{
			settings.permission_reviewer_model_id = decoded_value;
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
		else if (key == kMcpServersKey)
		{
			const nlohmann::json parsed = nlohmann::json::parse(decoded_value, nullptr, false);
			settings.mcp_servers = uam::mcp_server_config::Parse(parsed);
		}
		else if (key == kFavoriteUamAgentIdsKey)
		{
			DecodeFavoriteUamAgentIds(decoded_value, settings.favorite_uam_agent_ids);
		}
		else if (key == kUamAgentCycleShortcutKey)
		{
			settings.uam_agent_cycle_shortcut = decoded_value;
		}
	}

	if (settings.provider_extra_flags.empty() && !legacy_extra_flags.empty())
	{
		settings.provider_extra_flags = legacy_extra_flags;
	}
	ClampSettings(settings);
	if (legacy_yolo)
	{
		for (auto& [provider_id, defaults] : settings.provider_chat_defaults)
		{
			(void)provider_id;
			defaults.command_safety_tier = "yolo";
		}
	}
	return result;
}
