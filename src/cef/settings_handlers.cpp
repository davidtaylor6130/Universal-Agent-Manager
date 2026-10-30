#include "app/storage_retention_service.h"
#include "app/data_folder_settings_service.h"
#include "app/central_instruction_file_service.h"
#include "common/config/voice_input_settings.h"
#include "app/execution_routing_integration.h"
#include "common/config/settings_store.h"
#include "common/config/host_scoped_settings.h"
#include "common/config/provider_availability.h"
#include <stdexcept>
#include <limits>
#include "common/paths/app_paths.h"
#include "common/config/central_provider_configuration.h"
#include "cef/uam_query_handler.h"
#include "common/companion/connection_lifecycle.h"
#include "common/companion/pairing_certificate.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/persistence_coordinator.h"
#include "app/theme_service.h"
#include "app/provider_mcp_settings_service.h"
#include "cef/cef_push.h"
#include "cef/companion_server.h"
#include "common/config/editor_file_associations.h"
#include "common/config/companion_network.h"
#include "common/config/settings_normalization.h"
#include "common/config/mcp_server_config.h"
#include "common/memory/memory_levels.h"
#include "common/paths/path_utils.h"
#include "common/platform/file_explorer_application.h"
#include "common/platform/powershell_selection.h"
#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif
#include "common/provider/excluded_provider_data.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include "common/config/execution_host_config.h"

#include <fstream>
#include <ranges>
#include <optional>
#include <set>
#include <unordered_set>
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
		const std::string provider_id = uam::provider_ids::NormalizeVersionManagedCliProviderId(active_provider_id);
		return provider_id == uam::provider_ids::kExcludedCliProviderId ? std::string{} : provider_id; // Antigravity-only builds manage no CLI versions.
	}

	std::string CliVersionProviderFromPayloadOrSelection(const uam::AppState& app, const nlohmann::json& payload)
	{
		const std::string requested = NormalizeCliVersionProviderId(uam::nlohmann_json::TrimmedStringValue(payload, {"providerId"}));
		if (payload.contains("providerId"))
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

	std::filesystem::path CompanionConfigPath(const uam::AppState& app)
	{
		return app.data_root / "companion.json";
	}

	bool ReadCompanionConfig(const uam::AppState& app, nlohmann::json& config)
	{
		std::string text;
		if (!uam::io::TryReadTextFile(CompanionConfigPath(app), text)) return false;
		try
		{
			config = nlohmann::json::parse(text);
			return config.is_object();
		}
		catch (const nlohmann::json::exception&)
		{
			return false;
		}
	}

	bool KeychainPromptsAllowed(const nlohmann::json& config)
	{
		return config.contains("allow_keychain_prompts") && config["allow_keychain_prompts"].is_boolean() && config["allow_keychain_prompts"].get<bool>();
	}

	bool WriteCompanionConfig(const uam::AppState& app, const nlohmann::json& config)
	{
		return uam::io::WriteTextFileWithBackup(CompanionConfigPath(app), config.dump(2) + "\n");
	}

	std::string CompanionUrl(const nlohmann::json& config)
	{
		const std::string origin = config.contains("origin") && config["origin"].is_string()
			? uam::strings::Trim(config["origin"].get<std::string>()) : std::string{};
		if (!origin.empty()) return origin + "/companion";
		std::string port = "58948";
		if (config.contains("port"))
		{
			if (config["port"].is_string()) port = config["port"].get<std::string>();
			else if (config["port"].is_number_integer()) port = std::to_string(config["port"].get<int>());
		}
		return "http://127.0.0.1:" + port + "/companion";
	}

	nlohmann::json CompanionSettingsResponse(const nlohmann::json& config)
	{
		const bool configured = config.is_object() && config.contains("token_file") && config["token_file"].is_string();
		const bool enabled = config.contains("enabled") && config["enabled"].is_boolean() && config["enabled"].get<bool>();
		return {
			{"configured", configured},
			{"enabled", enabled},
			{"url", CompanionUrl(config)},
			{"restartRequired", true},
		};
	}
} // namespace

namespace
{
	SettingsMemoryCanonicalValues CaptureMemorySettings(const AppSettings& value)
	{
		return {value.memory_enabled_default, value.memory_level_default, value.memory_idle_delay_seconds,
		    value.memory_recall_budget_bytes, value.goal_max_loop_iterations, value.acp_setup_inactivity_timeout_seconds,
		    value.acp_turn_output_limit_mib, value.memory_worker_bindings,
		    value.permission_reviewer_provider_id, value.permission_reviewer_model_id};
	}

	void ApplyMemorySettings(AppSettings& target, SettingsMemoryCanonicalValues value) noexcept
	{
		target.memory_enabled_default = value.memory_enabled_default;
		target.memory_level_default.swap(value.memory_level_default);
		target.memory_idle_delay_seconds = value.memory_idle_delay_seconds;
		target.memory_recall_budget_bytes = value.memory_recall_budget_bytes;
		target.goal_max_loop_iterations = value.goal_max_loop_iterations;
		target.acp_setup_inactivity_timeout_seconds = value.acp_setup_inactivity_timeout_seconds;
		target.acp_turn_output_limit_mib = value.acp_turn_output_limit_mib;
		target.memory_worker_bindings.swap(value.memory_worker_bindings);
		target.permission_reviewer_provider_id.swap(value.permission_reviewer_provider_id);
		target.permission_reviewer_model_id.swap(value.permission_reviewer_model_id);
	}

	nlohmann::json MemorySettingsValues(const AppSettings& value)
	{
		nlohmann::json bindings = nlohmann::json::object();
		for (const auto& [id, binding] : value.memory_worker_bindings)
			if (!uam::excluded_provider_data::IsExcluded(id) && !uam::excluded_provider_data::IsExcluded(binding.worker_provider_id)) bindings[id] = {{"workerProviderId", binding.worker_provider_id}, {"workerModelId", binding.worker_model_id}};
		return {{"memoryEnabledDefault", value.memory_enabled_default}, {"memoryLevelDefault", value.memory_level_default},
		    {"memoryIdleDelaySeconds", value.memory_idle_delay_seconds}, {"memoryRecallBudgetBytes", value.memory_recall_budget_bytes},
		    {"goalMaxLoopIterations", value.goal_max_loop_iterations}, {"acpSetupInactivityTimeoutSeconds", value.acp_setup_inactivity_timeout_seconds},
		    {"acpTurnOutputLimitMiB", value.acp_turn_output_limit_mib}, {"memoryWorkerBindings", std::move(bindings)},
		    {"permissionReviewerProviderId", value.permission_reviewer_provider_id}, {"permissionReviewerModelId", value.permission_reviewer_model_id}};
	}

	nlohmann::json MemorySettingsPrimarySnapshot(const uam::AppState& app)
	{
		nlohmann::json result = {{"scope", app.settings_publication_scope}, {"revision", app.settings_publication_revision}, {"primaryValidated", false}};
		try
		{
			AppSettings primary;
			const auto loaded = SettingsStore::LoadPrimary(AppPaths::SettingsFilePath(app.data_root), primary);
			if (loaded.loaded && !loaded.recovered_from_backup && !loaded.unrecovered_error)
			{
				result["values"] = MemorySettingsValues(primary);
				result["primaryValidated"] = true;
			}
		}
		catch (...) {} // No backup/default values become a primary observation.
		return result;
	}

