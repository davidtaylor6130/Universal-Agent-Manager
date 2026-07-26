#include "uam_query_handler.h"
#include "uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/models/app_models.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/state/app_state.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// Chat lifecycle handlers (session CRUD + select)
// Pure chat domain — no cross-dependencies with other handler groups
// ---------------------------------------------------------------------------

void UamQueryHandler::HandleGetInitialState(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	nlohmann::json state = uam::StateSerializer::Serialize(m_app);
	cb->Success(state.dump());
}

void UamQueryHandler::HandleSelectSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	ChatSession* target_chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Selected chat no longer exists.");
	if (target_chat == nullptr)
	{
		return;
	}

	const std::string previous_last_opened_at = target_chat->last_opened_at;
	ChatDomainService().SelectChatById(m_app, chat_id);

	ChatSession* selected_chat = ChatDomainService().SelectedChat(m_app);
	if (selected_chat == nullptr)
	{
		cb->Failure(404, "Selected chat no longer exists.");
		return;
	}

	selected_chat->last_opened_at = uam::time::TimestampNow();
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *selected_chat, "", ""))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		(void)PersistenceCoordinator().SaveSettings(m_app);
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
		return;
	}

	ChatDomainService().SortChatsByRecent(m_app.chats);
	ChatDomainService().SelectChatById(m_app, chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleGetChatMessages(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string known_digest = payload.value("messagesDigest", "");
	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}

	const nlohmann::json serialized = uam::StateSerializer::SerializeSession(*chat);
	const std::string messages_digest = serialized.value("messagesDigest", "");
	nlohmann::json result;
	result["chatId"] = chat_id;
	result["messagesDigest"] = messages_digest;
	if (!known_digest.empty() && known_digest == messages_digest)
	{
		result["unchanged"] = true;
		cb->Success(result.dump());
		return;
	}

	result["unchanged"] = false;
	const nlohmann::json* messages = uam::nlohmann_json::FindArrayField(serialized, "messages");
	result["messages"] = messages == nullptr ? nlohmann::json::array() : *messages;
	cb->Success(result.dump());
}

void UamQueryHandler::HandleGetToolCallContent(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string tool_call_id = payload.value("toolCallId", "");
	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr || tool_call_id.empty())
	{
		if (chat != nullptr)
		{
			cb->Failure(400, "A tool call id is required.");
		}
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}

	for (const Message& message : chat->messages)
	{
		const auto tool = std::ranges::find_if(message.tool_calls, [&](const ToolCall& candidate) { return candidate.id == tool_call_id; });
		if (tool != message.tool_calls.end())
		{
			cb->Success(nlohmann::json{{"content", uam::StateSerializer::ToolCallContentForFrontend(*tool)}}.dump());
			return;
		}
	}

	cb->Failure(404, "Tool call not found: " + tool_call_id);
}

void UamQueryHandler::HandleCreateSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Chat");
	const std::string requested_folder_id = payload.value("folderId", "");
	const ProviderProfile* preferred_provider = uam::query_handler_internal::ResolvePreferredCliProvider(m_app);
	const std::string default_provider_id = uam::query_handler_internal::DefaultNewChatProviderId(m_app, preferred_provider);
	const std::string requested_provider_id = payload.value("providerId", default_provider_id);
	const std::string provider_id = uam::query_handler_internal::ResolveNewChatProviderId(m_app, requested_provider_id, preferred_provider);
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);

	const std::string target_folder_id = uam::query_handler_internal::ResolveRequestedNewChatFolderId(m_app, requested_folder_id);
	if (target_folder_id.empty())
	{
		cb->Failure(400, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "A workspace folder is required to create a chat."));
		return;
	}

	ChatSession chat = ChatDomainService().CreateNewChat(target_folder_id, provider_id);
	if (!title.empty())
	{
		chat.title = title;
	}
	chat.workspace_directory = uam::paths::ResolveWorkspaceRootPath(m_app, chat).string();
	const nlohmann::json* payload_defaults = uam::nlohmann_json::FindObjectField(payload, "defaults");
	uam::query_handler_internal::ApplyProviderDefaultsToChat(m_app.settings, chat, payload_defaults);

	m_app.chats.push_back(std::move(chat));

	ChatSession& created_chat = m_app.chats.back();
	const std::string created_chat_id = created_chat.id;
	ChatDomainService().SelectChatById(m_app, created_chat_id);

	ChatHistorySyncService sync;
	if (!sync.SaveChatWithStatus(m_app, created_chat, "", ""))
	{
		uam::query_handler_internal::RollbackCreatedChat(m_app, created_chat_id, previous_selected_chat_id, false);
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to persist new chat."));
		return;
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		uam::query_handler_internal::RollbackCreatedChat(m_app, created_chat_id, previous_selected_chat_id, true);
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to persist new chat settings."));
		return;
	}

	if (const ChatSession* selected = ChatDomainService().SelectedChat(m_app); selected != nullptr && ProviderResolutionService().ChatUsesCliOutput(m_app, *selected))
	{
		uam::MarkSelectedCliTerminalForLaunch(m_app);
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleBranchFromMessage(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<int> message_index = uam::nlohmann_json::IntFieldStrict(payload, "messageIndex");
	if (!message_index.has_value() || *message_index < 0)
	{
		cb->Failure(400, "A valid message index is required.");
		return;
	}

	if (uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Branch source chat no longer exists.") == nullptr)
	{
		return;
	}
	const std::optional<std::string> replacement_content = payload.contains("content")
	                                                      ? std::optional<std::string>(payload.value("content", ""))
	                                                      : std::nullopt;

	std::string branch_id;
	std::string branch_error;
	if (!uam::BranchFromMessageAndRetry(m_app, chat_id, *message_index, replacement_content, &branch_id, &branch_error))
	{
		const bool branch_was_created = !branch_id.empty();
		if (branch_was_created && ChatDomainService().FindChatById(m_app, branch_id) != nullptr)
		{
			uam::PushStateUpdateIfChanged(browser, m_app);
		}
		cb->Failure(branch_was_created ? 500 : 409,
		            uam::query_handler_internal::FailureDetailOrFallback(
		                branch_error,
		                branch_was_created ? "Branch regeneration could not start." : "Failed to create message branch."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	nlohmann::json result;
	result["chatId"] = branch_id;
	cb->Success(result.dump());
}

void UamQueryHandler::HandleRenameSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string title = payload.value("title", "");

	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatHistorySyncService().RenameChat(m_app, *chat, title))
	{
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to rename chat: " + chat_id));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatPinned(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool pinned = payload.value("pinned", false);

	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->pinned == pinned)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous_pinned = chat->pinned;
	chat->pinned = pinned;

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat pin updated.", "Chat pin changed in UI, but failed to save."))
	{
		chat->pinned = previous_pinned;
		cb->Failure(500, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to persist chat pin."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if (uam::query_handler_internal::FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id) == nullptr)
	{
		return;
	}

	if (!RemoveChatById(m_app, chat_id))
	{
		cb->Failure(409, uam::query_handler_internal::FailureDetailOrFallback(m_app.status_line, "Failed to delete chat: " + chat_id));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
