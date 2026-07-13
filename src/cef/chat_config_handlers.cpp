#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "cef/cef_push.h"
#include "common/config/approval_modes.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/security/command_safety.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// Chat configuration handlers (model, provider, approval, memory)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	uam::CliTerminalState* FindCliTerminalByRoutingKey(uam::AppState& app, const std::string& chat_id, const std::string& terminal_id)
	{
		return uam::FindCliTerminalForRoutingKey(app, chat_id, terminal_id);
	}
} // namespace

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
	const std::string chat_id = payload.value("chatId", "");
	const std::string model_id = uam::strings::Trim(payload.value("modelId", ""));

	if (!IsAllowedAcpModelId(model_id))
	{
		cb->Failure(400, "Unsupported ACP model: " + model_id);
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
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change model while the structured runtime is busy.");
		return;
	}

	if (chat->model_id == model_id)
	{
		if (session != nullptr && session->running && !model_id.empty() && session->current_model_id != model_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::string previous_model_id = chat->model_id;
	const std::string previous_updated_at = chat->updated_at;
	chat->model_id = model_id;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model updated.", "Chat model changed in UI, but failed to save."))
	{
		chat->model_id = previous_model_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat model."));
		return;
	}

	if (session != nullptr && session->running)
	{
		std::string acp_error;
		const bool live_updated = model_id.empty() ? uam::StopAcpSession(m_app, chat->id) : uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error);
		if (!live_updated)
		{
			chat->model_id = previous_model_id;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model reverted.", "Chat model changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatCodexOptions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string reasoning_effort = uam::codex::NormalizeReasoningEffort(payload.value("reasoningEffort", ""));
	const std::string service_tier = uam::codex::NormalizeServiceTier(payload.value("serviceTier", ""));

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCodexCli))
	{
		cb->Failure(409, "Codex reasoning and speed options are only available for Codex chats.");
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change Codex reasoning or speed while the structured runtime is busy.");
		return;
	}

	if (chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const std::string previous_updated_at = chat->updated_at;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Codex chat options updated.", "Codex chat options changed in UI, but failed to save."))
	{
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist Codex chat options."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatProvider(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string provider_id = uam::strings::Trim(payload.value("providerId", ""));

	const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, provider_id);
	if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
	{
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat->provider_id) == provider->id)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::size_t message_count = chat->messages_loaded ? chat->messages.size() : chat->persisted_message_count;
	if (message_count > 0)
	{
		cb->Failure(409, "Cannot change provider after messages have been added.");
		return;
	}

	if (uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id); session != nullptr && session->running)
	{
		cb->Failure(409, "Cannot change provider while the structured runtime is running.");
		return;
	}

	if (uam::CliTerminalState* terminal = FindCliTerminalByRoutingKey(m_app, chat->id, ""); terminal != nullptr && terminal->running)
	{
		cb->Failure(409, "Cannot change provider while the CLI terminal is running.");
		return;
	}

	const std::string previous_provider_id = chat->provider_id;
	const std::string previous_model_id = chat->model_id;
	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const std::string previous_approval_mode = chat->approval_mode;
	const bool previous_auto_approve_commands = chat->auto_approve_commands;
	const bool previous_memory_enabled = chat->memory_enabled;
	const std::string previous_native_session_id = chat->native_session_id;
	const auto previous_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(chat->id);
	const bool had_previous_resolved_native_session = previous_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
	const std::string previous_resolved_native_session_id = had_previous_resolved_native_session ? previous_resolved_native_session->second : std::string{};
	const std::string previous_updated_at = chat->updated_at;
	chat->provider_id = provider->id;
	ApplyProviderDefaultsToChat(m_app.settings, *chat);
	chat->native_session_id.clear();
	ChatHistorySyncService().ForgetResolvedNativeSessionForChat(m_app, chat->id);
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat provider updated.", "Chat provider changed in UI, but failed to save."))
	{
		chat->provider_id = previous_provider_id;
		chat->model_id = previous_model_id;
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->approval_mode = previous_approval_mode;
		chat->auto_approve_commands = previous_auto_approve_commands;
		chat->memory_enabled = previous_memory_enabled;
		chat->native_session_id = previous_native_session_id;
		if (had_previous_resolved_native_session)
		{
			m_app.resolved_native_sessions_by_chat_id[chat->id] = previous_resolved_native_session_id;
		}
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat provider."));
		return;
	}

	uam::ClearStoppedCliTerminalAttachmentForChat(m_app, chat->id);

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string mode_id = uam::approval_modes::NormalizeIncomingApprovalModeId(payload.value("modeId", ""));

	if (!uam::approval_modes::IsAppApprovalMode(mode_id))
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
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change structured runtime mode while the structured runtime is busy.");
		return;
	}

	if (chat->approval_mode == mode_id)
	{
		if (session != nullptr && session->running && session->current_mode_id != mode_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionMode(m_app, chat->id, mode_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
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

	if (session != nullptr && session->running)
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, mode_id, &acp_error))
		{
			chat->approval_mode = previous_mode_id;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode reverted.", "Chat mode changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatAutoApproveCommands(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", false);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->auto_approve_commands == enabled)
	{
		if (enabled && !AutoApprovePendingAcpPermissionOrFail(m_app, chat->id, cb))
		{
			return;
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->auto_approve_commands;
	const std::string previous_updated_at = chat->updated_at;
	chat->auto_approve_commands = enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat auto-approval updated.", "Chat auto-approval changed in UI, but failed to save."))
	{
		chat->auto_approve_commands = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat auto-approval."));
		return;
	}

	if (enabled && !AutoApprovePendingAcpPermissionOrFail(m_app, chat->id, cb))
	{
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatCommandSafetyTier(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("commandSafetyTier", "")));
	if (requested != "low" && requested != "medium" && requested != "high")
	{
		cb->Failure(400, "Command safety tier must be low, medium, or high.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (chat->command_safety_tier == requested)
	{
		cb->Success("{}");
		return;
	}

	const std::string previous = chat->command_safety_tier;
	const std::string previous_updated_at = chat->updated_at;
	chat->command_safety_tier = requested;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Command safety tier updated.", "Command safety tier changed in UI, but failed to save."))
	{
		chat->command_safety_tier = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist command safety tier."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatMemoryEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", true);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->memory_enabled == enabled)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->memory_enabled;
	const std::string previous_updated_at = chat->updated_at;
	chat->memory_enabled = enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat memory setting updated.", "Chat memory setting changed in UI, but failed to save."))
	{
		chat->memory_enabled = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat memory setting."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
