#include "cef/uam_query_handler.h"
#include "cef/uam_bridge_request.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_security.h"

#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/application_core_helpers.h"
#include "app/persistence_coordinator.h"
#include "app/runtime_orchestration_services.h"
#include "common/paths/app_paths.h"

#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/chat/chat_folder_store.h"
#include "common/utils/string_utils.h"

#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{
		constexpr const char* kPreferredProviderId = "gemini-cli";
		constexpr std::size_t kRecentOutputReplayLimitBytes = 256 * 1024;
		constexpr std::size_t kMaxClipboardTextBytes = 1024 * 1024;

	int FolderFailureCode(const std::string& status_line)
	{
		if (status_line.find("no longer exists") != std::string::npos || status_line.find("not found") != std::string::npos)
		{
			return 404;
		}

		return 400;
	}

	const ProviderProfile* ResolvePreferredCliProvider(const uam::AppState& app)
	{
		if (const ProviderProfile* preferred = ProviderProfileStore::FindById(app.provider_profiles, kPreferredProviderId); preferred != nullptr)
		{
			if (ProviderRuntime::IsRuntimeEnabled(*preferred) && ProviderRuntime::UsesCliOutput(*preferred) && preferred->supports_interactive)
			{
				return preferred;
			}
		}

		for (const ProviderProfile& provider : app.provider_profiles)
		{
			if (ProviderRuntime::IsRuntimeEnabled(provider) && ProviderRuntime::UsesCliOutput(provider) && provider.supports_interactive)
			{
				return &provider;
			}
		}

		return nullptr;
	}

		bool IsSafeAcpToken(const std::string& value)
		{
			if (value.empty() || value.size() > 160 || value.front() == '-')
			{
				return false;
			}
			for (const char ch : value)
			{
				const bool safe =
				    (ch >= 'a' && ch <= 'z') ||
				    (ch >= 'A' && ch <= 'Z') ||
				    (ch >= '0' && ch <= '9') ||
				    ch == '.' ||
				    ch == '_' ||
				    ch == '-' ||
				    ch == ':' ||
				    ch == '/';
				if (!safe)
				{
					return false;
				}
			}
			return true;
		}

		bool IsAllowedAcpModelId(const std::string& model_id)
		{
			return model_id.empty() || IsSafeAcpToken(model_id);
		}

		std::string NormalizeAcpApprovalMode(const std::string& mode_id)
		{
			const std::string trimmed = Trim(mode_id);
			return trimmed.empty() ? "default" : trimmed;
		}

		bool IsAllowedAcpApprovalMode(const std::string& mode_id)
		{
			return mode_id == "default" || mode_id == "plan";
		}

		bool AcpSessionBlocksModelChange(const uam::AcpSessionState& session)
	{
		return session.processing ||
		       session.waiting_for_permission ||
		       session.waiting_for_user_input ||
		       session.initialize_request_id != 0 ||
		       session.session_setup_request_id != 0 ||
		       session.prompt_request_id != 0 ||
		       session.cancel_request_id != 0 ||
			       !session.queued_prompt.empty();
		}

#if defined(_WIN32)
		std::wstring WideFromUtf8(const std::string& value)
		{
			if (value.empty())
			{
				return std::wstring();
			}
			const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
			if (wide_len <= 0)
			{
				return std::wstring();
			}
			std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), wide_len) <= 0)
			{
				return std::wstring();
			}
			return wide;
		}
#endif

		bool WriteNativeClipboardText(const std::string& text, std::string* error_out)
		{
#if defined(__APPLE__)
			FILE* pipe = popen("/usr/bin/pbcopy", "w");
			if (pipe == nullptr)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to launch pbcopy.";
				}
				return false;
			}

			const std::size_t written = std::fwrite(text.data(), 1, text.size(), pipe);
			const int status = pclose(pipe);
			if (written != text.size() || status != 0)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to write clipboard text through pbcopy.";
				}
				return false;
			}
			return true;
