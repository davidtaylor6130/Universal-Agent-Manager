#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/provider_resolution_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_profile.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/utils/base64.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// CLI terminal handlers (start, stop, resize, write input)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	constexpr std::size_t kRecentOutputReplayLimitBytes = 256 * 1024;

	uam::CliTerminalState* FindCliTerminalByRoutingKey(uam::AppState& app, const std::string& chat_id, const std::string& terminal_id)
	{
		return uam::FindCliTerminalForRoutingKey(app, chat_id, terminal_id);
	}

	bool CliInputLooksLikeTurnSubmit(const std::string& data)
	{
		return uam::strings::Contains(data, '\r') || uam::strings::Contains(data, '\n');
	}

	nlohmann::json BuildCliBindingResponse(const uam::CliTerminalState& terminal)
	{
		nlohmann::json data;
		data["terminalId"] = terminal.terminal_id;
		data["sessionId"] = terminal.frontend_chat_id;
		data["sourceChatId"] = uam::CliTerminalPrimaryChatId(terminal);
		data["running"] = terminal.running;
		data["lifecycleState"] = uam::CliTerminalLifecycleStateLabel(terminal);
		data["turnState"] = uam::CliTerminalLifecycleIsProcessing(terminal) ? "busy" : "idle";
		data["lastError"] = terminal.last_error;

		if (!terminal.recent_output_bytes.empty())
		{
			const std::size_t start_offset = terminal.recent_output_bytes.size() > kRecentOutputReplayLimitBytes ? terminal.recent_output_bytes.size() - kRecentOutputReplayLimitBytes : 0;
			data["replayData"] = uam::base64::Encode(terminal.recent_output_bytes.substr(start_offset));
		}

		return data;
	}
} // namespace

void UamQueryHandler::HandleStartCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const int rows = payload.value("rows", uam::kCliTerminalDefaultRows);
	const int cols = payload.value("cols", uam::kCliTerminalDefaultCols);
	const std::string terminal_id = payload.value("terminalId", "");
	uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "request_received", nullptr, "chat_id=" + chat_id + ", terminal_id=" + terminal_id);

	if (uam::CliTerminalState* existing = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); existing != nullptr && existing->running)
	{
		if (existing->lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown)
		{
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "restart_shutting_down_terminal", existing);
			uam::StopCliTerminal(*existing, false, uam::CliTerminalStopMode::FastExit);
		}
		else
		{
			ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
			if (chat == nullptr)
			{
				return;
			}

			const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *chat);
			uam::RepairCliTerminalIdentityForChat(m_app, *existing, *chat, provider);
			uam::CliTerminalState& terminal = *existing;
			terminal.ui_attached = true;
			terminal.rows = uam::ClampCliTerminalResizeRows(rows);
			terminal.cols = uam::ClampCliTerminalResizeCols(cols);
			PlatformServicesFactory::Instance().terminal_runtime.ResizeCliTerminal(terminal);
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "reused_running_terminal", &terminal);
			cb->Success(BuildCliBindingResponse(terminal).dump());
			return;
		}
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(m_app.data_root, *chat, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load chat messages."));
		return;
	}
	uam::CliTerminalState& terminal = uam::EnsureCliTerminalForChat(m_app, *chat);
	terminal.frontend_chat_id = chat->id;
	terminal.ui_attached = true;
	if (terminal.terminal_id.empty())
	{
		terminal.terminal_id = "term-" + chat->id;
	}
	uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "terminal_prepared", &terminal);

	if (!ProviderResolutionService().ChatProviderIsAvailable(m_app, *chat))
	{
		terminal.running = false;
		terminal.generation_in_progress = false;
		terminal.turn_state = uam::CliTerminalTurnState::Idle;
		terminal.should_launch = false;
		terminal.last_error = ProviderResolutionService().ChatProviderUnavailableReason(m_app, *chat);
		terminal.lifecycle_state = uam::CliTerminalLifecycleState::Stopped;
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(BuildCliBindingResponse(terminal).dump());
		return;
	}

	if (!terminal.running)
	{
		if (!uam::StartCliTerminalForChat(m_app, terminal, *chat, rows, cols))
		{
			uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "start_failed", &terminal, terminal.last_error);
			cb->Success(BuildCliBindingResponse(terminal).dump());
			return;
		}

		uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "started_terminal", &terminal);
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
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
	if (payload.value("quit", false) && term->running)
	{
		uam::BeginCliTerminalIdleShutdown(*term);
		uam::LogCliDiagnosticEvent(m_app, "handle_stop_cli", "quit_requested", term);
	}
	else
	{
		uam::LogCliDiagnosticEvent(m_app, "handle_stop_cli", "ui_detached", term);
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResizeCli(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	const int rows = uam::ClampCliTerminalResizeRows(payload.value("rows", uam::kCliTerminalDefaultRows));
	const int cols = uam::ClampCliTerminalResizeCols(payload.value("cols", uam::kCliTerminalDefaultCols));

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
			const bool wrote = uam::WriteToCliTerminal(*term, data.c_str(), data.size());
			uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", wrote ? "pty_write_ok" : "pty_write_failed", term, "", static_cast<long long>(data.size()));
			if (wrote && CliInputLooksLikeTurnSubmit(data))
			{
				uam::MarkCliTerminalTurnBusy(*term);
				uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", "turn_marked_busy_from_submit", term);
				uam::PushStateUpdateIfChanged(browser, m_app);
			}
		}
	}

	cb->Success("{}");
}