	bool MemoryRequestText(const nlohmann::json& value)
	{
		if (!value.is_string()) return false;
		const auto& text = value.get_ref<const std::string&>();
		return !text.empty() && text.size() <= 128 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; });
	}

	bool ValidMemorySettingsPayload(const nlohmann::json& payload)
	{
		if (!payload.is_object()) return false;
		if (payload.contains("enabledDefault") && !payload["enabledDefault"].is_boolean()) return false;
		for (const auto* key : {"levelDefault", "permissionReviewerProviderId", "permissionReviewerModelId"})
			if (payload.contains(key) && !payload[key].is_string()) return false;
		for (const auto* key : {"idleDelaySeconds", "recallBudgetBytes", "goalMaxLoopIterations", "acpTurnOutputLimitMiB", "acpSetupInactivityTimeoutSeconds"})
		{
			if (!payload.contains(key)) continue;
			const auto& value = payload[key];
			if (!value.is_number_integer()) return false;
			if (value.is_number_unsigned()) { if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return false; }
			else if (value.get<std::int64_t>() < std::numeric_limits<int>::min() || value.get<std::int64_t>() > std::numeric_limits<int>::max()) return false;
		}
		if (payload.contains("workerBindings"))
		{
			if (!payload["workerBindings"].is_object()) return false;
			for (const auto& [id, binding] : payload["workerBindings"].items())
			{
				if (!binding.is_object()) return false;
				for (const auto* key : {"workerProviderId", "workerModelId"})
					if (binding.contains(key) && !binding[key].is_string()) return false;
			}
		}
		return true;
	}
}

void UamQueryHandler::HandleGetMemorySettings(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	// This read fences older stamped writes; the snapshot never falls back to backup/defaults.
	if (!PersistenceCoordinator().AdvanceSettingsPublication(m_app))
	{
		cb->Failure(503, "Settings publication fence is unavailable.");
		return;
	}
	cb->Success(MemorySettingsPrimarySnapshot(m_app).dump());
}

void UamQueryHandler::HandleSetMemorySettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string request_id;
	if (payload.contains("memoryRequestId"))
	{
		if (!MemoryRequestText(payload["memoryRequestId"])) { cb->Failure(400, "Invalid memory request identity."); return; }
		request_id = payload["memoryRequestId"].get<std::string>();
	}
	if (!ValidMemorySettingsPayload(payload)) { cb->Failure(400, "Invalid memory settings payload."); return; }
	const bool scope_supplied = payload.contains("expectedSettingsScope");
	const bool revision_supplied = payload.contains("expectedSettingsRevision");
	const bool request_supplied = payload.contains("memoryRequestId");
	if ((scope_supplied || revision_supplied || request_supplied) && !(scope_supplied && revision_supplied && request_supplied))
	{
		cb->Failure(400, "Supply the complete settings publication fence and request identity.");
		return;
	}
	if (scope_supplied != revision_supplied || (scope_supplied && (!MemoryRequestText(payload["expectedSettingsScope"]) ||
	    !payload["expectedSettingsRevision"].is_number_integer() ||
	    (payload["expectedSettingsRevision"].is_number_unsigned() ? payload["expectedSettingsRevision"].get<std::uint64_t>() > PersistenceCoordinator::kMaxSettingsPublicationRevision :
	        payload["expectedSettingsRevision"].get<std::int64_t>() < 0 || static_cast<std::uint64_t>(payload["expectedSettingsRevision"].get<std::int64_t>()) > PersistenceCoordinator::kMaxSettingsPublicationRevision))))
	{
		cb->Failure(400, "Invalid settings publication fence.");
		return;
	}
	if (!PersistenceCoordinator().EnsureSettingsPublicationScope(m_app)) { cb->Failure(503, "Settings publication scope is unavailable."); return; }
	// Only complete stamped clients understand noncommitted typed Success bodies.
	const bool typed_outcomes = scope_supplied && !request_id.empty();
	const auto reject = [&]()
	{
		if (!typed_outcomes) { cb->Failure(409, "Settings were not confirmed saved. Refresh current values before retrying."); return; }
		auto response = MemorySettingsPrimarySnapshot(m_app);
		response["memoryRequestId"] = request_id;
		response["outcome"] = "rejected";
		cb->Success(response.dump());
	};
	if (scope_supplied && (payload["expectedSettingsScope"] != m_app.settings_publication_scope ||
	    payload["expectedSettingsRevision"].get<std::uint64_t>() != m_app.settings_publication_revision)) { reject(); return; }
	if (!PersistenceCoordinator().ResolveMemorySettingsUncertainty(m_app))
	{
		if (!typed_outcomes) { cb->Failure(500, "Settings were not confirmed saved. Refresh current values before retrying."); return; }
		auto response = nlohmann::json{{"memoryRequestId", request_id}, {"outcome", "unknown"},
		    {"scope", m_app.settings_publication_scope}, {"revision", m_app.settings_publication_revision}, {"primaryValidated", false}};
		cb->Success(response.dump());
		return;
	}
	// All supplied field types have been validated before this private candidate is built.
	AppSettings candidate = m_app.settings;
	if (const std::optional<bool> enabled_default = uam::nlohmann_json::BoolFieldStrict(payload, "enabledDefault"))
	{
		candidate.memory_enabled_default = *enabled_default;
	}
	candidate.memory_level_default = uam::memory_levels::Normalize(uam::nlohmann_json::TrimmedStringValueOr(payload, "levelDefault", candidate.memory_level_default), candidate.memory_enabled_default);
	candidate.memory_enabled_default = uam::memory_levels::IsEnabled(candidate.memory_level_default);
	if (const std::optional<int> idle_delay_seconds = uam::nlohmann_json::IntFieldStrict(payload, "idleDelaySeconds"))
	{
		candidate.memory_idle_delay_seconds = *idle_delay_seconds;
	}
	if (const std::optional<int> recall_budget_bytes = uam::nlohmann_json::IntFieldStrict(payload, "recallBudgetBytes"))
	{
		candidate.memory_recall_budget_bytes = *recall_budget_bytes;
	}
	if (const std::optional<int> max_loop_iterations = uam::nlohmann_json::IntFieldStrict(payload, "goalMaxLoopIterations"))
	{
		candidate.goal_max_loop_iterations = *max_loop_iterations;
	}
	if (const std::optional<int> output_limit_mib = uam::nlohmann_json::IntFieldStrict(payload, "acpTurnOutputLimitMiB"))
	{
		candidate.acp_turn_output_limit_mib = *output_limit_mib;
	}
	if (const std::optional<int> setup_timeout_seconds = uam::nlohmann_json::IntFieldStrict(payload, "acpSetupInactivityTimeoutSeconds"))
	{
		candidate.acp_setup_inactivity_timeout_seconds = *setup_timeout_seconds;
	}
	uam::settings::ClampMemorySettings(candidate);
	uam::settings::ClampGoalSettings(candidate);
	uam::settings::ClampAcpOutputSettings(candidate);
	uam::settings::ClampRuntimeTimeoutSettings(candidate);
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
			candidate.memory_worker_bindings[chat_provider_id] = MemoryWorkerBinding{worker_provider_id, worker_model_id};
		}
	}
	if (payload.contains("permissionReviewerProviderId"))
	{
		const std::string provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(payload.value("permissionReviewerProviderId", ""));
		candidate.permission_reviewer_provider_id = ProviderProfileStore::FindById(m_app.provider_profiles, provider_id) == nullptr ? "" : provider_id;
		// An explicit choice replaces a remembered excluded provider (see AppSettings::excluded_provider_selections).
		candidate.excluded_provider_selections.erase("permission_reviewer_provider_id");
		m_app.settings.excluded_provider_selections.erase("permission_reviewer_provider_id");
	}
	if (payload.contains("permissionReviewerModelId"))
	{
		candidate.permission_reviewer_model_id = uam::strings::Trim(payload.value("permissionReviewerModelId", ""));
	}

	SettingsMemoryCanonicalValues previous = CaptureMemorySettings(m_app.settings);
	ApplyMemorySettings(m_app.settings, CaptureMemorySettings(candidate));
	SettingsMemoryWriteReceipt receipt;
	try
	{
		(void)PersistenceCoordinator().SaveSettings(m_app, &receipt, m_memorySettingsStageGateForTests);
	}
	catch (...)
	{
		// A receipt-less unexpected failure cannot prove non-commit.
		receipt.outcome = SettingsMemoryWriteOutcome::UnknownAfterReplacePermission;
	}
	const bool committed = receipt.outcome == SettingsMemoryWriteOutcome::Committed && receipt.canonical_values_ready;
	if (committed)
	{
		ApplyMemorySettings(m_app.settings, std::move(receipt.canonical));
		m_app.memory_settings_uncertain = false;
	}
	else if (receipt.outcome == SettingsMemoryWriteOutcome::RejectedBeforeReplace)
		ApplyMemorySettings(m_app.settings, std::move(previous));
	else m_app.memory_settings_uncertain = true;

	// Freeze action-local facts before any notification callback can run.
	nlohmann::json response;
	if (committed)
	{
		response = {{"memoryRequestId", request_id}, {"outcome", "committed"},
		    {"scope", m_app.settings_publication_scope}, {"revision", m_app.settings_publication_revision},
		    {"primaryValidated", true}, {"values", MemorySettingsValues(m_app.settings)}};
	}
	else if (receipt.outcome == SettingsMemoryWriteOutcome::RejectedBeforeReplace)
	{
		response = MemorySettingsPrimarySnapshot(m_app);
		response["memoryRequestId"] = request_id;
		response["outcome"] = "rejected";
	}
	else response = {{"memoryRequestId", request_id}, {"outcome", "unknown"},
	    {"scope", m_app.settings_publication_scope}, {"revision", m_app.settings_publication_revision}, {"primaryValidated", false}};
	const std::string terminal_response = response.dump();
	try { if (m_memorySettingsAfterSaveForTests) m_memorySettingsAfterSaveForTests(); } catch (...) {}
	if (committed) { try { uam::PushStateUpdateIfChanged(browser, m_app); } catch (...) {} }
	// One terminal callback attempt. Delivery failure cannot turn a captured commit into rejection.
	try
	{
		if (committed || typed_outcomes) cb->Success(terminal_response);
		else cb->Failure(500, "Settings were not confirmed saved. Refresh current values before retrying.");
	}
	catch (...) {}

}

