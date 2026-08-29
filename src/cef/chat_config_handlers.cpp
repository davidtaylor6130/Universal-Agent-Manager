#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/agent_definition_service.h"
#include "app/agent_run_scheduler.h"
#include "app/chat_lifecycle_service.h"
#include "app/computer_use_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/uam_control_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "computer_use/computer_use_mcp_config.h"
#include "computer_use/computer_use_platform.h"
#include "common/memory/memory_levels.h"
#include "common/paths/workspace_root.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/security/command_safety.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// Chat configuration handlers (model, provider, approval, memory)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

void UamQueryHandler::HandleOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string source_chat_id = payload.value("chatId", "");
	const std::string native_session_id = uam::strings::Trim(payload.value("nativeSessionId", ""));
	const bool select_chat = payload.value("selectChat", true);
	if (native_session_id.empty())
	{
		cb->Failure(400, "A native session id is required.");
		return;
	}

	ChatSession* source_chat = FindChatOrFail(m_app, source_chat_id, cb, "Source chat not found: " + source_chat_id);
	if (source_chat == nullptr)
	{
		return;
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *source_chat);
	if (!ProviderRuntime::UsesNativeOverlayHistory(provider) && !ProviderRuntime::UsesLocalHistory(provider))
	{
		cb->Failure(409, "This provider does not expose a native or local session history path.");
		return;
	}

	const std::string source_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider.id);

	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	const auto previous_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(source_chat->id);
	const bool had_previous_resolved_native_session = previous_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
	const std::string previous_resolved_native_session_id = had_previous_resolved_native_session ? previous_resolved_native_session->second : std::string{};
	ChatSession* target_chat = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false);

	bool inserted_chat = false;
	std::string target_chat_id;
	bool had_previous_target_resolved_native_session = false;
	std::string previous_target_resolved_native_session_id;
	if (target_chat == nullptr)
	{
		target_chat = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false);
		if (target_chat == nullptr)
		{
			cb->Failure(404, "Sub-agent chat not found in native history.");
			return;
		}
		inserted_chat = true;
		target_chat_id = target_chat->id;
	}
	else
	{
		target_chat_id = target_chat->id;
		const auto previous_target_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(target_chat_id);
		had_previous_target_resolved_native_session = previous_target_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
		previous_target_resolved_native_session_id = had_previous_target_resolved_native_session ? previous_target_resolved_native_session->second : std::string{};
		m_app.resolved_native_sessions_by_chat_id[target_chat->id] = native_session_id;
	}

	const std::string previous_provider_id = target_chat->provider_id;
	const std::string previous_native_session_id = target_chat->native_session_id;
	const std::string previous_updated_at = target_chat->updated_at;
	const std::string previous_last_opened_at = target_chat->last_opened_at;
	if (!inserted_chat && !select_chat)
	{
		target_chat = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false);
		if (target_chat == nullptr)
		{
			cb->Failure(404, "Sub-agent chat history is unavailable.");
			return;
		}
	}
	if (target_chat->provider_id.empty())
	{
		target_chat->provider_id = source_provider_id;
	}
	if (select_chat)
	{
		ChatDomainService().SelectChatById(m_app, target_chat_id);
	}

	ChatSession* selected_chat = select_chat ? ChatDomainService().SelectedChat(m_app) : target_chat;
	if (selected_chat == nullptr)
	{
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id, true);
		}
		if (!inserted_chat)
		{
			ChatHistorySyncService().RestoreOpenNativeSessionChatMetadata(*target_chat, previous_provider_id, previous_native_session_id, previous_updated_at);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
		}
		ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat->id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		cb->Failure(404, "Selected chat no longer exists.");
		return;
	}

	if (select_chat)
	{
		selected_chat->last_opened_at = uam::time::TimestampNow();
	}
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		if (select_chat)
		{
			ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			ChatHistorySyncService().RestoreOpenNativeSessionChatMetadata(*selected_chat, previous_provider_id, previous_native_session_id, previous_updated_at);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat->id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id, true);
		}
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *selected_chat, "", ""))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		if (select_chat)
		{
			ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			ChatHistorySyncService().RestoreOpenNativeSessionChatMetadata(*selected_chat, previous_provider_id, previous_native_session_id, previous_updated_at);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat->id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id, true);
		}
		(void)PersistenceCoordinator().SaveSettings(m_app);
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	ChatDomainService().SortChatsByRecent(m_app.chats);
	ChatDomainService().SelectChatById(m_app, select_chat ? target_chat_id : previous_selected_chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"chatId", target_chat_id}}.dump());
}