#elif defined(_WIN32)
			const std::wstring wide = WideFromUtf8(text);
			if (wide.empty() && !text.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Clipboard text is not valid UTF-8.";
				}
				return false;
			}
			if (!OpenClipboard(nullptr))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to open the Windows clipboard.";
				}
				return false;
			}

			if (!EmptyClipboard())
			{
				CloseClipboard();
				if (error_out != nullptr)
				{
					*error_out = "Failed to clear the Windows clipboard.";
				}
				return false;
			}

			const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
			HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
			if (memory == nullptr)
			{
				CloseClipboard();
				if (error_out != nullptr)
				{
					*error_out = "Failed to allocate Windows clipboard memory.";
				}
				return false;
			}

			void* locked = GlobalLock(memory);
			if (locked == nullptr)
			{
				GlobalFree(memory);
				CloseClipboard();
				if (error_out != nullptr)
				{
					*error_out = "Failed to lock Windows clipboard memory.";
				}
				return false;
			}
			std::memcpy(locked, wide.c_str(), bytes);
			GlobalUnlock(memory);

			if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
			{
				GlobalFree(memory);
				CloseClipboard();
				if (error_out != nullptr)
				{
					*error_out = "Failed to set Windows clipboard text.";
				}
				return false;
			}
			CloseClipboard();
			return true;
#else
			if (error_out != nullptr)
			{
				*error_out = "Native clipboard writes are not implemented for this platform.";
			}
			return false;
#endif
		}

	static const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string Base64Encode(const std::string& input)
	{
		std::string out;
		out.reserve(((input.size() + 2) / 3) * 4);

		std::size_t i = 0;
		unsigned char buf[3];
		while (i + 3 <= input.size())
		{
			buf[0] = static_cast<unsigned char>(input[i]);
			buf[1] = static_cast<unsigned char>(input[i + 1]);
			buf[2] = static_cast<unsigned char>(input[i + 2]);
			out += kBase64Chars[(buf[0] >> 2) & 0x3F];
			out += kBase64Chars[((buf[0] & 0x03) << 4) | ((buf[1] >> 4) & 0x0F)];
			out += kBase64Chars[((buf[1] & 0x0F) << 2) | ((buf[2] >> 6) & 0x03)];
			out += kBase64Chars[buf[2] & 0x3F];
			i += 3;
		}

		const std::size_t remaining = input.size() - i;
		if (remaining == 1)
		{
			buf[0] = static_cast<unsigned char>(input[i]);
			out += kBase64Chars[(buf[0] >> 2) & 0x3F];
			out += kBase64Chars[(buf[0] & 0x03) << 4];
			out += "==";
		}
		else if (remaining == 2)
		{
			buf[0] = static_cast<unsigned char>(input[i]);
			buf[1] = static_cast<unsigned char>(input[i + 1]);
			out += kBase64Chars[(buf[0] >> 2) & 0x3F];
			out += kBase64Chars[((buf[0] & 0x03) << 4) | ((buf[1] >> 4) & 0x0F)];
			out += kBase64Chars[(buf[1] & 0x0F) << 2];
			out += '=';
		}

		return out;
	}

	uam::CliTerminalState* FindCliTerminalByRoutingKey(uam::AppState& app, const std::string& chat_id, const std::string& terminal_id)
	{
		if (!terminal_id.empty())
		{
			for (auto& term : app.cli_terminals)
			{
				if (term && term->terminal_id == terminal_id)
				{
					return term.get();
				}
			}
		}

		if (!chat_id.empty())
		{
			for (auto& term : app.cli_terminals)
			{
				if (term && CliTerminalMatchesChatId(*term, chat_id))
				{
					return term.get();
				}
			}
		}

		return nullptr;
	}

	bool CliInputLooksLikeTurnSubmit(const std::string& data)
	{
		return data.find('\r') != std::string::npos || data.find('\n') != std::string::npos;
	}

	nlohmann::json BuildCliBindingResponse(const uam::CliTerminalState& terminal)
	{
		nlohmann::json data;
		data["terminalId"] = terminal.terminal_id;
		data["sessionId"] = terminal.frontend_chat_id;
		data["sourceChatId"] = CliTerminalPrimaryChatId(terminal);
		data["running"] = terminal.running;
		data["lifecycleState"] = CliTerminalLifecycleStateLabel(terminal);
		data["turnState"] = CliTerminalLifecycleIsProcessing(terminal) ? "busy" : "idle";
		data["lastError"] = terminal.last_error;

		if (!terminal.recent_output_bytes.empty())
		{
			const std::size_t start_offset = terminal.recent_output_bytes.size() > kRecentOutputReplayLimitBytes ? terminal.recent_output_bytes.size() - kRecentOutputReplayLimitBytes : 0;
			data["replayData"] = Base64Encode(terminal.recent_output_bytes.substr(start_offset));
		}

		return data;
	}
} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UamQueryHandler::UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url) : m_app(app), m_trustedUiIndexUrl(std::move(trusted_ui_index_url))
{
}