void UamQueryHandler::HandleSetMcpServers(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string scope;
	std::string scope_error;
	if (!uam::host_scoped_settings::ParseScope(m_app.settings, payload, scope, scope_error)) { cb->Failure(400, scope_error); return; }
	const nlohmann::json* requested_servers = uam::nlohmann_json::FindArrayField(payload, "servers");
	if (requested_servers == nullptr)
	{
		cb->Failure(400, "servers must be an array");
		return;
	}
	std::vector<McpServerConfiguration> servers = uam::mcp_server_config::Parse(*requested_servers);
	std::string error;
	if (!uam::mcp_server_config::NormalizeAndValidate(servers, &error))
	{
		cb->Failure(400, error);
		return;
	}

	if (!scope.empty() && std::ranges::any_of(servers, [&](const McpServerConfiguration& server) { return !server.execution_host_id.empty() && server.execution_host_id != scope; }))
	{
		cb->Failure(400, "MCP servers must belong to the selected computer.");
		return;
	}
	const AppSettings previous = m_app.settings;
	if (scope.empty()) m_app.settings.mcp_servers = std::move(servers);
	else m_app.settings.host_scoped_settings[scope].mcp_servers = std::move(servers);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, "Failed to save MCP server settings.");
		return;
	}
	cb->Success(nlohmann::json{{"servers", uam::mcp_server_config::Serialize(uam::host_scoped_settings::McpServers(m_app.settings, scope))}}.dump());
	uam::PushStateUpdateIfChanged(browser, m_app);
}

void UamQueryHandler::HandleSetUamAgentPreferences(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const nlohmann::json* favorite_ids = uam::nlohmann_json::FindArrayField(payload, "favoriteUamAgentIds");
	if (favorite_ids == nullptr || !payload.contains("uamAgentCycleShortcut") || !payload["uamAgentCycleShortcut"].is_string())
	{
		cb->Failure(400, "favoriteUamAgentIds must be an array and uamAgentCycleShortcut must be a string.");
		return;
	}
	if (!std::ranges::all_of(*favorite_ids, [](const nlohmann::json& item) { return item.is_string(); }))
	{
		cb->Failure(400, "Every favorite UAM agent id must be a string.");
		return;
	}

	AppSettings next = m_app.settings;
	next.favorite_uam_agent_ids = uam::nlohmann_json::TrimmedStringArrayField(payload, "favoriteUamAgentIds");
	next.uam_agent_cycle_shortcut = uam::strings::TrimAndLowerAscii(payload["uamAgentCycleShortcut"].get_ref<const std::string&>());
	if (!uam::settings::IsUamAgentCycleShortcut(next.uam_agent_cycle_shortcut))
	{
		cb->Failure(400, "Unsupported UAM agent cycle shortcut.");
		return;
	}
	uam::settings::NormalizeUamAgentPreferences(next);

	const AppSettings previous = m_app.settings;
	m_app.settings = std::move(next);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, "Failed to save UAM agent preferences.");
		return;
	}
	cb->Success(nlohmann::json{
	    {"favoriteUamAgentIds", m_app.settings.favorite_uam_agent_ids},
	    {"uamAgentCycleShortcut", m_app.settings.uam_agent_cycle_shortcut},
	}.dump());
	uam::PushStateUpdateIfChanged(browser, m_app);
}

