#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/persistence_coordinator.h"
#include "app/theme_service.h"
#include "cef/cef_push.h"
#include "common/config/editor_file_associations.h"
#include "common/config/settings_normalization.h"
#include "common/config/voice_input_settings.h"
#include "common/memory/memory_levels.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Settings handlers (memory, provider defaults, editor, CLI version, theme)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	std::vector<EditorFileAssociation> ParseEditorFileAssociationsPayload(const nlohmann::json& payload)
	{
		std::vector<EditorFileAssociation> associations;
		const nlohmann::json* file_associations = uam::nlohmann_json::FindArrayField(payload, "fileAssociations");
		if (file_associations == nullptr)
		{
			return associations;
		}

		for (const nlohmann::json& item : *file_associations)
		{
			if (!item.is_object())
			{
				continue;
			}

			EditorFileAssociation association;
			association.id = uam::nlohmann_json::TrimmedStringValue(item, {"id"});
			association.name = uam::nlohmann_json::TrimmedStringValue(item, {"name"});
			if (association.id.empty())
			{
				association.id = "editor-group-" + std::to_string(associations.size() + 1);
			}
			association.editor_preset_id = uam::nlohmann_json::TrimmedStringValue(item, {"editorPresetId"});
			association.extensions = uam::nlohmann_json::StringArrayField(item, "extensions");

			std::optional<EditorFileAssociation> normalized_association = uam::editor_file_associations::NormalizeEditorFileAssociation(std::move(association));
			if (!normalized_association)
			{
				continue;
			}
			associations.push_back(std::move(*normalized_association));
		}

		return associations;
	}

	std::string NormalizeCliVersionProviderId(std::string_view provider_id)
	{
		const std::string normalized = uam::provider_ids::NormalizeCliProviderAlias(provider_id);
		return uam::provider_ids::IsVersionManagedCliProviderId(normalized) ? normalized : std::string{};
	}

	std::string FallbackCliVersionProviderId(const std::string& active_provider_id)
	{
		return uam::provider_ids::NormalizeVersionManagedCliProviderId(active_provider_id);
	}

	std::string CliVersionProviderFromPayloadOrSelection(const uam::AppState& app, const nlohmann::json& payload)
	{
		const std::string requested = NormalizeCliVersionProviderId(uam::nlohmann_json::TrimmedStringValue(payload, {"providerId"}));
		if (!requested.empty())
		{
			return requested;
		}

		const std::string selected_provider_id = NormalizeCliVersionProviderId(ChatDomainService().SelectedChatProviderId(app));
		if (!selected_provider_id.empty())
		{
			return selected_provider_id;
		}

		return FallbackCliVersionProviderId(app.settings.active_provider_id);
	}
} // namespace

