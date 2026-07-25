#pragma once

#include "common/memory/memory_levels.h"

#include "common/config/approval_modes.h"
#include "common/config/editor_file_associations.h"
#include "common/config/settings_normalization.h"
#include "common/config/voice_input_settings.h"
#include "common/models/app_models.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace uam::settings_frontend_json
{
	inline nlohmann::json SerializeMemoryWorkerBindings(const std::map<std::string, MemoryWorkerBinding>& bindings)
	{
		nlohmann::json bindings_json = nlohmann::json::object();
		for (const auto& entry : bindings)
		{
			const std::string chat_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(entry.first);
			const std::string worker_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(entry.second.worker_provider_id);
			if (chat_provider_id.empty() || worker_provider_id.empty())
			{
				continue;
			}

			bindings_json[chat_provider_id] = {
			    {"workerProviderId", worker_provider_id},
			    {"workerModelId", uam::strings::Trim(entry.second.worker_model_id)},
			};
		}
		return bindings_json;
	}

	inline nlohmann::json SerializeProviderChatDefaults(const std::map<std::string, ProviderChatDefaults>& defaults_by_provider)
	{
		nlohmann::json defaults_json = nlohmann::json::object();
		for (const auto& entry : defaults_by_provider)
		{
			const std::string provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(entry.first);
			if (provider_id.empty())
			{
				continue;
			}

			defaults_json[provider_id] = {
			    {"modelId", uam::strings::Trim(entry.second.model_id)},
			    {"approvalMode", uam::approval_modes::NormalizePersistedProviderDefaultApprovalMode(entry.second.approval_mode)},
			    {"autoApproveCommands", entry.second.auto_approve_commands},
			    {"memoryEnabled", entry.second.memory_enabled},
			    {"memoryLevel", uam::memory_levels::Normalize(entry.second.memory_level, entry.second.memory_enabled)},
			    {"reasoningEffort", uam::codex::NormalizeReasoningEffort(entry.second.reasoning_effort)},
			    {"serviceTier", uam::codex::NormalizeServiceTier(entry.second.service_tier)},
			    {"smallModelMode", entry.second.small_model_mode},
			};
		}
		return defaults_json;
	}

	inline std::string SerializeMarkdownStoreDirectory(const AppSettings& settings)
	{
		return uam::strings::Trim(settings.markdown_store_directory);
	}

	inline nlohmann::json SerializeEditorFileAssociations(const std::vector<EditorFileAssociation>& associations)
	{
		std::vector<EditorFileAssociation> normalized_associations = associations;
		uam::editor_file_associations::NormalizeEditorFileAssociations(normalized_associations);

		nlohmann::json associations_json = nlohmann::json::array();
		for (const EditorFileAssociation& association : normalized_associations)
		{
			associations_json.push_back({
			    {"id", association.id},
			    {"name", association.name},
			    {"extensions", association.extensions},
			    {"editorPresetId", association.editor_preset_id},
			});
		}
		return associations_json;
	}

	inline nlohmann::json SerializeCommonSettingsFields(const AppSettings& settings)
	{
		nlohmann::json settings_json;
		settings_json["activeProviderId"] = uam::provider_ids::NormalizeCliProviderAliasOrSelf(settings.active_provider_id);
		settings_json["theme"] = uam::settings::NormalizeThemeId(settings.ui_theme);
		settings_json["showProviderIconsInSidebar"] = settings.show_provider_icons_in_sidebar;
		settings_json["showWorktreePathInSidebar"] = settings.show_worktree_path_in_sidebar;
		settings_json["memoryEnabledDefault"] = settings.memory_enabled_default;
		settings_json["memoryLevelDefault"] = uam::memory_levels::Normalize(settings.memory_level_default, settings.memory_enabled_default);
		settings_json["memoryIdleDelaySeconds"] = std::clamp(settings.memory_idle_delay_seconds, uam::settings::kMinMemoryIdleDelaySeconds, uam::settings::kMaxMemoryIdleDelaySeconds);
		settings_json["memoryRecallBudgetBytes"] = std::clamp(settings.memory_recall_budget_bytes, uam::settings::kMinMemoryRecallBudgetBytes, uam::settings::kMaxMemoryRecallBudgetBytes);
		settings_json["goalMaxLoopIterations"] = std::max(0, settings.goal_max_loop_iterations);
		settings_json["updateChecksEnabled"] = settings.update_checks_enabled;
		settings_json["updateLastCheckedAt"] = settings.update_last_checked_at;
		settings_json["dismissedUpdateVersions"] = settings.dismissed_update_versions;
		settings_json["defaultNewChatProviderId"] = uam::provider_ids::NormalizeCliProviderAliasOrSelf(settings.default_new_chat_provider_id);
		settings_json["providerChatDefaults"] = SerializeProviderChatDefaults(settings.provider_chat_defaults);
		settings_json["defaultEditorPresetId"] = uam::editor_file_associations::NormalizeEditorPresetId(settings.default_editor_preset_id);
		settings_json["editorFileAssociations"] = SerializeEditorFileAssociations(settings.editor_file_associations);
		settings_json["voiceInputMode"] = uam::voice_input::NormalizeMode(settings.voice_input_mode);
		settings_json["voiceInputServerBaseUrl"] = uam::strings::Trim(settings.voice_input_server_base_url);
		settings_json["voiceInputServerEndpoint"] = uam::strings::Trim(settings.voice_input_server_endpoint);
		settings_json["voiceInputServerModel"] = uam::strings::Trim(settings.voice_input_server_model);
		settings_json["voiceInputApiKeyEnv"] = uam::strings::Trim(settings.voice_input_api_key_env);
		settings_json["voiceInputCapabilities"] = {
		    {"system", {{"supported", true}, {"reason", ""}}},
		    {"local", {{"supported", false}, {"reason", "Local AI transcription is coming soon."}}},
#if defined(__APPLE__)
		    {"server", {{"supported", true}, {"reason", ""}}},
#else
		    {"server", {{"supported", false}, {"reason", "Server transcription audio capture is not available on this platform yet."}}},
#endif
		};
		return settings_json;
	}

	inline nlohmann::json SerializeLiveSettingsFields(const AppSettings& settings, const std::string& memory_last_status)
	{
		nlohmann::json settings_json = SerializeCommonSettingsFields(settings);
		settings_json["memoryLastStatus"] = memory_last_status;
		settings_json["markdownStoreDirectory"] = SerializeMarkdownStoreDirectory(settings);
		settings_json["memoryWorkerBindings"] = SerializeMemoryWorkerBindings(settings.memory_worker_bindings);
		return settings_json;
	}

	inline nlohmann::json SerializeFingerprintSettingsFields(const AppSettings& settings)
	{
		nlohmann::json settings_json = SerializeCommonSettingsFields(settings);
		settings_json["markdownStoreDirectory"] = SerializeMarkdownStoreDirectory(settings);
		return settings_json;
	}
} // namespace uam::settings_frontend_json
