#include "cef/uam_query_handler.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/runtime_orchestration_services.h"

#include "common/platform/platform_services.h"
#include "common/runtime/terminal/terminal_provider_cli.h"

#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>
#include <algorithm>

namespace
{
	constexpr const char* kPreferredProviderId = "gemini-cli";
	constexpr std::size_t kRecentOutputReplayLimitBytes = 256 * 1024;

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
				if (term && (term->frontend_chat_id == chat_id || term->attached_chat_id == chat_id))
				{
					return term.get();
				}
			}
		}

		return nullptr;
	}

	nlohmann::json BuildCliBindingResponse(const uam::CliTerminalState& terminal)
	{
		nlohmann::json data;
		data["terminalId"] = terminal.terminal_id;
		data["sessionId"] = terminal.frontend_chat_id;
		data["sourceChatId"] = terminal.attached_chat_id;
		data["running"] = terminal.running;
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

UamQueryHandler::UamQueryHandler(uam::AppState& app)
	: m_app(app)
{}

// ---------------------------------------------------------------------------
// CefMessageRouterBrowserSide::Handler
// ---------------------------------------------------------------------------

bool UamQueryHandler::OnQuery(CefRefPtr<CefBrowser>  browser,
                               CefRefPtr<CefFrame>    /*frame*/,
                               int64_t                /*query_id*/,
                               const CefString&       request,
                               bool                   /*persistent*/,
                               CefRefPtr<Callback>    callback)
{
	CEF_REQUIRE_UI_THREAD();

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
	else if (action == "sendMessage")
		HandleSendMessage(browser, payload, callback);
	else if (action == "createFolder")
		HandleCreateFolder(browser, payload, callback);
	else if (action == "toggleFolder")
		HandleToggleFolder(browser, payload, callback);
	else if (action == "startCliTerminal")
		HandleStartCli(browser, payload, callback);
	else if (action == "resizeCliTerminal")
		HandleResizeCli(payload, callback);
	else if (action == "writeCliInput")
		HandleWriteCliInput(payload, callback);
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
	ChatDomainService().SelectChatById(m_app, chat_id);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCreateSession(CefRefPtr<CefBrowser>  browser,
                                           const nlohmann::json&  payload,
                                           CefRefPtr<Callback>    cb)
{
	const std::string title      = payload.value("title",      "New Chat");
	const std::string folder_id  = payload.value("folderId",   "");
	const ProviderProfile* preferred_provider = ResolvePreferredCliProvider(m_app);
	const std::string provider_id = payload.value(
		"providerId",
		preferred_provider != nullptr ? preferred_provider->id : m_app.settings.active_provider_id);

	ChatSession chat = ChatDomainService().CreateNewChat(folder_id, provider_id);
	EnsureChatUsesPreferredCliProvider(m_app, chat);
	if (!title.empty())
		chat.title = title;

	m_app.chats.push_back(std::move(chat));

	// Select the new chat
	ChatDomainService().SelectChatById(m_app, m_app.chats.back().id);

	// Persist
	ChatHistorySyncService sync;
	sync.SaveChatWithStatus(m_app, m_app.chats.back(), "", "");

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
	ChatHistorySyncService().RenameChat(m_app, chat, title);

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

	m_app.chats.erase(m_app.chats.begin() + idx);

	// Adjust selection
	if (m_app.selected_chat_index >= static_cast<int>(m_app.chats.size()))
		m_app.selected_chat_index = static_cast<int>(m_app.chats.size()) - 1;

	PersistenceCoordinator().SaveSettings(m_app);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSendMessage(CefRefPtr<CefBrowser>  browser,
                                         const nlohmann::json&  payload,
                                         CefRefPtr<Callback>    cb)
{
	const std::string chat_id = payload.value("chatId",  "");
	const std::string content = payload.value("content", "");

	const int idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(idx)];
	EnsureChatUsesPreferredCliProvider(m_app, chat);

	// Add the user message and queue the provider call.
	// ProviderRequestService will drive the async generation loop; stream tokens
	// are delivered to the frontend via PushStreamToken() / PushStreamDone()
	// from the polling path in Application::Update().
	ProviderRequestService().QueuePromptForChat(m_app, chat, content);

	// Immediately push state so the user message appears in the UI.
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleCreateFolder(CefRefPtr<CefBrowser>  browser,
                                          const nlohmann::json&  payload,
                                          CefRefPtr<Callback>    cb)
{
	const std::string title = payload.value("title", "New Folder");

	ChatFolder folder;
	folder.id        = ChatDomainService().NewFolderId();
	folder.title     = title;
	folder.collapsed = false;
	m_app.folders.push_back(std::move(folder));

	PersistenceCoordinator().SaveSettings(m_app);
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

	PersistenceCoordinator().SaveSettings(m_app);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleStartCli(CefRefPtr<CefBrowser>  browser,
                                      const nlohmann::json&  payload,
                                      CefRefPtr<Callback>    cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const int rows = payload.value("rows", 24);
	const int cols = payload.value("cols", 80);
	const std::string terminal_id = payload.value("terminalId", "");

	// If a terminal for this chat is already running, nothing to do.
	if (uam::CliTerminalState* existing = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); existing != nullptr && existing->running)
	{
		cb->Success(BuildCliBindingResponse(*existing).dump());
		return;
	}

	const int chat_idx = ChatDomainService().FindChatIndexById(m_app, chat_id);
	if (chat_idx < 0)
	{
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	}

	ChatSession& chat = m_app.chats[static_cast<std::size_t>(chat_idx)];
	EnsureChatUsesPreferredCliProvider(m_app, chat);
	uam::CliTerminalState& terminal = EnsureCliTerminalForChat(m_app, chat);
	terminal.frontend_chat_id = chat.id;
	if (terminal.terminal_id.empty())
	{
		terminal.terminal_id = "term-" + chat.id;
	}

	if (!terminal.running)
	{
		if (!StartCliTerminalForChat(m_app, terminal, chat, rows, cols))
		{
			cb->Success(BuildCliBindingResponse(terminal).dump());
			return;
		}
	}

	uam::PushStateUpdate(browser, m_app);
	cb->Success(BuildCliBindingResponse(terminal).dump());
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

void UamQueryHandler::HandleWriteCliInput(const nlohmann::json& payload,
                                           CefRefPtr<Callback>   cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	const std::string data    = payload.value("data",   "");

	if (!data.empty())
	{
		if (uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); term != nullptr && term->running)
		{
			// Write raw PTY bytes directly to the terminal master fd.
			// xterm.js sends individual keystrokes and escape sequences that
			// must reach the child process unmodified — do NOT queue these as
			// structured prompts (which wrap them in bracketed-paste sequences
			// and append \r, breaking all interactive CLI communication).
			PlatformServicesFactory::Instance().terminal_runtime.WriteToCliTerminal(
				*term, data.c_str(), data.size());
		}
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleSetTheme(CefRefPtr<CefBrowser>  browser,
                                      const nlohmann::json&  payload,
                                      CefRefPtr<Callback>    cb)
{
	const std::string theme = payload.value("theme", "dark");
	m_app.settings.ui_theme = theme;
	PersistenceCoordinator().SaveSettings(m_app);
	uam::PushStateUpdate(browser, m_app);
	cb->Success("{}");
}