void UamQueryHandler::HandleSetChatModel(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = uam::nlohmann_json::TrimmedStringValueOr(payload, "chatId", "");
	const std::string model_id = uam::nlohmann_json::TrimmedStringValueOr(payload, "modelId", "");
	const std::string model_role = uam::nlohmann_json::TrimmedStringValueOr(payload, "modelRole", "worker");

	if (!IsAllowedModelId(model_id) || (model_role != "worker" && model_role != "reviewer"))
	{
		cb->Failure(400, "Unsupported ACP model selection.");
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	if (model_role == "reviewer")
	{
		if (chat->reviewer_model_id == model_id)
		{
			cb->Success(nlohmann::json{{"reviewerModelId", model_id}}.dump());
			return;
		}
		const std::string previous_model_id = chat->reviewer_model_id;
		const std::string previous_updated_at = chat->updated_at;
		chat->reviewer_model_id = model_id;
		chat->updated_at = uam::time::TimestampNow();
		if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat reviewer model updated.", "Chat reviewer model changed in UI, but failed to save."))
		{
			chat->reviewer_model_id = previous_model_id;
			chat->updated_at = previous_updated_at;
			cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat reviewer model."));
			return;
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"reviewerModelId", model_id}}.dump());
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool defer_live_update = session != nullptr && AcpSessionBlocksModelChange(*session);
	const bool is_copilot = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCopilotCli);
	const uam::AcpModelState* selected_model = nullptr;
	if (session != nullptr)
	{
		const auto found = std::ranges::find_if(session->available_models, [&model_id](const uam::AcpModelState& model) { return model.id == model_id; });
		if (found != session->available_models.end())
		{
			selected_model = &*found;
		}
	}
	std::string reasoning_effort = chat->reasoning_effort;
	std::string service_tier = chat->service_tier;
	bool service_tier_explicit = chat->service_tier_explicit;
	if (selected_model != nullptr && !reasoning_effort.empty() && !selected_model->supported_reasoning_efforts.empty() &&
	    !uam::ranges::Contains(selected_model->supported_reasoning_efforts, reasoning_effort))
	{
		reasoning_effort = uam::ranges::Contains(selected_model->supported_reasoning_efforts, selected_model->default_reasoning_effort)
		                       ? selected_model->default_reasoning_effort
		                       : selected_model->supported_reasoning_efforts.front();
	}
	if (selected_model != nullptr && !service_tier.empty() && !uam::ranges::Contains(selected_model->additional_speed_tiers, service_tier))
	{
		service_tier.clear();
		service_tier_explicit = true;
	}
	const bool copilot_effort_changed = is_copilot && chat->reasoning_effort != reasoning_effort;
	if (defer_live_update && copilot_effort_changed)
	{
		cb->Failure(409, "Wait for the active Copilot request to finish before changing to a model with a different effort.");
		return;
	}

	if (chat->model_id == model_id && chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier && chat->service_tier_explicit == service_tier_explicit)
	{
		if (!defer_live_update && session != nullptr && session->running && !model_id.empty() && session->current_model_id != model_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"modelId", model_id}, {"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
		return;
	}

	const std::string previous_model_id = chat->model_id;
	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const bool previous_service_tier_explicit = chat->service_tier_explicit;
	const std::string previous_updated_at = chat->updated_at;
	chat->model_id = model_id;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->service_tier_explicit = service_tier_explicit;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model updated.", "Chat model changed in UI, but failed to save."))
	{
		chat->model_id = previous_model_id;
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->service_tier_explicit = previous_service_tier_explicit;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat model."));
		return;
	}

	if (!defer_live_update && session != nullptr && session->running)
	{
		std::string acp_error;
		const bool live_updated = model_id.empty() || copilot_effort_changed ? uam::StopAcpSession(m_app, chat->id) : uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error, previous_model_id);
		if (!live_updated)
		{
			chat->model_id = previous_model_id;
			chat->reasoning_effort = previous_reasoning_effort;
			chat->service_tier = previous_service_tier;
			chat->service_tier_explicit = previous_service_tier_explicit;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model reverted.", "Chat model changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"modelId", model_id}, {"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
}

void UamQueryHandler::HandleSetChatCodexOptions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested_reasoning_effort = payload.value("reasoningEffort", "");
	std::string service_tier = uam::codex::NormalizeServiceTier(payload.value("serviceTier", ""));

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool is_codex = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCodexCli);
	const bool is_copilot = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCopilotCli);
	bool service_tier_explicit = is_codex && (payload.contains("serviceTierExplicit") ? payload.value("serviceTierExplicit", false) : payload.contains("serviceTier"));
	std::string reasoning_effort = is_copilot ? NormalizeCopilotReasoningEffort(requested_reasoning_effort) : uam::codex::NormalizeReasoningEffort(requested_reasoning_effort);
	if (session != nullptr && uam::AcpSessionHasBlockingRuntimeWork(*session))
	{
		cb->Failure(409, "Wait for the active provider request to finish before changing model options.");
		return;
	}
	const uam::AcpModelState* selected_model = nullptr;
	if (session != nullptr)
	{
		const std::string selected_model_id = chat->model_id.empty() ? session->current_model_id : chat->model_id;
		for (const uam::AcpModelState& model : session->available_models)
		{
			if (model.id == selected_model_id)
			{
				selected_model = &model;
				break;
			}
		}
	}
	if (!is_codex && !is_copilot && (selected_model == nullptr || selected_model->supported_reasoning_efforts.empty()))
	{
		cb->Failure(409, "This provider model does not expose reasoning-effort options.");
		return;
	}
	if (selected_model != nullptr && !reasoning_effort.empty() && !selected_model->supported_reasoning_efforts.empty())
	{
		const auto requested = std::ranges::find(selected_model->supported_reasoning_efforts, reasoning_effort);
		if (requested == selected_model->supported_reasoning_efforts.end())
		{
			reasoning_effort = uam::ranges::Contains(selected_model->supported_reasoning_efforts, selected_model->default_reasoning_effort)
			                       ? selected_model->default_reasoning_effort
			                       : selected_model->supported_reasoning_efforts.front();
		}
	}
	if (!is_codex)
	{
		service_tier.clear();
		service_tier_explicit = false;
	}
	if (is_codex && selected_model != nullptr && !service_tier.empty() && !uam::ranges::Contains(selected_model->additional_speed_tiers, service_tier))
	{
		cb->Failure(409, "The selected model does not support that speed tier.");
		return;
	}
	if (chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier && chat->service_tier_explicit == service_tier_explicit)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
		return;
	}

	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const bool previous_service_tier_explicit = chat->service_tier_explicit;
	const std::string previous_updated_at = chat->updated_at;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->service_tier_explicit = service_tier_explicit;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model options updated.", "Chat model options changed in UI, but failed to save."))
	{
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->service_tier_explicit = previous_service_tier_explicit;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist Codex chat options."));
		return;
	}
	if (is_copilot && session != nullptr && session->running)
	{
		(void)uam::StopAcpSession(m_app, chat->id);
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
}

