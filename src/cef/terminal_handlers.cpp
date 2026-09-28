#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/provider_resolution_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"
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
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// CLI terminal handlers (start, stop, resize, write input)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	constexpr std::size_t kRecentOutputReplayLimitBytes = 256 * 1024;

	/// <summary>Continues a UI-owned terminal start after the existing remote stop poller settles.</summary>
	class PendingCliStartTask final : public CefTask
	{
	  public:
		PendingCliStartTask(std::weak_ptr<void> lifetime, std::function<bool(std::string_view)> resume)
		    : m_lifetime(std::move(lifetime)), m_resume(std::move(resume))
		{
		}

		void Execute() override
		{
			if (m_lifetime.expired() || !m_resume) return;
			CEF_REQUIRE_UI_THREAD();
			try
			{
				if (m_resume({})) m_resume = {};
				else if (!CefPostDelayedTask(TID_UI, this, 50))
				{
					(void)m_resume("The terminal start queue is unavailable. Retry opening the terminal.");
					m_resume = {};
				}
			}
			catch (const std::exception& error)
			{
				(void)m_resume(error.what());
				m_resume = {};
			}
		}

	  private:
		std::weak_ptr<void> m_lifetime;
		std::function<bool(std::string_view)> m_resume;
		IMPLEMENT_REFCOUNTING(PendingCliStartTask);
	};

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
		data["turnState"] = uam::CliTurnStateLabel(terminal);
		data["lastError"] = terminal.last_error;
		data["pendingSteer"] = !terminal.pending_steer_prompt.empty();

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
			terminal.ui_attachment_id = payload.value("attachmentId", "");
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
	terminal.ui_attachment_id = payload.value("attachmentId", "");
	terminal.rows = uam::ClampCliTerminalLaunchRows(rows);
	terminal.cols = uam::ClampCliTerminalLaunchCols(cols);
	if (terminal.terminal_id.empty())
	{
		terminal.terminal_id = "term-" + chat->id;
	}
	uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "terminal_prepared", &terminal);

	if (!ProviderResolutionService().ChatProviderIsAvailable(m_app, *chat))
	{
		uam::StopCliTerminal(terminal);
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
	if (terminal.native_session_setup_cancel != nullptr)
	{
		// Reattachments share the outstanding creation; stale detaches carry a different attachment ID.
		cb->Success(BuildCliBindingResponse(terminal).dump());
		return;
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *chat);
	if (!chat->imported_read_only && uam::ProviderSupportsInteractiveTerminal(provider) &&
	    ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, m_app.settings).empty())
	{
		std::string handoff_error;
		if (!uam::PrepareAcpSessionForCliTerminalLaunch(m_app, *chat, &handoff_error))
		{
			if (!handoff_error.empty())
			{
				terminal.last_error = handoff_error;
				uam::PushStateUpdateIfChanged(browser, m_app);
				cb->Success(BuildCliBindingResponse(terminal).dump());
				return;
			}
			const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id);
			const std::optional<ExecutionHost> launch_host = host != nullptr ? std::optional<ExecutionHost>(*host) : std::nullopt;
			const std::string provider_id = chat->provider_id;
			const std::string host_id = chat->execution_host_id;
			const std::string native_id = chat->native_session_id;
			const std::filesystem::path workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
			const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(provider, *chat, m_app.settings);
			const double deadline = uam::GetAppTimeSeconds() + 5.0;
			const std::shared_ptr<std::stop_source> request = std::make_shared<std::stop_source>();
			terminal.native_session_setup_cancel = request;
			terminal.last_error.clear();
			CefRefPtr<PendingCliStartTask> task = new PendingCliStartTask(m_asyncLifetime,
			    [this, browser, payload, cb, request, chat_id, provider_id, host_id, native_id, workspace, argv, launch_host, deadline](std::string_view task_error)
			    {
				    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
				    uam::CliTerminalState* target = uam::FindCliTerminalForChat(m_app, chat_id);
				    if (target == nullptr || target->native_session_setup_cancel != request)
				    {
					    cb->Failure(409, "Terminal start was canceled.");
					    return true;
				    }
				    if (request->stop_requested() || !target->ui_attached)
				    {
					    target->native_session_setup_cancel.reset();
					    cb->Failure(409, "Terminal start was canceled.");
					    return true;
				    }
				    std::string error(task_error);
				    const ExecutionHost* current_host = uam::execution_hosts::Find(m_app.settings.execution_hosts, host_id);
				    if (current == nullptr || current->imported_read_only || target->running ||
				        current->provider_id != provider_id || current->execution_host_id != host_id ||
				        current->native_session_id != native_id ||
				        uam::paths::ResolveWorkspaceRootPath(m_app, *current) != workspace ||
				        ProviderRuntime::BuildInteractiveArgv(ProviderResolutionService().ProviderForChatOrDefault(m_app, *current), *current, m_app.settings) != argv || !launch_host ||
				        current_host == nullptr || !uam::execution_hosts::SameConnection(*current_host, *launch_host))
					    error = "The chat or remote host changed. Retry opening its terminal.";
				    if (error.empty() && uam::GetAppTimeSeconds() >= deadline)
					    error = "The remote stop timed out. Retry opening the terminal.";
				    if (error.empty() && !uam::PrepareAcpSessionForCliTerminalLaunch(m_app, *current, &error) && error.empty())
					    return false;
				    target->native_session_setup_cancel.reset();
				    if (!error.empty())
				    {
					    target->last_error = error;
					    uam::PushStateUpdateIfChanged(browser, m_app);
					    cb->Success(BuildCliBindingResponse(*target).dump());
					    return true;
				    }
				    nlohmann::json resumed_payload = payload;
				    resumed_payload["attachmentId"] = target->ui_attachment_id;
				    resumed_payload["rows"] = target->rows;
				    resumed_payload["cols"] = target->cols;
				    HandleStartCli(browser, resumed_payload, cb);
				    return true;
			    });
			uam::PushStateUpdateIfChanged(browser, m_app);
			task->Execute();
			return;
		}
	}
	if (!chat->imported_read_only &&
	    ProviderRuntimeRegistry::Resolve(provider).RequiresNativeSessionCreation() &&
	    uam::ProviderSupportsInteractiveTerminal(provider) &&
	    ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, m_app.settings).empty() &&
	    uam::ResolveProviderInteractiveResumeId(m_app, *chat, provider).empty())
	{
		std::optional<ExecutionHost> remote_host;
		if (!uam::paths::IsControllerLocalWorkspace(*chat))
		{
			const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id);
			if (host == nullptr || host->transport != "ssh" || host->runner_status != "ready" ||
			    host->runner_protocol_version < 3)
			{
				terminal.last_error = "Native session creation needs a ready, current remote runner. Recheck it in Settings.";
				cb->Success(BuildCliBindingResponse(terminal).dump());
				return;
			}
			remote_host = *host;
		}
		std::string handoff_error;
		if (!uam::PrepareAcpSessionForCliTerminalLaunch(m_app, *chat, &handoff_error))
		{
			terminal.last_error = handoff_error;
			cb->Success(BuildCliBindingResponse(terminal).dump());
			return;
		}
		const std::filesystem::path workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
		const std::string provider_id = chat->provider_id;
		const std::string host_id = chat->execution_host_id;
		const std::string native_id_before = chat->native_session_id;
		const std::string interactive_command = provider.interactive_command;
		const std::shared_ptr<std::stop_source> request = std::make_shared<std::stop_source>();
		terminal.native_session_setup_cancel = request;
		terminal.last_error.clear();
		using namespace uam::query_handler_async;
		if (!CefPostTask(TID_FILE_BACKGROUND, new CefQueryWorkerTask(m_asyncLifetime, cb,
		    [workspace, provider, request, remote_host]()
		    {
			    std::string error;
			    const std::string native_id = ProviderRuntimeRegistry::Resolve(provider).CreateNativeSession(
			        provider, workspace, request->get_token(), &error,
			        remote_host ? &*remote_host : nullptr);
			    return native_id.empty() ? AsyncFailure(502, error)
			                             : AsyncSuccess({{"nativeSessionId", native_id}});
		    },
		    [this, browser, chat_id, provider_id, host_id, native_id_before, workspace, interactive_command, request, remote_host](AsyncCefResult& response)
		    {
			    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
			    uam::CliTerminalState* target = uam::FindCliTerminalForChat(m_app, chat_id);
			    if (target == nullptr || target->native_session_setup_cancel != request)
			    {
				    response = AsyncFailure(409, "Terminal start was canceled.");
				    return;
			    }
			    target->native_session_setup_cancel.reset();
			    if (request->stop_requested() || current == nullptr || current->imported_read_only ||
			        current->provider_id != provider_id || current->execution_host_id != host_id ||
			        current->native_session_id != native_id_before || target->running ||
			        ProviderResolutionService().ProviderForChatOrDefault(m_app, *current).interactive_command != interactive_command ||
			        uam::paths::ResolveWorkspaceRootPath(m_app, *current) != workspace)
			    {
				    response = AsyncFailure(409, "The chat changed while the provider was starting. Retry opening its terminal.");
			    }
			    if (response.ok && remote_host)
			    {
				    const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, remote_host->id);
				    if (host == nullptr || host->transport != remote_host->transport ||
				        host->ssh_alias != remote_host->ssh_alias || host->platform != remote_host->platform ||
				        host->runner_directory != remote_host->runner_directory ||
				        host->runner_version != remote_host->runner_version ||
				        host->runner_protocol_version != remote_host->runner_protocol_version)
				    {
					    response = AsyncFailure(409, "Remote host changed while the provider was starting. Retry opening its terminal.");
				    }
			    }
			    if (response.ok)
			    {
				    std::string handoff_error;
				    const ProviderProfile& current_provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *current);
				    if (!uam::ResolveProviderInteractiveResumeId(m_app, *current, current_provider).empty())
					    response = AsyncFailure(409, "The chat already has a native session. Retry opening its terminal.");
				    else if (!uam::PrepareAcpSessionForCliTerminalLaunch(m_app, *current, &handoff_error))
					    response = AsyncFailure(409, handoff_error);
			    }
			    if (response.ok)
			    {
				    const std::string native_id = nlohmann::json::parse(response.body).at("nativeSessionId").get<std::string>();
				    current->native_session_id = native_id;
				    m_app.resolved_native_sessions_by_chat_id[chat_id] = native_id;
				    // Preserve the ID even after detachment, and retry this same save after storage failure.
				    if (!ChatRepository::SaveChat(m_app.data_root, *current))
				    {
					    m_app.pending_chat_save_at_by_chat_id[chat_id] = uam::GetAppTimeSeconds() + 1.0;
					    response = AsyncFailure(500, "Could not save the native session. Retry when storage is available.");
				    }
				    else if (target->ui_attached)
				    {
					    if (!uam::StartCliTerminalForChat(m_app, *target, *current, target->rows, target->cols))
						    response = AsyncFailure(500, target->last_error);
				    }
			    }
			    if (!response.ok)
			    {
				    target->last_error = response.error;
				    uam::LogCliDiagnosticEvent(m_app, "handle_start_cli", "native_session_setup_failed", target, response.error);
			    }
			    uam::PushStateUpdateIfChanged(browser, m_app);
			    response = AsyncSuccess(BuildCliBindingResponse(*target));
		    })))
		{
			terminal.native_session_setup_cancel.reset();
			terminal.last_error = "The background task queue is unavailable.";
			cb->Success(BuildCliBindingResponse(terminal).dump());
		}
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

	const bool quit = payload.value("quit", false);
	if (!quit && !uam::DetachCliTerminalUi(*term, payload.value("attachmentId", "")))
	{
		cb->Success("{}");
		return;
	}
	term->ui_attached = false;
	if (quit && term->native_session_setup_cancel != nullptr)
	{
		uam::StopCliTerminal(*term);
	}
	if (quit && term->running)
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

	if (uam::CliTerminalState* term = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id); term != nullptr)
	{
		term->rows = rows;
		term->cols = cols;
		if (term->running) PlatformServicesFactory::Instance().terminal_runtime.ResizeCliTerminal(*term);
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
			if (!wrote)
			{
				uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", "pty_write_failed", term, "", static_cast<long long>(data.size()));
				m_app.status_line = term->last_error;
				uam::PushStateUpdateIfChanged(browser, m_app);
				cb->Failure(500, term->last_error);
				return;
			}
			if (CliInputLooksLikeTurnSubmit(data))
			{
				uam::MarkCliTerminalTurnBusy(*term);
				uam::LogCliDiagnosticEvent(m_app, "handle_write_cli_input", "turn_submitted", term);
				uam::PushStateUpdateIfChanged(browser, m_app);
			}
		}
		else
		{
			cb->Failure(409, "Provider terminal is not running. Restart it to send input.");
			return;
		}
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleSteerCliTerminal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string terminal_id = payload.value("terminalId", "");
	uam::CliTerminalState* terminal = FindCliTerminalByRoutingKey(m_app, chat_id, terminal_id);
	if (terminal == nullptr)
	{
		cb->Failure(404, "Terminal fallback session was not found.");
		return;
	}
	std::string error;
	if (!uam::RequestCliTerminalSteer(*terminal, payload.value("text", ""), payload.value("retry", false), &error))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to steer terminal fallback."));
		return;
	}
	uam::LogCliDiagnosticEvent(m_app, "handle_steer_cli_terminal", "steer_requested", terminal);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(BuildCliBindingResponse(*terminal).dump());
}