void UamQueryHandler::HandleSetProviderChatDefaults(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string scope;
	std::string scope_error;
	if (!uam::host_scoped_settings::ParseScope(m_app.settings, payload, scope, scope_error)) { cb->Failure(400, scope_error); return; }
	if ((payload.contains("defaultProviderId") && !payload["defaultProviderId"].is_string()) ||
	    (payload.contains("defaults") && !payload["defaults"].is_object()))
	{
		cb->Failure(400, "Provider settings require a provider ID and defaults object.");
		return;
	}
	const AppSettings previous = m_app.settings;
	if (const nlohmann::json* entries = uam::nlohmann_json::FindObjectField(payload, "defaults"))
	{
		for (const auto& entry : entries->items()) for (const char* key : {"hiddenModelIds", "hiddenProviderIds"})
		{
			if (!entry.value().is_object() || !entry.value().contains(key)) continue;
			const auto& values = entry.value()[key];
			if (!values.is_array() || values.size() > 4096 || std::ranges::any_of(values, [](const nlohmann::json& id) { return !id.is_string() || id.get_ref<const std::string&>().size() > 1024; }))
			{
				cb->Failure(400, std::string(key) + " must contain at most 4096 model or provider IDs.");
				return;
			}
		}
	}
	const std::string requested_default_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(payload.value("defaultProviderId", uam::host_scoped_settings::DefaultProvider(m_app.settings, scope)));
	if (!requested_default_provider_id.empty())
	{
		const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, requested_default_provider_id);
		if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
		{
			cb->Failure(400, "Unsupported default provider: " + requested_default_provider_id);
			return;
		}
		if (payload.contains("defaultProviderId"))
		{
			if (scope.empty()) m_app.settings.default_new_chat_provider_id = provider->id;
			else m_app.settings.host_scoped_settings[scope].default_new_chat_provider_id = provider->id;
			// An explicit choice replaces a remembered excluded provider (see AppSettings::excluded_provider_selections).
			m_app.settings.excluded_provider_selections.erase(scope.empty() ? std::string("default_new_chat_provider_id") : "host:" + scope);
		}
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
			const ProviderChatDefaults fallback = uam::provider_chat_defaults::ForProvider(m_app.settings, provider->id, scope);
			ProviderChatDefaults defaults = ProviderDefaultsFromSettingsPayload(it.value(), fallback, provider->id);
			if (scope.empty()) m_app.settings.provider_chat_defaults[provider->id] = defaults;
			else m_app.settings.host_scoped_settings[scope].provider_chat_defaults[provider->id] = defaults;
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

void UamQueryHandler::HandleSetNativeImportFilters(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto is_string_array = [&payload](const char* key)
	{
		const auto field = payload.find(key);
		return field != payload.end() && field->is_array() && std::ranges::all_of(*field, [](const nlohmann::json& item) { return item.is_string(); });
	};
	if (!payload.is_object() || !is_string_array("titlePrefixes") || !is_string_array("directories"))
	{
		cb->Failure(400, "Import filters must be lists of text.");
		return;
	}

	const AppSettings previous = m_app.settings;
	m_app.settings.native_import_excluded_title_prefixes = uam::nlohmann_json::TrimmedStringArrayField(payload, "titlePrefixes");
	m_app.settings.native_import_excluded_directories = uam::nlohmann_json::TrimmedStringArrayField(payload, "directories");
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to save the import filters."));
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
	m_app.settings.automatic_provider_updates = payload.value("automaticProviderUpdates", m_app.settings.automatic_provider_updates);
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
	if (payload.contains("fileExplorerApplication"))
	{
		if (!payload["fileExplorerApplication"].is_string())
		{
			cb->Failure(400, "File explorer application must be a path.");
			return;
		}
		const std::string application = payload["fileExplorerApplication"].get<std::string>();
#if defined(_WIN32)
		constexpr bool windows = true;
#else
		constexpr bool windows = false;
#endif
		if (!uam::platform::IsFileExplorerApplication(application, windows))
		{
			cb->Failure(400, "Choose an existing file explorer application using its full path.");
			return;
		}
		m_app.settings.file_explorer_application = application;
	}
	if (payload.contains("windowsPowerShell"))
	{
		if (!payload["windowsPowerShell"].is_string() || !uam::powershell::IsPreference(payload["windowsPowerShell"].get_ref<const std::string&>()))
		{
			cb->Failure(400, "PowerShell must be auto, windows-powershell or pwsh.");
			return;
		}
		m_app.settings.windows_powershell = payload["windowsPowerShell"].get<std::string>();
	}
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
	for (const char* field : {"providerId", "executionHostId"})
	{
		if (payload.contains(field) && (!payload[field].is_string() || uam::strings::Trim(payload[field].get_ref<const std::string&>()).empty()))
		{
			cb->Failure(400, std::string(field) + " must be a nonempty string.");
			return;
		}
	}
	const std::string execution_host_id = uam::nlohmann_json::TrimmedStringValue(payload, {"executionHostId"});

	const std::string provider_id = CliVersionProviderFromPayloadOrSelection(m_app, payload);
	if (ProviderProfileStore::FindById(m_app.provider_profiles, provider_id) == nullptr)
	{
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	}

	std::string error;
	if (!ProviderCliCompatibilityService().StartProviderVersionCheck(m_app, provider_id, true, execution_host_id, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to start provider CLI check."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRefreshAllCliProviderVersions(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	ProviderCliCompatibilityService().StartVersionCheck(m_app, true, true);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleApplyCliProviderVersion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	for (const char* field : {"providerId", "executionHostId"})
	{
		if (payload.contains(field) && (!payload[field].is_string() || uam::strings::Trim(payload[field].get_ref<const std::string&>()).empty()))
		{
			cb->Failure(400, std::string(field) + " must be a nonempty string.");
			return;
		}
	}
	const std::string execution_host_id = uam::nlohmann_json::TrimmedStringValue(payload, {"executionHostId"});

	const std::string provider_id = CliVersionProviderFromPayloadOrSelection(m_app, payload);
	if (provider_id.empty() || ProviderProfileStore::FindById(m_app.provider_profiles, provider_id) == nullptr)
	{
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	}

	const std::string version = uam::strings::Trim(payload.value("version", ""));
	std::string error;
	if (!ProviderCliCompatibilityService().StartInstallProviderVersion(m_app, provider_id, version, &error, execution_host_id, payload.value("stopSessions", false), false, payload.value("allowInteractiveElevation", false)))
	{
		m_app.status_line = FailureDetailOrFallback(error, "Failed to start provider CLI install.");
		uam::PushStateUpdateIfChanged(browser, m_app);
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

bool UamQueryHandler::CompanionAllowsKeychainPrompts() const
{
	nlohmann::json config;
	return ReadCompanionConfig(m_app, config) && KeychainPromptsAllowed(config);
}

nlohmann::json UamQueryHandler::CompanionConnectionStatus(const nlohmann::json& config) const
{
	auto response = CompanionTransportStatus(config);
#if defined(__APPLE__)
	// Only macOS has the Keychain dialog this Phone setting controls.
	response["allowKeychainPrompts"] = KeychainPromptsAllowed(config);
#endif
	return response;
}

nlohmann::json UamQueryHandler::CompanionTransportStatus(const nlohmann::json& config) const
{
#if UAM_ENABLE_MOBILE_COMPANION
	if (m_companion) return m_companion->Status();
	return UamCompanionService::ConfigurationStatus(m_app.data_root);
#endif
	const auto saved = CompanionSettingsResponse(config);
	const bool enabled = saved["enabled"].get<bool>();
	const bool configured = saved["configured"].get<bool>();
	auto response = uam::companion::ConnectionStatus(
	    {configured, enabled, configured ? saved["url"].get<std::string>() : std::string{}, 0}, enabled ? "failed" : "disabled", enabled ? "configurationInvalid" : "disabled");
#if defined(__APPLE__)
	response["localAddresses"] = uam::platform::MacLocalIpv4Addresses();
#endif
	return response;
}

void UamQueryHandler::ApplyCompanionConfiguration(CefRefPtr<CefBrowser> browser, const nlohmann::json& config)
{
#if UAM_ENABLE_MOBILE_COMPANION
	if (!m_companion) StartCompanion(browser);
	const auto saved = CompanionSettingsResponse(config);
	if (m_companion) m_companion->Reload(saved["enabled"].get<bool>());
#else
	(void)browser;
	(void)config;
#endif
}

#if UAM_ENABLE_MOBILE_COMPANION
void UamQueryHandler::HandleRestartCompanion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.is_object() || !payload.empty())
	{ cb->Failure(400, "Restart phone access does not accept configuration changes."); return; }
	nlohmann::json config;
	if (!ReadCompanionConfig(m_app, config) || !config.contains("token_file") || !config["token_file"].is_string())
	{ cb->Failure(409, "Phone access is not configured in Universal Agent Manager."); return; }
	ApplyCompanionConfiguration(browser, config);
	cb->Success(CompanionConnectionStatus(config).dump());
}

void UamQueryHandler::HandleGetCompanionSettings(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	nlohmann::json config = nlohmann::json::object();
	(void)ReadCompanionConfig(m_app, config);
	cb->Success(CompanionConnectionStatus(config).dump());
}

void UamQueryHandler::HandleSetCompanionEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled)
	{
		cb->Failure(400, "enabled must be a boolean.");
		return;
	}

	nlohmann::json config;
	if (!ReadCompanionConfig(m_app, config) || !config.contains("token_file") || !config["token_file"].is_string())
	{
		cb->Failure(409, "Phone access is not configured.");
		return;
	}
	config["enabled"] = *enabled;
	if (!WriteCompanionConfig(m_app, config))
	{
		cb->Failure(500, "Failed to persist phone access settings.");
		return;
	}
#if defined(__APPLE__)
	if (*enabled && config.contains("origin") && config["origin"].is_string() &&
	    !config["origin"].get_ref<const std::string&>().empty())
		uam::platform::RequestMacLocalNetworkAccess();
#endif
	ApplyCompanionConfiguration(browser, config);
	cb->Success(CompanionConnectionStatus(config).dump());
}

void UamQueryHandler::HandleSetCompanionKeychainPrompts(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<bool> allowed = uam::nlohmann_json::BoolFieldStrict(payload, "allowed");
	if (!allowed) { cb->Failure(400, "allowed must be a boolean."); return; }
	nlohmann::json config;
	if (!ReadCompanionConfig(m_app, config) || !config.contains("token_file") || !config["token_file"].is_string()) { cb->Failure(409, "Phone access is not configured."); return; }
	config["allow_keychain_prompts"] = *allowed;
	if (!WriteCompanionConfig(m_app, config)) { cb->Failure(500, "Failed to persist phone access settings."); return; }
	cb->Success(CompanionConnectionStatus(config).dump());
}

void UamQueryHandler::HandleGetCompanionToken(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	nlohmann::json config;
	if (!ReadCompanionConfig(m_app, config) || !config.contains("token_file") || !config["token_file"].is_string())
	{
		cb->Failure(409, "Phone access is not configured.");
		return;
	}
	std::string token;
	if (!uam::io::TryReadTextFile(config["token_file"].get<std::string>(), token))
	{
		cb->Failure(404, "Phone access token is unavailable.");
		return;
	}
	token = uam::strings::Trim(token);
	if (token.size() != 64 || token.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
	{
		cb->Failure(404, "Phone access token is invalid or unavailable.");
		return;
	}
	const auto status = CompanionConnectionStatus(config);
	nlohmann::json response{{"token", token}, {"url", status.value("state", std::string{}) == "ready" ? status.value("effectiveUrl", std::string{}) : std::string{}},
	    {"state", status["state"]}, {"reasonCode", status["reasonCode"]}, {"reason", status["reason"]}};
	// The public CA is optional and is never a reason to expose an unready saved URL.
	try
	{
		const std::string certificate = uam::companion::ReadCompanionPairingCertificate(config);
		if (!certificate.empty()) response["certificate"] = certificate;
	}
	catch (...) {}
	cb->Success(response.dump());
}
#endif

void UamQueryHandler::HandleApplyCliProviderVersions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.contains("targets") || !payload["targets"].is_array() || payload["targets"].empty() || payload["targets"].size() > 64)
	{
		cb->Failure(400, "Choose between 1 and 64 provider installations to update.");
		return;
	}
	std::vector<std::tuple<std::string, std::string, std::string>> targets;
	std::unordered_set<std::string> seen;
	for (const auto& target : payload["targets"])
	{
		if (!target.is_object() || !target.contains("providerId") || !target["providerId"].is_string() ||
		    !target.contains("version") || !target["version"].is_string() ||
		    (target.contains("executionHostId") && !target["executionHostId"].is_string()))
		{
			cb->Failure(400, "Each update requires a provider, version and valid machine ID.");
			return;
		}
		const std::string provider = uam::provider_ids::CanonicalCliProviderLookupId(target["providerId"].get<std::string>());
		const std::string host = target.value("executionHostId", "local");
		if (uam::execution_hosts::Find(m_app.settings.execution_hosts, host) == nullptr)
		{
			cb->Failure(400, "The update list references a machine that no longer exists.");
			return;
		}
		if (!seen.insert(CliProviderVersionStateKey(provider, host)).second ||
		    !ProviderProfileStore::FindById(m_app.provider_profiles, provider) ||
		    !ProviderCliCompatibilityService().IsSupportedVersionForProvider(provider, target["version"].get<std::string>()))
		{
			cb->Failure(400, "The update list contains duplicate or unsupported provider versions.");
			return;
		}
		targets.emplace_back(provider, target["version"].get<std::string>(), host);
	}
	ProviderCliCompatibilityService().StartInstallProviderVersions(m_app, targets, payload.value("allowInteractiveElevation", false));
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleGetAppPermissions(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
#if defined(__APPLE__)
	const uam::platform::MacPrivacyPermissions permissions = uam::platform::GetMacPrivacyPermissions();
	cb->Success(nlohmann::json{{"platform", "macos"}, {"microphone", permissions.microphone},
	    {"speechRecognition", permissions.speech_recognition}, {"localNetwork", "system_managed"}}.dump());
#else
	cb->Success(nlohmann::json{{"platform", "other"}, {"microphone", "system_managed"},
	    {"speechRecognition", "unsupported"}, {"localNetwork", "system_managed"}}.dump());
#endif
}

void UamQueryHandler::HandleRequestAppPermission(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string permission = uam::nlohmann_json::TrimmedStringValue(payload, {"permission"});
#if defined(__APPLE__)
	if (!uam::platform::RequestMacPrivacyPermission(permission)) { cb->Failure(400, "Unknown permission."); return; }
	cb->Success("{}");
#else
	(void)permission;
	cb->Failure(409, "Manage this permission in your operating system settings.");
#endif
}

void UamQueryHandler::HandleOpenAppPermissionSettings(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string permission = uam::nlohmann_json::TrimmedStringValue(payload, {"permission"});
#if defined(__APPLE__)
	std::string pane;
	if (permission == "localNetwork") pane = "Privacy_LocalNetwork";
	else if (permission == "microphone") pane = "Privacy_Microphone";
	else if (permission == "speechRecognition") pane = "Privacy_SpeechRecognition";
	else { cb->Failure(400, "Unknown permission."); return; }
	std::string error;
	if (!uam::platform::OpenExternalUrl("x-apple.systempreferences:com.apple.preference.security?" + pane, &error))
	{ cb->Failure(500, error); return; }
	cb->Success("{}");
#else
	(void)permission;
	cb->Failure(409, "Open your operating system privacy settings.");
#endif
}

#if UAM_ENABLE_MOBILE_COMPANION
void UamQueryHandler::HandleRepairCompanionLanAddress(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
#if defined(__APPLE__)
	const std::string address = uam::nlohmann_json::TrimmedStringValue(payload, {"address"});
	const std::vector<std::string> addresses = uam::platform::MacLocalIpv4Addresses();
	if (std::ranges::find(addresses, address) == addresses.end())
	{ cb->Failure(409, "That network address is no longer available. Refresh and try again."); return; }
	nlohmann::json config;
	if (!ReadCompanionConfig(m_app, config) || !config.contains("origin") || !config["origin"].is_string() ||
	    !config.contains("proxy_config") || !config["proxy_config"].is_string())
	{ cb->Failure(409, "Phone HTTPS configuration is unavailable."); return; }
	std::string proxy_text;
	const std::filesystem::path proxy_path = config["proxy_config"].get<std::string>();
	if (!uam::io::TryReadTextFile(proxy_path, proxy_text)) { cb->Failure(500, "Cannot read phone HTTPS configuration."); return; }
	nlohmann::json proxy = nlohmann::json::parse(proxy_text, nullptr, false);
	if (!uam::companion::RepairLanAddress(config, proxy, address, addresses))
	{ cb->Failure(409, "This phone HTTPS configuration requires manual network configuration."); return; }
	if (!uam::io::WriteTextFileWithBackup(proxy_path, proxy.dump(2) + "\n"))
	{ cb->Failure(500, "Cannot save phone HTTPS configuration."); return; }
	if (!WriteCompanionConfig(m_app, config))
	{
		(void)uam::io::WriteTextFileWithBackup(proxy_path, proxy_text);
		cb->Failure(500, "Cannot save phone URL. The proxy configuration was restored."); return;
	}
	uam::platform::RequestMacLocalNetworkAccess();
	ApplyCompanionConfiguration(browser, config);
	cb->Success(CompanionConnectionStatus(config).dump());
#else
	(void)payload;
	cb->Failure(409, "Automatic LAN repair is currently available on macOS.");
#endif
}
#endif

void UamQueryHandler::HandleReadCentralInstructionFile(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	if (!payload.contains("path") || !payload["path"].is_string())
	{ cb->Failure(400, "Choose a configured instruction file."); return; }
	const std::string path = payload["path"].get<std::string>();
	const CentralProviderConfiguration configuration = m_app.settings.central_provider_configuration;
	if (!uam::central_instruction_files::Authorized(configuration, path))
	{ cb->Failure(403, "Save this instruction path in Shared setup before editing it."); return; }
	if (m_pendingCentralInstructionFiles >= 4)
	{ cb->Failure(429, "Instruction files are busy. Try again shortly."); return; }
	if (!m_instructionQueries) m_instructionQueries = CefThread::CreateThread("uam-instruction-files");
	if (!m_instructionQueries) { cb->Failure(503, "The instruction file worker is unavailable."); return; }
	++m_pendingCentralInstructionFiles;
	if (!m_instructionQueries->GetTaskRunner()->PostTask(new CefQueryWorkerTask(m_asyncLifetime, cb,
	    [configuration, path]()
	    {
		    const uam::central_instruction_files::TextFile file = uam::central_instruction_files::Read(configuration, path);
		    return file.ok ? AsyncSuccess({{"path", file.path}, {"content", file.content}}) : AsyncFailure(file.status, file.error);
	    },
	    [this, path](AsyncCefResult& result)
	    {
		    --m_pendingCentralInstructionFiles;
		    if (result.ok && !uam::central_instruction_files::Authorized(m_app.settings.central_provider_configuration, path))
			    result = AsyncFailure(409, "The configured instruction paths changed. Save Shared setup and reopen the file.");
	    })))
	{
		--m_pendingCentralInstructionFiles;
		cb->Failure(503, "The instruction file worker is unavailable.");
	}
}

void UamQueryHandler::HandleSaveCentralInstructionFile(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	for (const char* key : {"path", "content", "expectedContent"})
		if (!payload.contains(key) || !payload[key].is_string())
		{ cb->Failure(400, "Instruction path, text and original text are required."); return; }
	const std::string path = payload["path"].get<std::string>();
	const std::string content = payload["content"].get<std::string>();
	const std::string expected_content = payload["expectedContent"].get<std::string>();
	const CentralProviderConfiguration configuration = m_app.settings.central_provider_configuration;
	if (!uam::central_instruction_files::Authorized(configuration, path))
	{ cb->Failure(403, "Save this instruction path in Shared setup before editing it."); return; }
	if (content.size() > uam::provider_setup::kMaxFileBytes || expected_content.size() > uam::provider_setup::kMaxFileBytes)
	{ cb->Failure(400, "Instruction files are limited to 2 MiB."); return; }
	if (m_pendingCentralInstructionFiles >= 4)
	{ cb->Failure(429, "Instruction files are busy. Try again shortly."); return; }
	const std::shared_ptr<std::shared_ptr<uam::central_instruction_files::PreparedSave>> prepared =
	    std::make_shared<std::shared_ptr<uam::central_instruction_files::PreparedSave>>();
	if (!m_instructionQueries) m_instructionQueries = CefThread::CreateThread("uam-instruction-files");
	if (!m_instructionQueries) { cb->Failure(503, "The instruction file worker is unavailable."); return; }
	++m_pendingCentralInstructionFiles;
	if (!m_instructionQueries->GetTaskRunner()->PostTask(new CefQueryWorkerTask(m_asyncLifetime, cb,
	    [configuration, path, content, expected_content, prepared]()
	    {
		    *prepared = uam::central_instruction_files::PrepareSave(configuration, path, content, expected_content);
		    const uam::central_instruction_files::TextFile& file = (*prepared)->result;
		    return file.ok ? AsyncSuccess(nlohmann::json::object()) : AsyncFailure(file.status, file.error);
	    },
	    [this, path, prepared](AsyncCefResult& result)
	    {
		    --m_pendingCentralInstructionFiles;
		    if (!result.ok) return;
		    if (!uam::central_instruction_files::Authorized(m_app.settings.central_provider_configuration, path))
		    {
			    result = AsyncFailure(409, "The configured instruction paths changed. Save Shared setup and reopen the file.");
			    return;
		    }
		    // Final bounded reread, atomic replacement and directory sync stay in this settings-owning UI turn.
		    const uam::central_instruction_files::TextFile file = uam::central_instruction_files::CommitSave(m_app.settings.central_provider_configuration, **prepared);
		    result = file.ok ? AsyncSuccess({{"path", file.path}, {"content", file.content}}) : AsyncFailure(file.status, file.error);
	    })))
	{
		--m_pendingCentralInstructionFiles;
		cb->Failure(503, "The instruction file worker is unavailable.");
	}
}

void UamQueryHandler::HandleSetCentralProviderConfiguration(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	CentralProviderConfiguration configuration;
	std::string error;
	if (!uam::central_configuration::Parse(payload, configuration, error))
	{
		cb->Failure(400, error);
		return;
	}
	const CentralProviderConfiguration previous = m_app.settings.central_provider_configuration;
	uam::central_configuration::KeepExcludedProviderOverrides(previous, configuration);
	m_app.settings.central_provider_configuration = std::move(configuration);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings.central_provider_configuration = previous;
		cb->Failure(500, "Could not save central provider configuration.");
		return;
	}
	cb->Success("{}");
	uam::PushStateUpdateIfChanged(browser, m_app);
}

