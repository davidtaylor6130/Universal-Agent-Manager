#include "cef/uam_query_handler.h"
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

#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <utility>

namespace
{
	constexpr const char* kPreferredProviderId = "gemini-cli";
	constexpr std::size_t kRecentOutputReplayLimitBytes = 256 * 1024;

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

	void EnsureChatUsesPreferredCliProvider(uam::AppState& app, ChatSession& chat)
	{
		const ProviderProfile* preferred = ResolvePreferredCliProvider(app);
		if (preferred == nullptr || preferred->id.empty())
		{
			return;
		}

		if (chat.provider_id != preferred->id)
		{
			chat.provider_id = preferred->id;
		}
	}

	static const char kBase64Chars[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
			const std::size_t start_offset = terminal.recent_output_bytes.size() > kRecentOutputReplayLimitBytes
				? terminal.recent_output_bytes.size() - kRecentOutputReplayLimitBytes
				: 0;
			data["replayData"] = Base64Encode(terminal.recent_output_bytes.substr(start_offset));
		}

		return data;
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UamQueryHandler::UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url)
	: m_app(app)
	, m_trustedUiIndexUrl(std::move(trusted_ui_index_url))
{}

// ---------------------------------------------------------------------------
// CefMessageRouterBrowserSide::Handler
// ---------------------------------------------------------------------------

bool UamQueryHandler::OnQuery(CefRefPtr<CefBrowser>  browser,
                               CefRefPtr<CefFrame>    frame,
                               int64_t                /*query_id*/,
                               const CefString&       request,
                               bool                   /*persistent*/,
                               CefRefPtr<Callback>    callback)
{
	CEF_REQUIRE_UI_THREAD();

	if (!frame->IsMain() || !uam::cef::IsTrustedUiUrl(frame->GetURL().ToString(), m_trustedUiIndexUrl))
	{
		callback->Failure(403, "Privileged bridge is restricted to the bundled UI.");
		return true;
	}

	nlohmann::json req;
	try
	{
		req = nlohmann::json::parse(request.ToString());
	}
	catch (const nlohmann::json::parse_error&)
	{
		callback->Failure(400, "Invalid JSON request");
		return true;
	}

	const std::string action = req.value("action", "");
	const nlohmann::json payload = req.value("payload", nlohmann::json::object());

	if (action == "getInitialState")
		HandleGetInitialState(browser, callback);
	else if (action == "selectSession")
		HandleSelectSession(browser, payload, callback);
	else if (action == "createSession")
		HandleCreateSession(browser, payload, callback);
	else if (action == "renameSession")
		HandleRenameSession(browser, payload, callback);
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
	else if (action == "stopAcpSession")
		HandleStopAcpSession(browser, payload, callback);
	else if (action == "setTheme")
		HandleSetTheme(browser, payload, callback);
	else
	{
		callback->Failure(404, "Unknown action: " + action);
	}

	return true;
}

void UamQueryHandler::OnQueryCanceled(CefRefPtr<CefBrowser> /*browser*/,
                                       CefRefPtr<CefFrame>   /*frame*/,
                                       int64_t               /*query_id*/)
{
	// Persistent queries are not used; nothing to cancel.
}

// ---------------------------------------------------------------------------
// Action handlers
// ---------------------------------------------------------------------------

void UamQueryHandler::HandleGetInitialState(CefRefPtr<CefBrowser>  /*browser*/,
                                             CefRefPtr<Callback>    cb)
{
	nlohmann::json state = uam::StateSerializer::Serialize(m_app);
	cb->Success(state.dump());
}