void UamQueryHandler::HandleSetChatProvider(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = ProviderSwitchChatIdFromPayload(payload);
	const std::string provider_id = ProviderSwitchProviderIdFromPayload(payload);

	switch (uam::SwitchChatProvider(m_app, chat_id, provider_id))
	{
	case uam::ChatProviderSwitchResult::UnsupportedProvider:
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	case uam::ChatProviderSwitchResult::ChatNotFound:
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	case uam::ChatProviderSwitchResult::ActiveRuntime:
		cb->Failure(409, "Cannot change provider while a runtime turn or input request is active.");
		return;
	case uam::ChatProviderSwitchResult::SaveFailed:
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat provider."));
		return;
	case uam::ChatProviderSwitchResult::Changed:
	case uam::ChatProviderSwitchResult::Unchanged:
		break;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string mode_id = uam::approval_modes::NormalizeIncomingApprovalModeId(payload.value("modeId", ""));

	if (!uam::approval_modes::IsAgentMode(mode_id))
	{
		cb->Failure(400, "Unsupported ACP mode: " + mode_id);
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool defer_live_update = session != nullptr && AcpSessionBlocksModelChange(*session);
	const std::string effective_mode_id = uam::approval_modes::EffectiveProviderMode(mode_id, chat->command_safety_tier);

	if (chat->approval_mode == mode_id)
	{
		if (!defer_live_update && session != nullptr && session->running && session->current_mode_id != effective_mode_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionMode(m_app, chat->id, effective_mode_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"approvalMode", chat->approval_mode}, {"currentModeId", session == nullptr ? effective_mode_id : session->current_mode_id}}.dump());
		return;
	}

	const std::string previous_mode_id = chat->approval_mode;
	const std::string previous_updated_at = chat->updated_at;
	chat->approval_mode = mode_id;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode updated.", "Chat mode changed in UI, but failed to save."))
	{
		chat->approval_mode = previous_mode_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat mode."));
		return;
	}

	if (!defer_live_update && session != nullptr && session->running)
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, effective_mode_id, &acp_error, previous_mode_id))
		{
			chat->approval_mode = previous_mode_id;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode reverted.", "Chat mode changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"approvalMode", chat->approval_mode}, {"currentModeId", session == nullptr ? effective_mode_id : session->current_mode_id}}.dump());
}

