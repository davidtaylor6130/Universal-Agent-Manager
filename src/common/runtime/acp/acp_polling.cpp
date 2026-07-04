#include "common/runtime/acp/acp_polling.h"
#include "common/runtime/acp/acp_claude_message_handlers.h"
#include "common/runtime/acp/acp_codex_message_handlers.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_update_handler.h"

#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <cstring>

#include <array>
#include <sstream>
#include <string>

namespace uam::acp_detail
{

bool ProcessAcpLine(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line, CefRefPtr<CefBrowser> browser)
{
	const std::string trimmed = uam::strings::Trim(line);
	if (trimmed.empty())
	{
		return false;
	}

	nlohmann::json message;
	try
	{
		message = nlohmann::json::parse(trimmed);
	}
	catch (const std::exception& ex)
	{
		const std::string error_message = std::string("Invalid JSON from ") + RuntimeDisplayName(session) + ": " + ex.what();
		AppendAcpDiagnostic(session, "parse", "invalid_json", "", "", false, 0, error_message, CapDiagnosticString(trimmed, kMaxAcpDiagnosticDetailBytes));
		FailAcpTurnOrSession(session, error_message);
		MarkAcpChatUnseenIfBackground(app, chat);
		return true;
	}

	{
		const IProviderRuntime& poll_runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
		const char* protocol_kind = poll_runtime.AcpProtocolKind();
		if (std::strcmp(protocol_kind, "claude-code-stream-json") == 0)
		{
			try
			{
				HandleClaudeMessage(app, session, chat, message, browser);
			}
			catch (const std::exception& ex)
			{
				const std::string error_message = std::string("Claude stream-json message handling failed: ") + ex.what();
				AppendAcpDiagnostic(session, "parse", "claude_message_parse_error", "", "", false, 0, error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
				FailAcpTurnOrSession(session, error_message);
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			return true;
		}

		if (uam::nlohmann_json::FindField(message, "method") != nullptr)
		{
			const std::string method = JsonDiagnosticStringValue(message, "method");
			if (std::strcmp(protocol_kind, "codex-app-server") == 0)
			{
				try
				{
					HandleCodexMessage(app, session, chat, message, browser);
				}
				catch (const std::exception& ex)
				{
					const std::string error_message = std::string("Codex app-server message handling failed: ") + ex.what();
					AppendAcpDiagnostic(session, "parse", "codex_message_parse_error", method, "", false, 0, error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
					FailAcpTurnOrSession(session, error_message);
					MarkAcpChatUnseenIfBackground(app, chat);
				}
			}
			else if (method == uam::acp_methods::kSessionUpdate)
			{
				HandleSessionUpdate(app, session, chat, JsonObjectValue(message, "params"), browser);
			}
			else
			{
				HandleAcpRequest(app, session, chat, message);
			}
			return true;
		}

		if (uam::nlohmann_json::FindField(message, "id") != nullptr)
		{
			HandleAcpResponse(app, session, chat, message);
			return true;
		}

		AppendAcpDiagnostic(session, "message", "ignored_without_method_or_id", "", "", false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		return false;
	}
}

bool DrainStdout(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
{
	bool changed = false;
	std::array<char, 8192> buffer{};
	while (true)
	{
		std::string read_error;
		const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(session, buffer.data(), buffer.size(), &read_error);
		if (read_bytes > 0)
		{
			changed = MarkAcpRuntimeActivity(session) || changed;
			session.stdout_buffer.append(buffer.data(), static_cast<std::size_t>(read_bytes));
			std::size_t newline_pos = std::string::npos;
			while ((newline_pos = session.stdout_buffer.find('\n')) != std::string::npos)
			{
				std::string line = session.stdout_buffer.substr(0, newline_pos);
				session.stdout_buffer.erase(0, newline_pos + 1);
				changed = ProcessAcpLine(app, session, chat, line, browser) || changed;
			}
			continue;
		}

		if (read_bytes == -2)
		{
			break;
		}

		if (read_bytes == 0)
		{
			break;
		}

		const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout: " + read_error);
		AppendAcpDiagnostic(session, "read", "stdout_read_failed", "", "", false, 0, message);
		FailAcpTurnOrSession(session, message);
		MarkAcpChatUnseenIfBackground(app, chat);
		changed = true;
		break;
	}
	return changed;
}

bool DrainStderr(AcpSessionState& session)
{
	bool changed = false;
	std::array<char, 4096> buffer{};
	while (true)
	{
		std::string read_error;
		const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStderr(session, buffer.data(), buffer.size(), &read_error);
		if (read_bytes > 0)
		{
			changed = MarkAcpRuntimeActivity(session) || changed;
			AppendRecentStderr(session, std::string(buffer.data(), static_cast<std::size_t>(read_bytes)));
			changed = true;
			continue;
		}

		if (read_bytes == -2 || read_bytes == 0)
		{
			break;
		}

		const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr: " + read_error);
		AppendAcpDiagnostic(session, "read", "stderr_read_failed", "", "", false, 0, message);
		changed = true;
		break;
	}
	return changed;
}

void MarkAcpProcessExited(AcpSessionState& session, bool has_exit_code, int exit_code)
{
	if (has_exit_code)
	{
		session.has_last_exit_code = true;
		session.last_exit_code = exit_code;
	}
	const bool active_turn = uam::AcpSessionHasActiveTurn(session);
	std::ostringstream detail;
	bool has_detail = false;
	if (has_exit_code)
	{
		detail << "exit_code=" << exit_code;
		has_detail = true;
	}
	if (!session.recent_stderr.empty())
	{
		if (has_detail)
		{
			detail << "\n";
		}
		detail << "stderr_tail=" << RecentStderrTail(session);
		has_detail = true;
	}
	const std::string pending_summary = PendingRequestSummary(session);
	if (!pending_summary.empty())
	{
		if (has_detail)
		{
			detail << "\n";
		}
		detail << "pending_requests=" << pending_summary;
	}
	AppendAcpDiagnostic(session, "process_exit", active_turn ? "active_turn" : "idle", "", "", has_exit_code, exit_code, "", detail.str());
	session.running = false;
	session.initialized = false;
	session.session_ready = false;
	if (active_turn)
	{
		const std::string message = uam::strings::NonEmptyOrFallback(session.last_error, std::string(RuntimeDisplayName(session)) + " process exited during an active turn.");
		FailAcpTurnOrSession(session, message);
	}
	else
	{
		session.lifecycle_state = kAcpLifecycleStopped;
	}
	session.processing = false;
	session.prompt_request_id = 0;
	session.cancel_request_id = 0;
	session.current_assistant_message_index = -1;
	session.pending_assistant_thoughts.clear();
	ResetAcpPendingInteractionState(session);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
}

} // namespace uam::acp_detail