// ---------------------------------------------------------------------------
// CefMessageRouterBrowserSide::Handler
// ---------------------------------------------------------------------------

bool UamQueryHandler::OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t /*query_id*/, const CefString& request, bool /*persistent*/, CefRefPtr<Callback> callback)
{
	CEF_REQUIRE_UI_THREAD();

	if (!frame->IsMain() || !uam::cef::IsTrustedUiUrl(frame->GetURL().ToString(), m_trustedUiIndexUrl))
	{
		callback->Failure(403, "Privileged bridge is restricted to the bundled UI.");
		return true;
	}

	const uam::cef::BridgeRequestParseResult parsed = uam::cef::ParseBridgeRequest(request.ToString());
	if (!parsed.ok)
	{
		callback->Failure(parsed.status, parsed.error);
		return true;
	}

	const std::string& action = parsed.request.action;
	const nlohmann::json& payload = parsed.request.payload;

	try
	{
		if (action == "getInitialState")
			HandleGetInitialState(browser, callback);
		else if (action == "selectSession")
			HandleSelectSession(browser, payload, callback);
		else if (action == "createSession")
			HandleCreateSession(browser, payload, callback);
		else if (action == "renameSession")
			HandleRenameSession(browser, payload, callback);
		else if (action == "setChatPinned")
			HandleSetChatPinned(browser, payload, callback);
		else if (action == "setChatProvider")
			HandleSetChatProvider(browser, payload, callback);
		else if (action == "setChatModel")
			HandleSetChatModel(browser, payload, callback);
		else if (action == "setChatApprovalMode")
			HandleSetChatApprovalMode(browser, payload, callback);
		else if (action == "deleteSession")
			HandleDeleteSession(browser, payload, callback);
		else if (action == "createFolder")
			HandleCreateFolder(browser, payload, callback);
		else if (action == "renameFolder")
			HandleRenameFolder(browser, payload, callback);
		else if (action == "deleteFolder")
			HandleDeleteFolder(browser, payload, callback);
		else if (action == "toggleFolder")
			HandleToggleFolder(browser, payload, callback);
		else if (action == "browseFolderDirectory")
			HandleBrowseFolderDirectory(browser, payload, callback);
		else if (action == "startCliTerminal")
			HandleStartCli(browser, payload, callback);
		else if (action == "stopCliTerminal")
			HandleStopCli(browser, payload, callback);
		else if (action == "resizeCliTerminal")
			HandleResizeCli(payload, callback);
		else if (action == "writeCliInput")
			HandleWriteCliInput(browser, payload, callback);
		else if (action == "sendAcpPrompt")
			HandleSendAcpPrompt(browser, payload, callback);
		else if (action == "cancelAcpTurn")
			HandleCancelAcpTurn(browser, payload, callback);
		else if (action == "resolveAcpPermission")
			HandleResolveAcpPermission(browser, payload, callback);
		else if (action == "resolveAcpUserInput")
			HandleResolveAcpUserInput(browser, payload, callback);
			else if (action == "stopAcpSession")
				HandleStopAcpSession(browser, payload, callback);
			else if (action == "writeClipboardText")
				HandleWriteClipboardText(payload, callback);
			else if (action == "setTheme")
				HandleSetTheme(browser, payload, callback);
		else
		{
			callback->Failure(404, "Unknown action: " + action);
		}
	}
	catch (const nlohmann::json::exception& ex)
	{
		callback->Failure(400, std::string("Invalid bridge payload: ") + ex.what());
	}
	catch (const std::exception& ex)
	{
		callback->Failure(500, std::string("Bridge request failed: ") + ex.what());
	}

	return true;
}