void UamQueryHandler::HandleSetChatUamAgent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string agent_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(payload.value("agentId", "")), "build");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!chat->agent_run_id.empty())
	{
		cb->Failure(403, "Managed child chats cannot change their assigned UAM agent.");
		return;
	}
	const uam::AgentDefinitionCatalog agents = uam::AgentDefinitionService::Load(
	    m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat));
	const auto selected = std::ranges::find(agents.definitions, agent_id, &uam::AgentDefinition::id);
	if (selected == agents.definitions.end() || (selected->mode != "primary" && selected->mode != "both"))
	{
		cb->Failure(400, "UAM agent is unavailable for primary chat use: " + agent_id);
		return;
	}
	if (chat->uam_agent_id == agent_id)
	{
		cb->Success(nlohmann::json{{"uamAgentId", agent_id}}.dump());
		return;
	}
	const std::string previous_agent_id = chat->uam_agent_id;
	const std::string previous_updated_at = chat->updated_at;
	chat->uam_agent_id = agent_id;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "UAM agent updated.", "UAM agent changed in UI, but failed to save."))
	{
		chat->uam_agent_id = previous_agent_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist the UAM agent."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"uamAgentId", chat->uam_agent_id}}.dump());
}

void UamQueryHandler::HandleSetChatUamControlEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled.has_value())
	{
		cb->Failure(400, "Agent goal control requires an explicit boolean enabled value.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!chat->agent_run_id.empty())
	{
		cb->Failure(403, "Managed child chats cannot change UAM Control authority.");
		return;
	}
	const ProviderProfile* provider = ProviderResolutionService().ProviderForChat(m_app, *chat);
	if (*enabled && (provider == nullptr ||
	                 !uam::UamControlService::SupportsStructuredProtocol(provider->structured_protocol)))
	{
		cb->Failure(409, "This provider's structured protocol cannot attach UAM Control. Supported providers are Gemini CLI, OpenCode, and GitHub Copilot CLI.");
		return;
	}
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (*enabled && session != nullptr && session->running && session->uam_control_capability_id.empty())
	{
		cb->Failure(409, "Stop the current provider session before enabling agent goal control.");
		return;
	}
	if (chat->uam_control_enabled == *enabled)
	{
		cb->Success(nlohmann::json{{"enabled", *enabled}}.dump());
		return;
	}

	const bool previous = chat->uam_control_enabled;
	const std::string previous_updated_at = chat->updated_at;
	chat->uam_control_enabled = *enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Agent goal control updated.",
	                                                "Agent goal control changed in UI, but failed to save."))
	{
		chat->uam_control_enabled = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist agent goal control."));
		return;
	}
	if (!*enabled)
	{
		if (session != nullptr)
		{
			uam::UamControlService::RevokeForSession(m_app, *session);
		}
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"enabled", chat->uam_control_enabled}}.dump());
}