#if UAM_ENABLE_SSH
void UamQueryHandler::HandleSetExecutionRoutingSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	nlohmann::json confirmed_payload = payload;
	try
	{
		if (confirmed_payload.contains("hosts") && confirmed_payload["hosts"].is_array())
			for (nlohmann::json& host_entry : confirmed_payload["hosts"])
			{
				const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, host_entry.at("hostId").get<std::string>());
				if (host == nullptr || host->id == "local") throw std::runtime_error("Select an existing remote host.");
				for (const nlohmann::json& mapping : host_entry.at("workspaceMappings"))
				{
					const ChatFolder* source = ChatDomainService().FindFolderById(m_app, mapping.at("sourceFolderId").get<std::string>());
					if (!source || source->directory != mapping.at("sourceDirectory").get<std::string>()) throw std::runtime_error("Choose the current registered source workspace.");
				}
				for (nlohmann::json& provider_entry : host_entry.at("providers"))
					if (provider_entry.value("confirm", false))
					{
						const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, provider_entry.at("providerId").get<std::string>());
						if (provider == nullptr) throw std::runtime_error("Select an available provider.");
						provider_entry["connectionIdentity"] = uam::ExecutionRoutingConnectionIdentity(*host);
						provider_entry["providerConfigurationIdentity"] = uam::ExecutionRoutingProviderIdentity(m_app, *provider, host->id);
					}
			}
	}
	catch (const std::exception& exception)
	{
		cb->Failure(400, exception.what());
		return;
	}
	ExecutionRoutingSettings settings;
	std::string error;
	if (!SettingsStore::ParseExecutionRoutingSettings(confirmed_payload, settings, error))
	{
		cb->Failure(400, error);
		return;
	}
	for (const ExecutionRoutingHostPolicy& policy : settings.hosts)
		for (const ExecutionRoutingWorkspaceMapping& mapping : policy.workspace_mappings)
		{
			const ChatFolder* source = ChatDomainService().FindFolderById(m_app, mapping.source_folder_id);
			const ChatFolder* destination = ChatDomainService().FindFolderById(m_app, mapping.destination_folder_id);
			if (source == nullptr || destination == nullptr || destination->execution_host_id != policy.host_id)
			{
				cb->Failure(400, "Routing mappings require existing source and destination workspaces on the selected host.");
				return;
			}
		}
	const ExecutionRoutingSettings previous = m_app.settings.execution_routing;
	m_app.settings.execution_routing = std::move(settings);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings.execution_routing = previous;
		cb->Failure(500, "Routing settings could not be saved.");
		return;
	}
	m_app.execution_routing_retry_at_by_chat_id.clear();
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleReleaseExecutionRoutingReservation(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	uam::ExecutionRoutingAssignment expected;
	bool acknowledged = false;
	try
	{
		expected.chat_id = payload.at("chatId").get<std::string>();
		expected.host_id = payload.at("executionHostId").get<std::string>();
		expected.connection_identity = payload.at("connectionIdentity").get<std::string>();
		expected.provider_configuration_identity = payload.at("providerConfigurationIdentity").get<std::string>();
		expected.destination_folder_id = payload.at("destinationFolderId").get<std::string>();
		expected.destination_directory = payload.at("destinationDirectory").get<std::string>();
		expected.automatic = true;
		acknowledged = payload.at("acknowledgeUnresolvedOwnership").get<bool>();
	}
	catch (const std::exception&)
	{
		cb->Failure(400, "Release requires the current chat assignment.");
		return;
	}
	std::string error;
	if (!uam::ReleaseUnresolvedExecutionRouting(m_app, expected, acknowledged, error))
	{
		cb->Failure(409, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
#endif

void UamQueryHandler::HandleSetProviderMcpConfiguration(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	try
	{
		const std::string host = payload.at("executionHostId").get<std::string>();
		const std::string provider = payload.at("providerId").get<std::string>();
		const std::string server = payload.at("serverId").get<std::string>();
		const bool enabled = payload.at("enabled").get<bool>();
		const std::string kind = payload.value("kind", std::string("server"));
		if (kind != "server" && kind != "preference") { cb->Failure(400, "Unknown provider MCP setting kind."); return; }
		std::string error;
		const int failure = uam::provider_mcp_settings::SetConfiguration(m_app.settings, host, provider, server, enabled, kind == "preference",
		    [&]() { return PersistenceCoordinator().SaveSettings(m_app); }, error);
		if (failure != 0) { cb->Failure(failure, error); return; }
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(uam::provider_mcp_settings::Status(m_app, host, "").dump());
	}
	catch (const std::exception&) { cb->Failure(400, "Select a provider, host, server and an enabled boolean."); }
}

void UamQueryHandler::HandleGetProviderMcpStatus(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	(void)browser;
	try
	{
		const std::string host_id = payload.value("executionHostId", std::string("local"));
		if (uam::execution_hosts::Find(m_app.settings.execution_hosts, host_id) == nullptr)
		{
			cb->Failure(404, "Execution host not found."); return;
		}
		cb->Success(uam::provider_mcp_settings::Status(m_app, host_id,
		    payload.value("workspaceDirectory", std::string{}), payload.value("chatId", std::string{})).dump());
	}
	catch (const std::exception&)
	{
		cb->Failure(400, "MCP status requires a valid host and optional chat and workspace strings.");
	}
}

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
void UamQueryHandler::HandlePreviewOpenCodeMcpImport(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	(void)browser;
	try
	{
		const std::string host = payload.value("executionHostId", std::string("local"));
		if (host != "local")
		{
			cb->Failure(400, "OpenCode file import is available on this computer. For an SSH host, paste its MCP JSON and use executable paths from that host."); return;
		}
		const std::string workspace = payload.value("workspaceDirectory", std::string{});
		if (!workspace.empty() && !uam::paths::PathFromUtf8(workspace).is_absolute())
		{
			cb->Failure(400, "Select an absolute workspace path or use global scope."); return;
		}
		const std::string source = payload.value("sourcePath", std::string{});
		// Snapshot environment on the GUI thread. The worker never reads AppState.
		const std::string inline_config = uam::env::GetNonEmptyString("OPENCODE_CONFIG_CONTENT").value_or("");
		const std::string override_path = uam::env::GetNonEmptyString("OPENCODE_CONFIG").value_or("");
		const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath();
		const std::filesystem::path config_root = uam::env::GetTrimmedPath("XDG_CONFIG_HOME").value_or(home ? *home / ".config" : std::filesystem::path{});
		const std::vector<std::filesystem::path> search = uam::provider_mcp_settings::CommandSearchPaths();
		(void)uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
		    [source, workspace, inline_config, override_path, config_root, search]()
		    {
			try
			{
				return uam::query_handler_async::AsyncSuccess(uam::provider_mcp_settings::ReadImport(
				    source, workspace, inline_config, override_path, config_root, search));
			}
			catch (const std::exception& error)
			{
				return uam::query_handler_async::AsyncFailure(400, error.what());
			}
		    });
	}
	catch (const std::exception&)
	{
		cb->Failure(400, "OpenCode import requires optional file and workspace path strings.");
	}
}
#endif

namespace
{
	nlohmann::json DataFolderSnapshot(const uam::DataFolderSettingsService::Snapshot& value)
	{
		return {{"currentFolder", uam::paths::Utf8PathString(value.current_folder)}, {"pendingFolder", uam::paths::Utf8PathString(value.pending_folder)}, {"revision", value.revision}, {"lastOperationId", value.last_operation_id}, {"transitionState", value.state}, {"externalOverride", value.external_override}, {"canChange", value.can_change}, {"restartReady", value.restart_ready}, {"message", value.message}};
	}
} // namespace

void UamQueryHandler::HandleDataFolderSettings(std::string_view action, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	// Called only from trusted-main-frame OnQuery, never from the companion dispatcher.
	const auto service = m_app.data_folder_settings;
	if (!service || service->StartupRoot() != m_app.data_root)
	{
		cb->Failure(409, "The data-folder owner is unavailable or changed. Restart UAM before changing its profile.");
		return;
	}
	const bool read = action == "getDataFolderSettings";
	const bool choose = action == "chooseDataFolder";
	const bool rollback = action == "rollbackDataFolderTransfer";
	if (rollback && !uam::DataFolderSettingsService::kTransfersValidated)
	{
		cb->Failure(409, "Data-folder recovery controls remain disabled pending native transaction validation.");
		return;
	}
	if (!payload.is_object() || (read ? !payload.empty() : payload.size() != (choose ? 3 : 2) || !payload.contains("expectedRevision") || !payload["expectedRevision"].is_string() || payload["expectedRevision"].get_ref<const std::string&>().size() > 300 || !payload.contains("operationId") || !payload["operationId"].is_string() || payload["operationId"].get_ref<const std::string&>().empty() || payload["operationId"].get_ref<const std::string&>().size() > 128 || (choose && (!payload.contains("folder") || !payload["folder"].is_string() || payload["folder"].get_ref<const std::string&>().size() > 32768))))
	{
		cb->Failure(400, "Folder changes require the observed revision and an action identity; selection also requires a folder string.");
		return;
	}
	const std::string revision = read ? std::string{} : payload["expectedRevision"].get<std::string>();
	const std::string folder = choose ? payload["folder"].get<std::string>() : std::string{};
	const std::string operation_id = read ? std::string{} : payload["operationId"].get<std::string>();
	// Read mutable runtime state only on the UI thread. Readiness is an observation;
	// the restart action and final shutdown barrier independently recheck it.
	const bool runtime_dormant = std::none_of(m_app.acp_sessions.begin(), m_app.acp_sessions.end(), [](const auto& session) { return session && session->running; }) &&
	    std::none_of(m_app.cli_terminals.begin(), m_app.cli_terminals.end(), [](const auto& terminal) { return terminal && terminal->running; });
	(void)uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
	                                                 [service, read, choose, rollback, revision, folder, operation_id, runtime_dormant]()
	                                                 {
		                                                 const auto snapshot = [runtime_dormant](uam::DataFolderSettingsService::Snapshot value)
		                                                 {
			                                                 value.restart_ready = value.restart_ready && runtime_dormant;
			                                                 return DataFolderSnapshot(value);
		                                                 };
		                                                 if (read)
			                                                 return uam::query_handler_async::AsyncSuccess(snapshot(service->Read()));
		                                                 const auto result = choose ? service->Choose(uam::paths::PathFromUtf8(folder), revision, operation_id) : rollback ? service->RollbackPendingTransfer(revision) : service->Cancel(revision, operation_id);
		                                                 // Persistence rejection and uncertainty carry authoritative state, never an empty success body.
		                                                 return uam::query_handler_async::AsyncSuccess(nlohmann::json{{"committed", result.committed}, {"settings", snapshot(result.settings)}, {"error", result.error}});
	                                                 });
}

void UamQueryHandler::HandleSetStorageSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<int> logs = uam::nlohmann_json::IntFieldStrict(payload, "backupLogRetentionDays");
	const std::optional<int> cache = uam::nlohmann_json::IntFieldStrict(payload, "httpCacheClearIntervalDays");
	if ((!payload.contains("backupLogRetentionDays") && !payload.contains("httpCacheClearIntervalDays")) ||
	    (payload.contains("backupLogRetentionDays") && (!logs || *logs < 0 || *logs > 3650)) ||
	    (payload.contains("httpCacheClearIntervalDays") && (!cache || *cache < 0 || *cache > 3650)))
	{ cb->Failure(400, "Storage policy requires integer day values from 0 to 3650; zero disables cleanup."); return; }
	if (!m_app.storage_retention_service)
	{ cb->Failure(409, "Storage policy is still loading."); return; }
	// Adopt a just-published policy before resolving omitted fields from current values.
	m_app.storage_retention_service->Poll(m_app);
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	if (!m_app.storage_retention_service->RequestPolicy(m_app.data_root,
	    {logs.value_or(m_app.settings.backup_log_retention_days), cache.value_or(m_app.settings.http_cache_clear_interval_days)}, lifetime,
	    [this, browser, cb, lifetime](bool saved, const std::string& error)
	    {
		    if (lifetime.expired()) return;
		    if (!saved) { cb->Failure(500, error); return; }
		    uam::PushStateUpdateIfChanged(browser, m_app);
		    cb->Success("{}");
	    })) cb->Failure(409, "Storage policy publication is busy or still loading. Retry shortly.");
}