void UamQueryHandler::HandleSetMemorySettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const AppSettings previous = m_app.settings;
	if (const std::optional<bool> enabled_default = uam::nlohmann_json::BoolFieldStrict(payload, "enabledDefault"))
	{
		m_app.settings.memory_enabled_default = *enabled_default;
	}
	m_app.settings.memory_level_default = uam::memory_levels::Normalize(uam::nlohmann_json::TrimmedStringValueOr(payload, "levelDefault", m_app.settings.memory_level_default), m_app.settings.memory_enabled_default);
	m_app.settings.memory_enabled_default = uam::memory_levels::IsEnabled(m_app.settings.memory_level_default);
	if (const std::optional<int> idle_delay_seconds = uam::nlohmann_json::IntFieldStrict(payload, "idleDelaySeconds"))
	{
		m_app.settings.memory_idle_delay_seconds = *idle_delay_seconds;
	}
	if (const std::optional<int> recall_budget_bytes = uam::nlohmann_json::IntFieldStrict(payload, "recallBudgetBytes"))
	{
		m_app.settings.memory_recall_budget_bytes = *recall_budget_bytes;
	}
	if (const std::optional<int> max_loop_iterations = uam::nlohmann_json::IntFieldStrict(payload, "goalMaxLoopIterations"))
	{
		m_app.settings.goal_max_loop_iterations = *max_loop_iterations;
	}
	uam::settings::ClampMemorySettings(m_app.settings);
	uam::settings::ClampGoalSettings(m_app.settings);
	if (const nlohmann::json* worker_bindings = uam::nlohmann_json::FindObjectField(payload, "workerBindings"); worker_bindings != nullptr)
	{
		for (auto it = worker_bindings->begin(); it != worker_bindings->end(); ++it)
		{
			if (!it.value().is_object())
			{
				continue;
			}
			const std::string chat_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(it.key());
			const std::string worker_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(it.value().value("workerProviderId", ""));
			const std::string worker_model_id = uam::strings::Trim(it.value().value("workerModelId", ""));
			if (chat_provider_id.empty() || worker_provider_id.empty() || ProviderProfileStore::FindById(m_app.provider_profiles, chat_provider_id) == nullptr || ProviderProfileStore::FindById(m_app.provider_profiles, worker_provider_id) == nullptr)
			{
				continue;
			}
			m_app.settings.memory_worker_bindings[chat_provider_id] = MemoryWorkerBinding{worker_provider_id, worker_model_id};
		}
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist memory settings."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetVoiceInputSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	AppSettings next = m_app.settings;
	next.voice_input_mode = uam::voice_input::NormalizeMode(payload.value("mode", next.voice_input_mode));
	next.voice_input_server_base_url = uam::strings::Trim(payload.value("serverBaseUrl", next.voice_input_server_base_url));
	next.voice_input_server_endpoint = uam::strings::Trim(payload.value("serverEndpoint", next.voice_input_server_endpoint));
	next.voice_input_server_model = uam::strings::Trim(payload.value("serverModel", next.voice_input_server_model));
	next.voice_input_api_key_env = uam::strings::Trim(payload.value("apiKeyEnv", next.voice_input_api_key_env));

	if (next.voice_input_mode == uam::voice_input::kServerMode)
	{
		std::string error;
		if (!uam::voice_input::ValidateServerSettings(next.voice_input_server_base_url, next.voice_input_server_endpoint,
		                                                    next.voice_input_server_model, next.voice_input_api_key_env, &error))
		{
			cb->Failure(400, error);
			return;
		}
	}

	const AppSettings previous = m_app.settings;
	m_app.settings = std::move(next);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist voice input settings."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetProviderChatDefaults(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const AppSettings previous = m_app.settings;
	const std::string requested_default_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(payload.value("defaultProviderId", m_app.settings.default_new_chat_provider_id));
	if (!requested_default_provider_id.empty())
	{
		const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, requested_default_provider_id);
		if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
		{
			cb->Failure(400, "Unsupported default provider: " + requested_default_provider_id);
			return;
		}
		m_app.settings.default_new_chat_provider_id = provider->id;
	}

	if (const nlohmann::json* defaults_by_provider = uam::nlohmann_json::FindObjectField(payload, "defaults"); defaults_by_provider != nullptr)
	{
		for (auto it = defaults_by_provider->begin(); it != defaults_by_provider->end(); ++it)
		{
			const std::string provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(it.key());
			const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, provider_id);
			if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
			{
				continue;
			}
			const ProviderChatDefaults fallback = DefaultsForProvider(m_app.settings, provider->id);
			ProviderChatDefaults defaults = ProviderDefaultsFromSettingsPayload(it.value(), fallback, provider->id);
			m_app.settings.provider_chat_defaults[provider->id] = defaults;
		}
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist provider chat defaults."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetSidebarSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<bool> show_provider_icons = uam::nlohmann_json::BoolFieldStrict(payload, "showProviderIconsInSidebar");
	const std::optional<bool> show_worktree_path = uam::nlohmann_json::BoolFieldStrict(payload, "showWorktreePathInSidebar");
	if (!show_provider_icons || !show_worktree_path)
	{
		cb->Failure(400, "Sidebar settings must be booleans.");
		return;
	}

	const AppSettings previous = m_app.settings;
	m_app.settings.show_provider_icons_in_sidebar = *show_provider_icons;
	m_app.settings.show_worktree_path_in_sidebar = *show_worktree_path;
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist sidebar settings."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetUpdateSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const AppSettings previous = m_app.settings;
	m_app.settings.update_checks_enabled = payload.value("enabled", m_app.settings.update_checks_enabled);
	m_app.settings.update_last_checked_at = uam::strings::SafeLine(
	    payload.value("lastCheckedAt", m_app.settings.update_last_checked_at), 64, true);

	if (const nlohmann::json* dismissed = uam::nlohmann_json::FindObjectField(payload, "dismissedVersions"); dismissed != nullptr)
	{
		m_app.settings.dismissed_update_versions.clear();
		for (auto it = dismissed->begin(); it != dismissed->end(); ++it)
		{
			if (!it.value().is_string())
			{
				continue;
			}
			const std::string id = uam::strings::SafeLine(it.key(), 128, true);
			const std::string version = uam::strings::SafeLine(it.value().get<std::string>(), 128, true);
			if (!id.empty() && !version.empty())
			{
				m_app.settings.dismissed_update_versions[id] = version;
			}
		}
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist update settings."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetEditorSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const AppSettings previous = m_app.settings;
	const std::string default_editor_preset_id = uam::strings::Trim(payload.value("defaultEditorPresetId", m_app.settings.default_editor_preset_id));
	m_app.settings.default_editor_preset_id = uam::editor_file_associations::NormalizeEditorPresetId(default_editor_preset_id);

	std::vector<EditorFileAssociation> associations = ParseEditorFileAssociationsPayload(payload);
	if (associations.empty())
	{
		associations = uam::editor_file_associations::DefaultEditorFileAssociations();
	}
	m_app.settings.editor_file_associations = std::move(associations);
	m_app.settings.editor_default_groups_version = uam::editor_file_associations::kDefaultGroupsVersion;

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist editor settings."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRefreshCliProviderVersion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string provider_id = CliVersionProviderFromPayloadOrSelection(m_app, payload);
	if (ProviderProfileStore::FindById(m_app.provider_profiles, provider_id) == nullptr)
	{
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	}

	ProviderCliCompatibilityService().StartProviderVersionCheck(m_app, provider_id, true);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRefreshAllCliProviderVersions(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	ProviderCliCompatibilityService().StartVersionCheck(m_app, true);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleApplyCliProviderVersion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string provider_id = CliVersionProviderFromPayloadOrSelection(m_app, payload);
	const std::string version = uam::strings::Trim(payload.value("version", ""));
	std::string error;
	if (!ProviderCliCompatibilityService().StartInstallProviderVersion(m_app, provider_id, version, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to start provider CLI install."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}


void UamQueryHandler::HandleSetTheme(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string requested_theme = uam::strings::TrimAndLowerAscii(payload.value("theme", "dark"));
	const std::string theme = uam::settings::NormalizeThemeId(requested_theme);
	if (theme != requested_theme || (ThemeService::IsCustomThemeId(theme) && !ThemeService::Exists(m_app.data_root, theme)))
	{
		cb->Failure(400, "Theme does not exist or has an invalid id.");
		return;
	}
	const std::string previous_theme = m_app.settings.ui_theme;
	m_app.settings.ui_theme = theme;
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings.ui_theme = previous_theme;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist theme."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