void UamQueryHandler::HandleListUamAgents(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;

	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(
	    m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat));
	nlohmann::json agents = nlohmann::json::array();
	for (const uam::AgentDefinition& agent : catalog.definitions)
	{
		if (agent.mode != "primary" && agent.mode != "both") continue;
		agents.push_back({
		    {"id", agent.id},
		    {"description", agent.description},
		    {"builtIn", agent.built_in},
		});
	}
	cb->Success(nlohmann::json{{"agents", std::move(agents)}, {"errors", catalog.errors}}.dump());
}

namespace
{
	nlohmann::json SerializeProviderAgentImportPreview(const uam::ProviderAgentImportPreview& preview)
	{
		return {
		    {"providerId", preview.provider_id},
		    {"sourcePath", uam::paths::Utf8PathString(preview.source_path)},
		    {"suggestedId", preview.suggested_id},
		    {"description", preview.description},
		    {"mode", preview.mode},
		    {"securityFields", preview.security_fields},
		    {"ignoredFields", preview.ignored_fields},
		    {"error", preview.error},
		    {"supported", preview.supported},
		};
	}
}

void UamQueryHandler::HandleBrowseProviderAgentImport(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(payload.value("currentValue", ""));
	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::File, initial_path, &selected_path, &error))
	{
		if (!error.empty()) cb->Failure(500, error);
		else cb->Success(nlohmann::json{{"selectedPath", ""}}.dump());
		return;
	}
	cb->Success(nlohmann::json{{"selectedPath", selected_path}}.dump());
}

void UamQueryHandler::HandlePreviewProviderAgentImport(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(
	    payload.value("providerId", ""), uam::paths::PathFromUtf8(payload.value("sourcePath", "")));
	cb->Success(SerializeProviderAgentImportPreview(preview).dump());
}

void UamQueryHandler::HandleImportProviderAgent(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Open a workspace chat before importing an agent.");
	if (chat == nullptr) return;

	uam::ProviderAgentImportRequest request;
	request.provider_id = payload.value("providerId", "");
	request.source_path = uam::paths::PathFromUtf8(payload.value("sourcePath", ""));
	request.canonical_id = payload.value("canonicalId", "");
	request.workspace_access = payload.value("workspaceAccess", "");
	request.workspace_scope = payload.value("workspaceScope", false);
	request.acknowledge_ignored_fields = payload.value("acknowledgeIgnoredFields", false);

	uam::AgentDefinition imported;
	std::string error;
	if (!uam::AgentDefinitionService::ImportProviderAgent(
	        m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat),
	        request, &imported, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Provider agent import failed."));
		return;
	}
	cb->Success(nlohmann::json{{"id", imported.id}, {"description", imported.description}, {"mode", imported.mode}}.dump());
}