void UamQueryHandler::HandleSetVcsSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<bool> git_enabled = uam::nlohmann_json::BoolFieldStrict(payload, "gitEnabled");
	const std::optional<bool> svn_enabled = uam::nlohmann_json::BoolFieldStrict(payload, "svnEnabled");
	if (!git_enabled || !svn_enabled) { cb->Failure(400, "VCS settings must be booleans."); return; }
	const std::optional<bool> pull_before_chat = uam::nlohmann_json::BoolFieldStrict(payload, "gitPullBeforeChat");
	if (payload.contains("gitPullBeforeChat") && !pull_before_chat)
	{
		cb->Failure(400, "Pull before chat requires a boolean."); return;
	}
	const std::optional<bool> svn_update_before_chat = uam::nlohmann_json::BoolFieldStrict(payload, "svnUpdateBeforeChat");
	if (payload.contains("svnUpdateBeforeChat") && !svn_update_before_chat)
	{
		cb->Failure(400, "SVN update before chat requires a boolean."); return;
	}
	const nlohmann::json* port_value = uam::nlohmann_json::FindStringField(payload, "gitPortMethod");
	const std::string port_method = port_value ? port_value->get<std::string>() : std::string();
	if (payload.contains("gitPortMethod") && (!port_value || (port_method != "patch" && port_method != "merge" && port_method != "squash")))
	{
		cb->Failure(400, "Git port method must be patch, merge or squash."); return;
	}
	const std::optional<bool> snapshot_svn = uam::nlohmann_json::BoolFieldStrict(payload, "gitSnapshotSvn");
	const std::optional<bool> snapshot_plain_folders = uam::nlohmann_json::BoolFieldStrict(payload, "gitSnapshotPlainFolders");
	if ((payload.contains("gitSnapshotSvn") && !snapshot_svn) ||
	    (payload.contains("gitSnapshotPlainFolders") && !snapshot_plain_folders))
	{
		cb->Failure(400, "Managed Git snapshot settings must be booleans."); return;
	}
	const std::optional<bool> automatic_worktree = uam::nlohmann_json::BoolFieldStrict(payload, "gitAutoWorktree");
	const nlohmann::json* base_value = uam::nlohmann_json::FindStringField(payload, "gitDefaultBaseBranch");
	const std::string base_branch = base_value == nullptr ? std::string() : uam::strings::Trim(base_value->get<std::string>());
	if ((payload.contains("gitAutoWorktree") && !automatic_worktree) ||
	    (payload.contains("gitDefaultBaseBranch") && (!base_value || base_branch.size() > 255 ||
	    std::ranges::any_of(base_branch, [](unsigned char c) { return c < 32 || c == 127; }))))
	{
		cb->Failure(400, "Automatic worktrees require a boolean and the default Git branch requires a short branch name.");
		return;
	}
	const bool has_cleanup = payload.contains("worktreeCleanupEnabled");
	const bool has_days = payload.contains("worktreeRetentionDays");
	const std::optional<bool> cleanup_enabled = uam::nlohmann_json::BoolFieldStrict(payload, "worktreeCleanupEnabled");
	const std::optional<int> retention_days = uam::nlohmann_json::IntFieldStrict(payload, "worktreeRetentionDays");
	if ((has_cleanup && !cleanup_enabled) || (has_days && (!retention_days || *retention_days < 1 || *retention_days > 3650)))
	{
		cb->Failure(400, "Worktree cleanup requires a boolean and retention from 1 to 3650 days.");
		return;
	}
	// Accepted policy changes cannot race an exclusive background removal.
	const bool policy_changed = m_app.settings.git_enabled != *git_enabled ||
	    (cleanup_enabled && m_app.settings.worktree_cleanup_enabled != *cleanup_enabled) ||
	    (retention_days && m_app.settings.worktree_retention_days != *retention_days);
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease;
	if (policy_changed)
	{
		lease = m_app.worktree_retention_gate.TryAcquireMutation();
		if (!lease) { cb->Failure(409, "Worktree cleanup is finishing. Retry the settings change."); return; }
	}
	const AppSettings previous = m_app.settings;
	m_app.settings.git_enabled = *git_enabled;
	m_app.settings.svn_enabled = *svn_enabled;
	if (automatic_worktree) m_app.settings.git_auto_worktree = *automatic_worktree;
	if (snapshot_svn) m_app.settings.git_snapshot_svn = *snapshot_svn;
	if (snapshot_plain_folders) m_app.settings.git_snapshot_plain_folders = *snapshot_plain_folders;
	if (pull_before_chat) m_app.settings.git_pull_before_chat = *pull_before_chat;
	if (svn_update_before_chat) m_app.settings.svn_update_before_chat = *svn_update_before_chat;
	if (port_value) m_app.settings.git_port_method = port_method == "merge" ? GitPortMethod::Merge : port_method == "squash" ? GitPortMethod::Squash : GitPortMethod::Patch;
	if (base_value) m_app.settings.git_default_base_branch = base_branch;
	if (cleanup_enabled) m_app.settings.worktree_cleanup_enabled = *cleanup_enabled;
	if (retention_days) m_app.settings.worktree_retention_days = *retention_days;
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Could not save VCS settings."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetProviderAvailability(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled || !payload.contains("providerId") || !payload["providerId"].is_string() ||
	    !payload.contains("executionHostId") || !payload["executionHostId"].is_string())
	{
		cb->Failure(400, "Provider availability requires a provider, host and enabled boolean.");
		return;
	}
	const std::string provider_id = payload["providerId"].get<std::string>();
	const std::string host_id = uam::provider_availability::HostId(payload["executionHostId"].get<std::string>());
	const AppSettings previous = m_app.settings;
	if (uam::execution_hosts::Find(m_app.settings.execution_hosts, host_id) == nullptr ||
	    !uam::provider_availability::Set(m_app.settings, provider_id, host_id, *enabled))
	{
		cb->Failure(400, "Provider or execution host is unavailable.");
		return;
	}
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Could not save provider availability."));
		return;
	}
	if (*enabled) ProviderCliCompatibilityService().CheckReenabledProvider(m_app, provider_id, host_id);
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