void UamQueryHandler::HandleSelectSession(CefRefPtr<CefBrowser>  browser,
                                           const nlohmann::json&  payload,
                                           CefRefPtr<Callback>    cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string previous_selected_chat_id = (ChatDomainService().SelectedChat(m_app) != nullptr) ? ChatDomainService().SelectedChat(m_app)->id : std::string{};
	ChatDomainService().SelectChatById(m_app, chat_id);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		cb->Failure(500, m_app.status_line.empty() ? "Failed to persist selected chat." : m_app.status_line);
		return;
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCreateSession(CefRefPtr<CefBrowser>  browser,
                                           const nlohmann::json&  payload,
                                           CefRefPtr<Callback>    cb)
{
	const std::string title      = payload.value("title",      "New Chat");
	const std::string requested_folder_id = payload.value("folderId", "");
	const ProviderProfile* preferred_provider = ResolvePreferredCliProvider(m_app);
	const std::string provider_id = payload.value(
		"providerId",
		preferred_provider != nullptr ? preferred_provider->id : m_app.settings.active_provider_id);
	const std::string previous_selected_chat_id = (ChatDomainService().SelectedChat(m_app) != nullptr) ? ChatDomainService().SelectedChat(m_app)->id : std::string{};

	const std::string target_folder_id = ResolveRequestedNewChatFolderId(m_app, requested_folder_id);

	ChatSession chat = ChatDomainService().CreateNewChat(target_folder_id, provider_id);
	EnsureChatUsesPreferredCliProvider(m_app, chat);
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

void UamQueryHandler::HandleRenameSession(CefRefPtr<CefBrowser>  browser,
                                           const nlohmann::json&  payload,
                                           CefRefPtr<Callback>    cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string title   = payload.value("title",   "");

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

void UamQueryHandler::HandleDeleteSession(CefRefPtr<CefBrowser>  browser,
                                           const nlohmann::json&  payload,
                                           CefRefPtr<Callback>    cb)
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

void UamQueryHandler::HandleCreateFolder(CefRefPtr<CefBrowser>  browser,
                                          const nlohmann::json&  payload,
                                          CefRefPtr<Callback>    cb)
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

void UamQueryHandler::HandleRenameFolder(CefRefPtr<CefBrowser>  browser,
                                          const nlohmann::json&  payload,
                                          CefRefPtr<Callback>    cb)
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

void UamQueryHandler::HandleDeleteFolder(CefRefPtr<CefBrowser>  browser,
                                          const nlohmann::json&  payload,
                                          CefRefPtr<Callback>    cb)
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

void UamQueryHandler::HandleToggleFolder(CefRefPtr<CefBrowser>  browser,
                                          const nlohmann::json&  payload,
                                          CefRefPtr<Callback>    cb)
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

void UamQueryHandler::HandleBrowseFolderDirectory(CefRefPtr<CefBrowser>  /*browser*/,
                                                   const nlohmann::json&  payload,
                                                   CefRefPtr<Callback>    cb)
{
	const std::string current_value = payload.value("currentValue", "");
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(current_value);

	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(
			PlatformPathBrowseTarget::Directory,
			initial_path,
			&selected_path,
			&error))
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

void UamQueryHandler::HandleStartCli(CefRefPtr<CefBrowser>  browser,
                                      const nlohmann::json&  payload,
                                      CefRefPtr<Callback>    cb)
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
	const std::string provider_before = chat.provider_id;
	EnsureChatUsesPreferredCliProvider(m_app, chat);
	if (chat.provider_id != provider_before)
	{
		uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "provider_override", nullptr, "chat_id=" + chat.id + ", from=" + provider_before + ", to=" + chat.provider_id);
	}

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

void UamQueryHandler::HandleStopCli(CefRefPtr<CefBrowser>  browser,
                                     const nlohmann::json&  payload,
                                     CefRefPtr<Callback>    cb)
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

void UamQueryHandler::HandleResizeCli(const nlohmann::json& payload,
                                       CefRefPtr<Callback>   cb)
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

void UamQueryHandler::HandleWriteCliInput(CefRefPtr<CefBrowser> browser,
                                           const nlohmann::json& payload,
                                           CefRefPtr<Callback>   cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	const std::string data    = payload.value("data",   "");

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

void UamQueryHandler::HandleSendAcpPrompt(CefRefPtr<CefBrowser> browser,
                                           const nlohmann::json& payload,
                                           CefRefPtr<Callback>   cb)
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

void UamQueryHandler::HandleCancelAcpTurn(CefRefPtr<CefBrowser> browser,
                                           const nlohmann::json& payload,
                                           CefRefPtr<Callback>   cb)
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

void UamQueryHandler::HandleResolveAcpPermission(CefRefPtr<CefBrowser> browser,
                                                  const nlohmann::json& payload,
                                                  CefRefPtr<Callback>   cb)
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

void UamQueryHandler::HandleStopAcpSession(CefRefPtr<CefBrowser> browser,
                                            const nlohmann::json& payload,
                                            CefRefPtr<Callback>   cb)
{
	const std::string chat_id = payload.value("chatId", "");
	uam::StopAcpSession(m_app, chat_id);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetTheme(CefRefPtr<CefBrowser>  browser,
                                      const nlohmann::json&  payload,
                                      CefRefPtr<Callback>    cb)
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