void UamQueryHandler::HandleGetManagedAgentTranscript(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string root_chat_id = payload.value("chatId", "");
	const std::string transcript_chat_id = payload.value("transcriptChatId", "");
	const ChatSession* root_chat = ChatDomainService().FindChatById(m_app, root_chat_id);
	if (root_chat == nullptr || !root_chat->agent_run_id.empty())
	{
		cb->Failure(404, "Managed agent transcript parent chat is unavailable.");
		return;
	}
	const auto run = std::ranges::find_if(m_app.agent_runs, [&](const AgentRun& candidate) {
		return candidate.root_chat_id == root_chat_id && candidate.transcript_chat_id == transcript_chat_id;
	});
	if (run == m_app.agent_runs.end())
	{
		cb->Failure(404, "Managed agent transcript is outside this chat or no longer exists.");
		return;
	}
	std::optional<ChatSession> loaded_transcript;
	ChatSession* transcript = ChatDomainService().FindChatById(m_app, transcript_chat_id);
	if (transcript == nullptr)
	{
		std::string warning;
		loaded_transcript = ChatRepository::LoadLocalChat(m_app.data_root, transcript_chat_id, true, &warning);
		if (!loaded_transcript.has_value())
		{
			cb->Failure(404, FailureDetailOrFallback(warning, "Managed agent transcript is unavailable."));
			return;
		}
		transcript = &*loaded_transcript;
	}
	if (transcript->agent_run_id != run->id)
	{
		cb->Failure(404, "Managed agent transcript is unavailable.");
		return;
	}
	std::string hydrate_warning;
	if (!transcript->messages_loaded &&
	    !ChatRepository::HydrateChatMessages(m_app.data_root, *transcript, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load the managed agent transcript."));
		return;
	}
	const nlohmann::json serialized = uam::StateSerializer::SerializeSession(*transcript);
	cb->Success(nlohmann::json{
	    {"runId", run->id}, {"agentId", run->agent_id}, {"status", run->status},
	    {"providerId", run->provider_id}, {"executionCapability", run->execution_capability},
	    {"resumedFromRunId", run->resumed_from_run_id},
	    {"title", transcript->title}, {"messages", serialized.value("messages", nlohmann::json::array())},
	}.dump());
}

void UamQueryHandler::HandleResumeAgentRun(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string new_run_id;
	std::string error;
	if (!uam::AgentRunScheduler::ResumeInterrupted(
	        m_app, payload.value("runId", ""), &new_run_id, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "Managed run could not be resumed."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"runId", new_run_id}, {"status", "queued"}}.dump());
}

void UamQueryHandler::HandleSetChatCommandSafetyTier(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string raw_requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("commandSafetyTier", "")));
	if (raw_requested != "off" && raw_requested != "acceptedits" && raw_requested != "aireview" && raw_requested != "yolo")
	{
		cb->Failure(400, "Permission mode must be default, accept edits, AI Review, or YOLO.");
		return;
	}
	const std::string requested = raw_requested == "acceptedits" ? uam::approval_modes::kAcceptEditsApprovalMode : raw_requested == "aireview" ? "aiReview" : raw_requested;
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	const auto auto_approve_pending = [&]()
	{
		uam::AcpSessionState* pending_session = uam::FindAcpSessionForChat(m_app, chat->id);
		if (pending_session == nullptr || pending_session->pending_permission.request_id_json.empty()) return;
		std::string acp_error;
		if (uam::TryAutoApprovePendingAcpPermission(m_app, chat->id, &acp_error))
		{
			pending_session->last_error.clear();
		}
		else if (!acp_error.empty())
		{
			pending_session->last_error = acp_error;
		}
	};
	if (chat->command_safety_tier == requested)
	{
		auto_approve_pending();
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"commandSafetyTier", chat->command_safety_tier}}.dump());
		return;
	}

	const std::string previous = chat->command_safety_tier;
	const ChatSession previous_chat = *chat;
	const std::string previous_updated_at = chat->updated_at;
	chat->command_safety_tier = requested;
	const std::string requested_effective_mode = uam::approval_modes::EffectiveProviderMode(chat->approval_mode, requested);
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Command safety tier updated.", "Command safety tier changed in UI, but failed to save."))
	{
		chat->command_safety_tier = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist command safety tier."));
		return;
	}
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (session != nullptr && session->running && !AcpSessionBlocksModelChange(*session) &&
	    CommandSafetyTierNeedsLiveUpdate(previous_chat, *chat))
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, requested_effective_mode, &acp_error, std::nullopt, previous))
		{
			chat->command_safety_tier = previous;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat permissions reverted.", "Chat permissions changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP permissions."));
			return;
		}
	}
	auto_approve_pending();

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"commandSafetyTier", chat->command_safety_tier}}.dump());
}