void UamQueryHandler::OnQueryCanceled(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, int64_t /*query_id*/)
{
	// Persistent queries are not used; nothing to cancel.
}

// ---------------------------------------------------------------------------
// Action handlers
// ---------------------------------------------------------------------------

void UamQueryHandler::HandleGetInitialState(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<Callback> cb)
{
	nlohmann::json state = uam::StateSerializer::Serialize(m_app);
	cb->Success(state.dump());
}

void UamQueryHandler::HandleSelectSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string previous_selected_chat_id = (ChatDomainService().SelectedChat(m_app) != nullptr) ? ChatDomainService().SelectedChat(m_app)->id : std::string{};
	const int target_chat_index = ChatDomainService().FindChatIndexById(m_app, chat_id);

	if (target_chat_index < 0)
	{
		cb->Failure(404, "Selected chat no longer exists.");
		return;
	}

	const std::string previous_last_opened_at = m_app.chats[static_cast<std::size_t>(target_chat_index)].last_opened_at;
	ChatDomainService().SelectChatById(m_app, chat_id);

	ChatSession* selected_chat = ChatDomainService().SelectedChat(m_app);
	if (selected_chat == nullptr)
	{
		cb->Failure(404, "Selected chat no longer exists.");
		return;
	}

	selected_chat->last_opened_at = TimestampNow();
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist selected chat." : m_app.status_line);
		return;
	}

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *selected_chat, "", ""))
	{
		selected_chat->last_opened_at = previous_last_opened_at;
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		(void)PersistenceCoordinator().SaveSettings(m_app);
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist selected chat." : m_app.status_line);
		return;
	}

	ChatDomainService().SortChatsByRecent(m_app.chats);
	ChatDomainService().SelectChatById(m_app, chat_id);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCreateSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Chat");
	const std::string requested_folder_id = payload.value("folderId", "");
	const ProviderProfile* preferred_provider = ResolvePreferredCliProvider(m_app);
	const std::string requested_provider_id = payload.value("providerId", preferred_provider != nullptr ? preferred_provider->id : m_app.settings.active_provider_id);
	const ProviderProfile* requested_provider = ProviderProfileStore::FindById(m_app.provider_profiles, requested_provider_id);
	const std::string provider_id = requested_provider != nullptr ? requested_provider->id : (preferred_provider != nullptr ? preferred_provider->id : m_app.settings.active_provider_id);
	const std::string previous_selected_chat_id = (ChatDomainService().SelectedChat(m_app) != nullptr) ? ChatDomainService().SelectedChat(m_app)->id : std::string{};

	const std::string target_folder_id = ResolveRequestedNewChatFolderId(m_app, requested_folder_id);
	if (target_folder_id.empty())
	{
		cb->Failure(400, m_app.status_line.empty() ? "A workspace folder is required to create a chat." : m_app.status_line);
		return;
	}

	ChatSession chat = ChatDomainService().CreateNewChat(target_folder_id, provider_id);
	if (!title.empty())
		chat.title = title;
	chat.workspace_directory = ResolveWorkspaceRootPath(m_app, chat).string();

	m_app.chats.push_back(std::move(chat));

	ChatSession& created_chat = m_app.chats.back();
	const std::string created_chat_id = created_chat.id;
	ChatDomainService().SelectChatById(m_app, created_chat_id);

	ChatHistorySyncService sync;
	if (!sync.SaveChatWithStatus(m_app, created_chat, "", ""))
	{
		const int created_chat_index = ChatDomainService().FindChatIndexById(m_app, created_chat_id);
		if (created_chat_index >= 0)
		{
			m_app.chats.erase(m_app.chats.begin() + created_chat_index);
		}

		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist new chat." : m_app.status_line);
		return;
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		const int created_chat_index = ChatDomainService().FindChatIndexById(m_app, created_chat_id);
		if (created_chat_index >= 0)
		{
			m_app.chats.erase(m_app.chats.begin() + created_chat_index);
		}

		std::error_code cleanup_ec;
		std::filesystem::remove_all(AppPaths::ChatPath(m_app.data_root, created_chat_id), cleanup_ec);
		std::filesystem::remove(AppPaths::UamChatFilePath(m_app.data_root, created_chat_id), cleanup_ec);
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist new chat settings." : m_app.status_line);
		return;
	}

	if (const ChatSession* selected = ChatDomainService().SelectedChat(m_app); selected != nullptr && ProviderResolutionService().ChatUsesCliOutput(m_app, *selected))
	{
		MarkSelectedCliTerminalForLaunch(m_app);
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRenameSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string title = payload.value("title", "");

	const int idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(idx)];
	if (!ChatHistorySyncService().RenameChat(m_app, chat, title))
	{
		cb->Failure(500, m_app.status_line.empty() ? ("Failed to rename chat: " + chat_id) : m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatPinned(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool pinned = payload.value("pinned", false);

	const int idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(idx)];
	if (chat.pinned == pinned)
	{
		uam::PushStateUpdate(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous_pinned = chat.pinned;
	chat.pinned = pinned;

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, chat, "Chat pin updated.", "Chat pin changed in UI, but failed to save."))
	{
		chat.pinned = previous_pinned;
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist chat pin." : m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatModel(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string model_id = Trim(payload.value("modelId", ""));

	if (!IsAllowedAcpModelId(model_id))
	{
		cb->Failure(400, "Unsupported ACP model: " + model_id);
		return;
	}

	const int idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(idx)];
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat.id);
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change model while the structured runtime is busy.");
		return;
	}

	if (chat.model_id == model_id)
	{
		if (session != nullptr && session->running && !model_id.empty() && session->current_model_id != model_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionModel(m_app, chat.id, model_id, &acp_error))
			{
				cb->Failure(409, acp_error.empty() ? "Failed to update live ACP model." : acp_error);
				return;
			}
		}
		uam::PushStateUpdate(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::string previous_model_id = chat.model_id;
	const std::string previous_updated_at = chat.updated_at;
	chat.model_id = model_id;
	chat.updated_at = TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, chat, "Chat model updated.", "Chat model changed in UI, but failed to save."))
	{
		chat.model_id = previous_model_id;
		chat.updated_at = previous_updated_at;
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist chat model." : m_app.status_line);
		return;
	}

	if (session != nullptr && session->running)
	{
		std::string acp_error;
		const bool live_updated = model_id.empty()
			? uam::StopAcpSession(m_app, chat.id)
			: uam::SetAcpSessionModel(m_app, chat.id, model_id, &acp_error);
		if (!live_updated)
		{
			chat.model_id = previous_model_id;
			chat.updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, chat, "Chat model reverted.", "Chat model changed in UI, but failed to revert.");
			cb->Failure(409, acp_error.empty() ? "Failed to update live ACP model." : acp_error);
			return;
		}
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatProvider(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string provider_id = Trim(payload.value("providerId", ""));

	const ProviderProfile* provider = ProviderProfileStore::FindById(m_app.provider_profiles, provider_id);
	if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
	{
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	}

	const int idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(idx)];
	if (chat.provider_id == provider->id)
	{
		uam::PushStateUpdate(browser, m_app);
		cb->Success("{}");
		return;
	}

	if (!chat.messages.empty())
	{
		cb->Failure(409, "Cannot change provider after messages have been added.");
		return;
	}

	if (uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat.id); session != nullptr && session->running)
	{
		cb->Failure(409, "Cannot change provider while the structured runtime is running.");
		return;
	}

	if (uam::CliTerminalState* terminal = FindCliTerminalByRoutingKey(m_app, chat.id, ""); terminal != nullptr && terminal->running)
	{
		cb->Failure(409, "Cannot change provider while the CLI terminal is running.");
		return;
	}

	const std::string previous_provider_id = chat.provider_id;
	const std::string previous_model_id = chat.model_id;
	const std::string previous_approval_mode = chat.approval_mode;
	const std::string previous_native_session_id = chat.native_session_id;
	const std::string previous_updated_at = chat.updated_at;
	chat.provider_id = provider->id;
	chat.model_id.clear();
	chat.approval_mode = "default";
	chat.native_session_id.clear();
	chat.updated_at = TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, chat, "Chat provider updated.", "Chat provider changed in UI, but failed to save."))
	{
		chat.provider_id = previous_provider_id;
		chat.model_id = previous_model_id;
		chat.approval_mode = previous_approval_mode;
		chat.native_session_id = previous_native_session_id;
		chat.updated_at = previous_updated_at;
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist chat provider." : m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string mode_id = NormalizeAcpApprovalMode(payload.value("modeId", ""));

	if (!IsAllowedAcpApprovalMode(mode_id))
	{
		cb->Failure(400, "Unsupported ACP mode: " + mode_id);
		return;
	}

	const int idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(idx)];
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat.id);
	if (session != nullptr && AcpSessionBlocksModelChange(*session))
	{
		cb->Failure(409, "Cannot change structured runtime mode while the structured runtime is busy.");
		return;
	}

	if (chat.approval_mode == mode_id)
	{
		if (session != nullptr && session->running && session->current_mode_id != mode_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionMode(m_app, chat.id, mode_id, &acp_error))
			{
				cb->Failure(409, acp_error.empty() ? "Failed to update live ACP mode." : acp_error);
				return;
			}
		}
		uam::PushStateUpdate(browser, m_app);
		cb->Success("{}");
		return;
	}

	const std::string previous_mode_id = chat.approval_mode;
	const std::string previous_updated_at = chat.updated_at;
	chat.approval_mode = mode_id;
	chat.updated_at = TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, chat, "Chat mode updated.", "Chat mode changed in UI, but failed to save."))
	{
		chat.approval_mode = previous_mode_id;
		chat.updated_at = previous_updated_at;
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist chat mode." : m_app.status_line);
		return;
	}

	if (session != nullptr && session->running)
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat.id, mode_id, &acp_error))
		{
			chat.approval_mode = previous_mode_id;
			chat.updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, chat, "Chat mode reverted.", "Chat mode changed in UI, but failed to revert.");
			cb->Failure(409, acp_error.empty() ? "Failed to update live ACP mode." : acp_error);
			return;
		}
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const int idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	if (!RemoveChatById(m_app, chat_id))
	{
		cb->Failure(409, m_app.status_line.empty() ? ("Failed to delete chat: " + chat_id) : m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCreateFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Folder");
	const std::string directory = payload.value("directory", "");
	std::string created_folder_id;

	if (!CreateFolder(m_app, title, directory, &created_folder_id))
	{
		cb->Failure(400, m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	ChatFolder* folder = ChatDomainService().FindFolderById(m_app, created_folder_id);
	if (folder == nullptr)
	{
		cb->Success("{}");
		return;
	}

	cb->Success(uam::StateSerializer::SerializeFolder(*folder).dump());
}

void UamQueryHandler::HandleRenameFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = payload.value("folderId", "");
	const std::string title = payload.value("title", "");
	const std::string directory = payload.value("directory", "");

	if (!RenameFolderById(m_app, folder_id, title, directory))
	{
		cb->Failure(FolderFailureCode(m_app.status_line), m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = payload.value("folderId", "");

	if (!DeleteFolderById(m_app, folder_id))
	{
		cb->Failure(FolderFailureCode(m_app.status_line), m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleToggleFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = payload.value("folderId", "");
	ChatFolder* folder = ChatDomainService().FindFolderById(m_app, folder_id);
	if (!folder)
	{
		cb->Failure(404, "Folder not found: " + folder_id);
		return;
	}

	folder->collapsed = !folder->collapsed;

	if (!ChatFolderStore::Save(m_app.data_root, m_app.folders))
	{
		folder->collapsed = !folder->collapsed;
		cb->Failure(500, "Failed to persist folder state.");
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleBrowseFolderDirectory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string current_value = payload.value("currentValue", "");
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(current_value);

	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::Directory, initial_path, &selected_path, &error))
	{
		if (!error.empty())
		{
			cb->Failure(500, error);
		}
		else
		{
			nlohmann::json result;
			result["selectedPath"] = "";
			cb->Success(result.dump());
		}
		return;
	}

	nlohmann::json result;
	result["selectedPath"] = selected_path;
	cb->Success(result.dump());
}

void UamQueryHandler::HandleStartCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const int rows = payload.value("rows", 24);
	const int cols = payload.value("cols", 80);
	const std::string terminal_id = payload.value("terminalId", "");
	uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "request_received", nullptr, "chat_id=" + chat_id + ", terminal_id=" + terminal_id);

	if (uam::CliTerminalState* existing = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); existing != nullptr && existing->running)
	{
		if (existing->lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown)
		{
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "restart_shutting_down_terminal", existing);
			StopCliTerminal(*existing, false, CliTerminalStopMode::FastExit);
		}
		else
		{
			existing->ui_attached = true;
			existing->rows = std::max(1, rows);
			existing->cols = std::max(1, cols);
			PlatformServicesFactory::Instance().terminal_runtime.ResizeCliTerminal(*existing);
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "reused_running_terminal", existing);
			cb->Success(BuildCliBindingResponse(*existing).dump());
			return;
		}
	}

	const int chat_idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (chat_idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(chat_idx)];
	uam::CliTerminalState& terminal = EnsureCliTerminalForChat(m_app, chat);
	terminal.frontend_chat_id = chat.id;
	terminal.ui_attached = true;
	if (terminal.terminal_id.empty())
	{
		terminal.terminal_id = "term-" + chat.id;
	}
	uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "terminal_prepared", &terminal);

	if (!terminal.running)
	{
		if (!StartCliTerminalForChat(m_app, terminal, chat, rows, cols))
		{
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "start_failed", &terminal, terminal.last_error);
			cb->Success(BuildCliBindingResponse(terminal).dump());
			return;
		}

		uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "started_terminal", &terminal);
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success(BuildCliBindingResponse(terminal).dump());
}

void UamQueryHandler::HandleStopCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id);

	if (term == nullptr)
	{
		cb->Success("{}");
		return;
	}

	term->ui_attached = false;
	uam::LogCliDiagnosticEvent(m_app, "handle_stop_cli", "ui_detached", term);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResizeCli(const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	const int rows = std::max(1, payload.value("rows", 24));
	const int cols = std::max(1, payload.value("cols", 80));

	if (uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); term != nullptr && term->running)
	{
		term->rows = rows;
		term->cols = cols;
		PlatformServicesFactory::Instance().terminal_runtime.ResizeCliTerminal(*term);
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleWriteCliInput(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	const std::string data = payload.value("data", "");

	if (!data.empty())
	{
		if (uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); term != nullptr && term->running && term->lifecycle_state != uam::CliTerminalLifecycleState::ShuttingDown)
		{
			// Write raw PTY bytes directly to the terminal master fd.
			// xterm.js sends individual keystrokes and escape sequences that
			// must reach the child process unmodified — do NOT queue these as
			// structured prompts (which wrap them in bracketed-paste sequences
			// and append \r, breaking all interactive CLI communication).
			const bool wrote = WriteToCliTerminal(*term, data.c_str(), data.size());
			uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", wrote ? "pty_write_ok" : "pty_write_failed", term, "", static_cast<long long>(data.size()));
			if (wrote && CliInputLooksLikeTurnSubmit(data))
			{
				MarkCliTerminalTurnBusy(*term);
				uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", "turn_marked_busy_from_submit", term);
				uam::PushStateUpdate(browser, m_app);
			}
		}
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleSendAcpPrompt(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string text = payload.value("text", "");

	std::string error;
	if (!uam::SendAcpPrompt(m_app, chat_id, text, &error))
	{
		cb->Failure(chat_id.empty() || text.empty() ? 400 : 500, error.empty() ? "Failed to send ACP prompt." : error);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCancelAcpTurn(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");

	std::string error;
	if (!uam::CancelAcpTurn(m_app, chat_id, &error))
	{
		cb->Failure(500, error.empty() ? "Failed to cancel ACP turn." : error);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResolveAcpPermission(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	const std::string option_id = payload.value("optionId", "");
	const bool cancelled = payload.value("cancelled", false) || option_id == "cancelled";

	std::string error;
	if (!uam::ResolveAcpPermission(m_app, chat_id, request_id, option_id, cancelled, &error))
	{
		cb->Failure(409, error.empty() ? "Failed to resolve ACP permission request." : error);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResolveAcpUserInput(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string request_id = payload.value("requestId", "");
	std::map<std::string, std::vector<std::string>> answers;

	const nlohmann::json raw_answers = payload.value("answers", nlohmann::json::object());
	if (!raw_answers.is_object())
	{
		cb->Failure(400, "ACP user input answers must be an object.");
		return;
	}

	for (auto it = raw_answers.begin(); it != raw_answers.end(); ++it)
	{
		if (it.key().empty())
		{
			continue;
		}

		std::vector<std::string> values;
		if (it.value().is_array())
		{
			for (const nlohmann::json& value : it.value())
			{
				if (value.is_string())
				{
					values.push_back(value.get<std::string>());
				}
			}
		}
		else if (it.value().is_string())
		{
			values.push_back(it.value().get<std::string>());
		}
		answers[it.key()] = std::move(values);
	}

	std::string error;
	if (!uam::ResolveAcpUserInput(m_app, chat_id, request_id, answers, &error))
	{
		cb->Failure(409, error.empty() ? "Failed to resolve ACP user input request." : error);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleStopAcpSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	uam::StopAcpSession(m_app, chat_id);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleWriteClipboardText(const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string text = payload.value("text", "");
	if (text.empty())
	{
		cb->Failure(400, "Clipboard text is empty.");
		return;
	}
	if (text.size() > kMaxClipboardTextBytes)
	{
		cb->Failure(413, "Clipboard text is too large.");
		return;
	}

	std::string error;
	if (!WriteNativeClipboardText(text, &error))
	{
		cb->Failure(500, error.empty() ? "Failed to write clipboard text." : error);
		return;
	}

	cb->Success(R"({"copied":true})");
}

void UamQueryHandler::HandleSetTheme(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string theme = payload.value("theme", "dark");
	const std::string previous_theme = m_app.settings.ui_theme;
	m_app.settings.ui_theme = theme;
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings.ui_theme = previous_theme;
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist theme." : m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}