void UamQueryHandler::HandleSetChatComputerUseEnabled(CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", false);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (enabled && !uam::computer_use::AvailableForChat(*chat))
	{
		cb->Failure(409, "Computer Use is disabled for remote execution hosts.");
		return;
	}
	const bool uses_uam_backend = uam::computer_use::UsesUamBackend(*chat);
	if (enabled && uses_uam_backend)
	{
		cb->Failure(409, "Ask the AI to use Computer Use. UAM will ask you once to approve its chosen target.");
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	if (enabled && session != nullptr && uam::AcpSessionHasActiveTurn(*session))
	{
		cb->Failure(409, "Activate provider computer use after the current structured turn finishes.");
		return;
	}

	if (!enabled)
	{
		// Turning computer use off is a safety boundary: terminate the provider first,
		// even if the cooperative control file cannot be updated.
		(void)uam::StopAcpSession(m_app, chat_id);
		chat->computer_use_enabled = false;
		if (uses_uam_backend)
			(void)uam::ComputerUseService::SetControlState(m_app, chat_id, "stopped");
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	chat->computer_use_enabled = true;
	(void)uam::StopAcpSession(m_app, chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatComputerUseBackend(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("backend", "")));
	if (requested != uam::computer_use::kBackendAuto && requested != uam::computer_use::kBackendProvider && requested != uam::computer_use::kBackendUam)
	{
		cb->Failure(400, "Computer-use backend must be auto, provider, or uam.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (requested == uam::computer_use::kBackendProvider && !uam::computer_use::ProviderBackendAvailable(chat->provider_id))
	{
		cb->Failure(409, "Provider computer use is unavailable in this structured session.");
		return;
	}
	if (chat->computer_use_enabled)
	{
		cb->Failure(409, "Turn off computer use before changing its control method.");
		return;
	}
	if (uam::computer_use::BackendPreference(chat->computer_use_backend) == requested)
	{
		cb->Success("{}");
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	if (session != nullptr && uam::AcpSessionHasActiveTurn(*session))
	{
		cb->Failure(409, "Change the computer-use backend after the current turn finishes.");
		return;
	}

	const ChatSession previous = *chat;
	const std::string previous_updated_at = chat->updated_at;
	chat->computer_use_backend = requested;
	chat->computer_use_target_kind = "window";
	chat->computer_use_target_id.clear();
	chat->computer_use_target_process_id.clear();
	chat->computer_use_target_title.clear();
	chat->computer_use_target_input_mode.clear();
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Computer-use control method updated.", "Computer-use control method changed in UI, but failed to save."))
	{
		*chat = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist the computer-use control method."));
		return;
	}

	(void)uam::StopAcpSession(m_app, chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetComputerUseControl(CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("state", "")));
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!uam::computer_use::UsesUamBackend(*chat))
	{
		cb->Failure(409, "Use the provider's controls for provider computer use.");
		return;
	}
	if (requested == "running" && !chat->computer_use_enabled)
	{
		cb->Failure(409, "Enable computer use before resuming it.");
		return;
	}

	std::string error;
	if (!uam::ComputerUseService::SetControlState(m_app, chat_id, requested, &error))
	{
		cb->Failure(400, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatMemoryEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", true);
	const std::string requested_level = uam::memory_levels::Normalize(uam::nlohmann_json::TrimmedStringValueOr(payload, "memoryLevel", ""), enabled);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->memory_level == requested_level && chat->memory_enabled == uam::memory_levels::IsEnabled(requested_level))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->memory_enabled;
	const std::string previous_level = chat->memory_level;
	const std::string previous_updated_at = chat->updated_at;
	chat->memory_level = requested_level;
	chat->memory_enabled = uam::memory_levels::IsEnabled(requested_level);
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat memory setting updated.", "Chat memory setting changed in UI, but failed to save."))
	{
		chat->memory_enabled = previous;
		chat->memory_level = previous_level;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat memory setting."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatSmallModelMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled)
	{
		cb->Failure(400, "Small-model mode must be a boolean.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}
	if (chat->small_model_mode == *enabled)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->small_model_mode;
	const std::string previous_updated_at = chat->updated_at;
	chat->small_model_mode = *enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Small-model workflow updated.", "Small-model workflow changed in UI, but failed to save."))
	{
		chat->small_model_mode = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist small-model workflow."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
