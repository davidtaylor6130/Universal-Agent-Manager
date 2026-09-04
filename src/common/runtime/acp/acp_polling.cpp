#include "common/runtime/acp/acp_polling.h"
#include "common/runtime/acp/acp_claude_message_handlers.h"
#include "common/runtime/acp/acp_codex_message_handlers.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_update_handler.h"

#include "app/chat_domain_service.h"
#include "app/goal_service.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/config/settings_normalization.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "remote/runner_proxy.h"

#include <charconv>
#include <cstring>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <limits>
#include <sstream>
#include <string>

namespace uam::acp_detail
{
	namespace
	{
		using PollClock = std::chrono::steady_clock;
		constexpr auto kAcpStdoutTimePerPoll = std::chrono::milliseconds(4);
		constexpr auto kAcpStderrTimePerPoll = std::chrono::milliseconds(2);
		constexpr std::size_t kAcpStderrChunksPerPoll = 16;
		constexpr std::uint64_t kBytesPerMiB = 1024ull * 1024;
		constexpr double kFatalTransportReconnectDelaySeconds = 0.25;
		constexpr std::string_view kRemoteAttachedMethod = "uam/remoteAttached";

		void ReactivateConfirmedRemoteGoal(AppState& app, const ChatSession& chat)
		{
			if (chat.goal_iteration_goal_id.empty()) return;
			const std::string owner_chat_id = uam::strings::NonEmptyOrFallback(
			    chat.goal_owner_chat_id, chat.id);
			ChatSession* owner = ChatDomainService().FindChatById(app, owner_chat_id);
			if (owner == nullptr) return;

			Goal* matched = nullptr;
			for (Goal& goal : owner->goals)
			{
				if (goal.status != GoalStatus::Blocked ||
				    !goal.last_blocker.ends_with(" process exited during an active turn."))
					continue;
				if (goal.id != chat.goal_iteration_goal_id)
					continue;
				if (matched != nullptr) return;
				matched = &goal;
			}
			if (matched != nullptr && GoalService::SetActiveGoal(app, owner_chat_id, matched->id))
			{
				SaveChatQuietly(app, *owner);
			}
		}

		void InvalidateAcpTransportImpl(
		    AppState& app,
		    AcpSessionState& session,
		    ChatSession& chat,
		    const std::string& message,
		    bool recover_remote_disconnect = false)
		{
			const bool remote_helper_owned =
			    chat.execution_host_id != uam::execution_hosts::kLocalHostId &&
			    (chat.remote_process_exists || chat.remote_turn_reconnect_pending);
			if (recover_remote_disconnect && remote_helper_owned)
			{
				const bool active_turn = uam::AcpSessionHasActiveTurn(session);
				if (session.running) QueueAcpProcessStop(app, session);
				MarkAcpProcessExited(session, &chat, false, 0, active_turn);
				session.processing = active_turn;
				session.recovering_remote_turn = active_turn && chat.remote_turn_reconnect_pending;
				session.recovering_remote_process = true;
				session.reconnect_pending = true;
				session.reconnect_not_before_time_s =
				    GetAppTimeSeconds() + kFatalTransportReconnectDelaySeconds;
				session.last_error = message;
				session.lifecycle_state = kAcpLifecycleStarting;
				AppendAcpDiagnostic(session, "reconnect", "remote_transport_recovery_scheduled",
				                    "", "", false, 0, message);
				MarkAcpChatUnseenIfBackground(app, chat);
				SaveChatQuietly(app, chat);
				return;
			}
			const bool undelivered_prompt = session.prompt_request_id == 0 && !session.queued_prompt.empty();
			std::string queued_prompt = undelivered_prompt ? session.queued_prompt : std::string{};
			std::deque<AcpQueuedUserPromptState> queued_prompts = std::move(session.queued_user_prompts);
			(void)FinalizeActiveAcpToolCallsAsFailed(chat, session);
			FailAcpTurnOrSession(session, &chat, message);
			bool restart_safe = true;
			if (session.running)
			{
				restart_safe = StopAcpProcessForRestart(app, session, chat);
			}
			if (session.restart_marker_save_pending)
			{
				session.queued_user_prompts = std::move(queued_prompts);
				if (undelivered_prompt)
				{
					session.queued_prompt = std::move(queued_prompt);
					session.processing = true;
				}
				session.last_error = message;
				session.lifecycle_state = kAcpLifecycleError;
				MarkAcpChatUnseenIfBackground(app, chat);
				return;
			}
			session.running = false;
			ResetAcpRuntimeState(session);
			session.queued_user_prompts = std::move(queued_prompts);
			if (undelivered_prompt)
			{
				session.queued_prompt = std::move(queued_prompt);
				session.processing = true;
			}
			session.last_error = message;
			session.lifecycle_state = kAcpLifecycleError;
			if ((undelivered_prompt || !session.queued_user_prompts.empty()) && session.managed_agent_run_id.empty() &&
			    restart_safe)
			{
				session.reconnect_pending = true;
				session.reconnect_not_before_time_s = GetAppTimeSeconds() + kFatalTransportReconnectDelaySeconds;
				AppendAcpDiagnostic(session, "reconnect", "scheduled_after_transport_failure", "", "", false, 0,
				                    "Queued prompts will resume on a fresh structured transport.");
			}
			MarkAcpChatUnseenIfBackground(app, chat);
			SaveChatQuietly(app, chat);
		}

		bool StopForAcpOutputSafety(
		    AppState& app,
		    AcpSessionState& session,
		    ChatSession& chat,
		    const std::string& reason,
		    const std::string& message)
		{
			AppendAcpDiagnostic(session, "read", reason, "", "", false, 0, message);
			InvalidateAcpTransportImpl(app, session, chat, message);
			return true;
		}

		bool TurnOutputFlooded(AcpSessionState& session, std::uint64_t bytes)
		{
			const std::int64_t now_second = std::chrono::duration_cast<std::chrono::seconds>(
			    PollClock::now().time_since_epoch()).count();
			if (session.turn_output_latest_second < 0 || now_second < session.turn_output_latest_second ||
			    now_second - session.turn_output_latest_second >=
			        static_cast<std::int64_t>(session.turn_output_bytes_per_second.size()))
			{
				session.turn_output_bytes_per_second.fill(0);
			}
			else
			{
				for (std::int64_t second = session.turn_output_latest_second + 1;
				     second <= now_second; ++second)
				{
					session.turn_output_bytes_per_second[static_cast<std::size_t>(second) %
					                                     session.turn_output_bytes_per_second.size()] = 0;
				}
			}
			session.turn_output_latest_second = now_second;
			session.turn_output_bytes_per_second[static_cast<std::size_t>(now_second) %
			                                     session.turn_output_bytes_per_second.size()] += bytes;

			std::uint64_t total = 0;
			for (const std::uint64_t bucket : session.turn_output_bytes_per_second)
			{
				total += bucket;
			}
			return total > kAcpOutputFloodBytesPerMinute;
		}

		struct BufferedStdoutResult
		{
			bool changed = false;
			std::size_t lines = 0;
		};

		struct RemoteOutputMarker
		{
			std::string ack_line;
			std::uintmax_t stdout_cursor = 0;
			std::uintmax_t stderr_cursor = 0;
		};

		std::optional<RemoteOutputMarker> ParseRemoteOutputMarker(
		    const AcpSessionState& session, std::string_view line)
		{
			if (session.remote_output_delivery_token.empty()) return std::nullopt;
			const std::string expected = std::string(uam::remote::kRemoteOutputMarkerPrefix) +
			                             session.remote_output_delivery_token + " ";
			if (!line.starts_with(expected)) return std::nullopt;
			line.remove_prefix(expected.size());
			const std::size_t separator = line.find(' ');
			if (separator == std::string_view::npos) return std::nullopt;
			std::uintmax_t stdout_cursor = 0;
			std::uintmax_t stderr_cursor = 0;
			const auto stdout_result = std::from_chars(
			    line.data(), line.data() + separator, stdout_cursor);
			const auto stderr_result = std::from_chars(
			    line.data() + separator + 1, line.data() + line.size(), stderr_cursor);
			if (stdout_result.ec != std::errc{} ||
			    stdout_result.ptr != line.data() + separator ||
			    stderr_result.ec != std::errc{} ||
			    stderr_result.ptr != line.data() + line.size())
				return std::nullopt;
			return RemoteOutputMarker{
			    .ack_line = std::string(uam::remote::kRemoteOutputAckPrefix) +
			                session.remote_output_delivery_token + " " +
			                std::to_string(stdout_cursor) + " " +
			                std::to_string(stderr_cursor) + "\n",
			    .stdout_cursor = stdout_cursor,
			    .stderr_cursor = stderr_cursor,
			};
		}

		std::optional<int> ParseRemoteSourceExit(
		    const AcpSessionState& session, std::string_view line)
		{
			if (session.remote_output_delivery_token.empty()) return std::nullopt;
			const std::string expected = std::string(uam::remote::kRemoteSourceExitPrefix) +
			                             session.remote_output_delivery_token + " ";
			if (!line.starts_with(expected)) return std::nullopt;
			line.remove_prefix(expected.size());
			int exit_code = -1;
			const auto parsed = std::from_chars(line.data(), line.data() + line.size(), exit_code);
			if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size())
				return std::nullopt;
			return exit_code;
		}

		std::optional<std::string> ParseRemoteInputReceipt(
		    const AcpSessionState& session, std::string_view line)
		{
			if (session.remote_output_delivery_token.empty()) return std::nullopt;
			const std::string expected =
			    std::string(uam::remote::kRemoteInputReceiptPrefix) +
			    session.remote_output_delivery_token + " ";
			if (!line.starts_with(expected)) return std::nullopt;
			line.remove_prefix(expected.size());
			if (line.empty() || line.find(' ') != std::string_view::npos || line.size() > 256)
				return std::nullopt;
			return std::string(line);
		}

		BufferedStdoutResult ProcessBufferedAcpStdout(
		    AppState& app,
		    AcpSessionState& session,
		    ChatSession& chat,
		    CefRefPtr<CefBrowser> browser,
		    std::size_t max_lines,
		    PollClock::time_point deadline)
		{
			BufferedStdoutResult result;
			std::size_t consumed = 0;
			while (result.lines < max_lines && PollClock::now() < deadline)
			{
				const std::size_t newline_pos = session.stdout_buffer.find('\n', consumed);
				if (newline_pos == std::string::npos)
				{
					break;
				}

				const std::string line = session.stdout_buffer.substr(consumed, newline_pos - consumed);
				consumed = newline_pos + 1;
				++result.lines;
				if (const std::optional<RemoteOutputMarker> marker =
				        ParseRemoteOutputMarker(session, line))
				{
					session.pending_remote_stdout_cursor = marker->stdout_cursor;
					session.pending_remote_stderr_cursor = marker->stderr_cursor;
					session.pending_remote_output_ack_line = marker->ack_line;
					result.changed = true;
					continue;
				}
				if (const std::optional<int> source_exit =
				        ParseRemoteSourceExit(session, line))
				{
					session.remote_source_exit_reported = true;
					session.remote_source_exit_code = *source_exit;
					session.pending_remote_source_exit_ack_line =
					    std::string(uam::remote::kRemoteSourceExitAckPrefix) +
					    session.remote_output_delivery_token + " " +
					    std::to_string(*source_exit) + "\n";
					result.changed = true;
					continue;
				}
				if (const std::optional<std::string> input_receipt =
				        ParseRemoteInputReceipt(session, line))
				{
					session.pending_remote_input_receipt_id = *input_receipt;
					result.changed = true;
					continue;
				}
				result.changed = ProcessAcpLine(app, session, chat, line, browser) || result.changed;
				if (!session.running)
				{
					break;
				}
			}

			if (consumed != 0)
			{
				session.stdout_buffer.erase(0, consumed);
			}
			return result;
		}
	} // namespace

	void InvalidateAcpTransport(
	    AppState& app,
	    AcpSessionState& session,
	    ChatSession& chat,
	    const std::string& message)
	{
		InvalidateAcpTransportImpl(app, session, chat, message);
	}

	void RecoverDisconnectedRemoteAcpTransport(
	    AppState& app,
	    AcpSessionState& session,
	    ChatSession& chat,
	    const std::string& message)
	{
		InvalidateAcpTransportImpl(app, session, chat, message, true);
	}

bool AppendAcpStdoutChunk(AcpSessionState& session, std::string_view chunk)
{
	if (chunk.size() > kMaxAcpStdoutLineBytes || session.stdout_buffer.size() > kMaxAcpStdoutLineBytes - chunk.size())
	{
		session.stdout_buffer.clear();
		return false;
	}
	session.stdout_buffer.append(chunk);
	return true;
}

std::size_t ProcessBufferedAcpStdoutForTests(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser, std::size_t max_lines)
{
	return ProcessBufferedAcpStdout(app, session, chat, browser, max_lines, PollClock::time_point::max()).lines;
}

bool ProcessAcpLine(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line, CefRefPtr<CefBrowser> browser)
{
	const std::string trimmed = uam::strings::Trim(line);
	if (trimmed.empty())
	{
		return false;
	}
	if (session.processing)
	{
		const std::uint64_t bytes = static_cast<std::uint64_t>(line.size());
		const std::uint64_t limit_mib = static_cast<std::uint64_t>(std::clamp(
		    app.settings.acp_turn_output_limit_mib,
		    uam::settings::kMinAcpTurnOutputLimitMiB,
		    uam::settings::kMaxAcpTurnOutputLimitMiB));
		const std::uint64_t limit_bytes = limit_mib * kBytesPerMiB;
		if (bytes > limit_bytes - std::min(session.turn_protocol_bytes, limit_bytes))
		{
			return StopForAcpOutputSafety(
			    app, session, chat, "turn_output_limit_exceeded",
			    "The provider exceeded UAM's configured " + std::to_string(limit_mib) +
			        " MiB cumulative turn-output safety ceiling.");
		}
		session.turn_protocol_bytes += bytes;
		if (TurnOutputFlooded(session, bytes))
		{
			return StopForAcpOutputSafety(
			    app, session, chat, "turn_output_flood",
			    "The provider emitted more than 256 MiB of structured output within 60 seconds.");
		}
		if (!session.turn_output_warning_emitted && session.turn_protocol_bytes >= limit_bytes / 2)
		{
			session.turn_output_warning_emitted = true;
			AppendAcpDiagnostic(
			    session, "read", "turn_output_warning", "", "", false, 0,
			    "Provider turn output reached half of the configured " + std::to_string(limit_mib) +
			        " MiB safety ceiling.");
		}
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
		InvalidateAcpTransport(app, session, chat, error_message);
		return true;
	}
	if (JsonDiagnosticStringValue(message, "method") == kRemoteAttachedMethod)
	{
		if (!session.recovering_remote_turn && !session.recovering_remote_process) return true;
		AppendAcpDiagnostic(session, "reconnect", "remote_attached", std::string(kRemoteAttachedMethod),
		                    "", false, 0, "Reattached to the existing remote turn.");
		if (session.remote_stop_unconfirmed)
		{
			session.recovering_remote_turn = false;
			session.recovering_remote_process = false;
			(void)uam::StopAcpSession(app, chat.id);
			return true;
		}
		session.recovering_remote_process = false;
		ReactivateConfirmedRemoteGoal(app, chat);
		session.reconnect_attempts = 0;
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
				MarkAcpRuntimeActivity(session);
			}
			catch (const std::exception& ex)
			{
				const std::string error_message = std::string("Claude stream-json message handling failed: ") + ex.what();
				AppendAcpDiagnostic(session, "parse", "claude_message_parse_error", "", "", false, 0, error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
				InvalidateAcpTransport(app, session, chat, error_message);
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
					MarkAcpRuntimeActivity(session);
				}
				catch (const std::exception& ex)
				{
					const std::string error_message = std::string("Codex app-server message handling failed: ") + ex.what();
					AppendAcpDiagnostic(session, "parse", "codex_message_parse_error", method, "", false, 0, error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
					InvalidateAcpTransport(app, session, chat, error_message);
				}
			}
			else
			{
				try
				{
					if (method == uam::acp_methods::kSessionUpdate)
					{
						HandleSessionUpdate(app, session, chat, JsonObjectValue(message, "params"), browser);
					}
					else
					{
						HandleAcpRequest(app, session, chat, message);
					}
					MarkAcpRuntimeActivity(session);
				}
				catch (const std::exception& ex)
				{
					const std::string error_message = std::string("ACP protocol message handling failed: ") + ex.what();
					AppendAcpDiagnostic(session, "parse", "protocol_message_error", method, "", false, 0,
					                    error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
					InvalidateAcpTransport(app, session, chat, error_message);
				}
			}
			return true;
		}

		if (uam::nlohmann_json::FindField(message, "id") != nullptr)
		{
			try
			{
				HandleAcpResponse(app, session, chat, message);
				MarkAcpRuntimeActivity(session);
			}
			catch (const std::exception& ex)
			{
				const std::string error_message = std::string("ACP response handling failed: ") + ex.what();
				AppendAcpDiagnostic(session, "parse", "protocol_response_error", "", JsonRpcIdToDiagnosticString(message["id"]), false, 0,
				                    error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
				InvalidateAcpTransport(app, session, chat, error_message);
			}
			return true;
		}

		AppendAcpDiagnostic(session, "message", "ignored_without_method_or_id", "", "", false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		return false;
	}
}

bool DrainStdout(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
{
	if (!session.running)
	{
		return false;
	}
	bool changed = false;
	if (!session.pending_remote_input_receipt_id.empty() &&
	    session.pending_remote_input_receipt_id == chat.remote_prompt_delivery_id)
	{
		const std::string old_session_id = chat.remote_prompt_delivery_session_id;
		const std::string old_delivery_id = chat.remote_prompt_delivery_id;
		const std::string old_payload = chat.remote_prompt_delivery_payload;
		chat.remote_prompt_delivery_session_id.clear();
		chat.remote_prompt_delivery_id.clear();
		chat.remote_prompt_delivery_payload.clear();
		if (SaveChatQuietly(app, chat))
		{
			session.pending_remote_input_receipt_id.clear();
			changed = true;
		}
		else
		{
			chat.remote_prompt_delivery_session_id = old_session_id;
			chat.remote_prompt_delivery_id = old_delivery_id;
			chat.remote_prompt_delivery_payload = old_payload;
			app.status_line = "Remote prompt receipt is waiting for a successful local save.";
			return true;
		}
	}
	session.stdout_poll_pending = false;
	std::size_t bytes_read = 0;
	std::size_t lines_processed = 0;
	const PollClock::time_point deadline = PollClock::now() + kAcpStdoutTimePerPoll;
	std::array<char, 8192> buffer{};
	while (bytes_read < kAcpStdoutBytesPerPoll && lines_processed < kAcpStdoutLinesPerPoll && PollClock::now() < deadline)
	{
		const BufferedStdoutResult buffered = ProcessBufferedAcpStdout(
		    app, session, chat, browser, kAcpStdoutLinesPerPoll - lines_processed, deadline);
		changed = buffered.changed || changed;
		lines_processed += buffered.lines;
		if (!session.running)
		{
			return true;
		}
		if (lines_processed >= kAcpStdoutLinesPerPoll || PollClock::now() >= deadline)
		{
			break;
		}

		std::string read_error;
		const std::size_t read_capacity = std::min(buffer.size(), kAcpStdoutBytesPerPoll - bytes_read);
		const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(session, buffer.data(), read_capacity, &read_error);
		if (read_bytes > 0)
		{
			bytes_read += static_cast<std::size_t>(read_bytes);
			if (!AppendAcpStdoutChunk(session, std::string_view(buffer.data(), static_cast<std::size_t>(read_bytes))))
			{
				const std::string message = std::string(RuntimeDisplayName(session)) + " emitted an oversized structured response without a newline.";
				AppendAcpDiagnostic(session, "parse", "stdout_line_too_large", "", "", false, 0, message);
				InvalidateAcpTransport(app, session, chat, message);
				return true;
			}
			continue;
		}

		if (read_bytes == -2)
		{
			session.stdout_poll_pending = false;
			break;
		}

		if (read_bytes == 0)
		{
			session.stdout_poll_pending = false;
			break;
		}

		const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout: " + read_error);
		AppendAcpDiagnostic(session, "read", "stdout_read_failed", "", "", false, 0, message);
		RecoverDisconnectedRemoteAcpTransport(app, session, chat, message);
		changed = true;
		break;
	}
	if (bytes_read >= kAcpStdoutBytesPerPoll || lines_processed >= kAcpStdoutLinesPerPoll || PollClock::now() >= deadline)
	{
		session.stdout_poll_pending = true;
	}
	if (!session.pending_remote_output_ack_line.empty())
	{
		// A permission or user-input request is actionable runtime state. Do not ACK
		// its bytes until the wait is resolved, so a crash forces runner replay.
		if (session.waiting_for_permission || session.waiting_for_user_input)
			return true;
		if (session.remote_consumed_stderr_cursor < session.pending_remote_stderr_cursor)
		{
			session.stderr_poll_pending = true;
			return true;
		}
		const std::uintmax_t old_stdout_cursor = chat.remote_delivered_stdout_cursor;
		const std::uintmax_t old_stderr_cursor = chat.remote_delivered_stderr_cursor;
		const std::vector<uam::AcpRemoteInteractionResponseState> old_responses =
		    chat.remote_interaction_responses;
		chat.remote_delivered_stdout_cursor = session.pending_remote_stdout_cursor;
		chat.remote_delivered_stderr_cursor = session.pending_remote_stderr_cursor;
		chat.remote_interaction_responses.clear();
		if (!SaveChatQuietly(app, chat))
		{
			chat.remote_delivered_stdout_cursor = old_stdout_cursor;
			chat.remote_delivered_stderr_cursor = old_stderr_cursor;
			chat.remote_interaction_responses = old_responses;
			app.status_line = "Remote output is waiting for a successful local save.";
			return true;
		}
		std::string write_error;
		if (!PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(
		        session, session.pending_remote_output_ack_line.data(),
		        session.pending_remote_output_ack_line.size(), &write_error))
		{
			const std::string message = "Failed to acknowledge persisted remote output: " +
			    uam::strings::NonEmptyOrFallback(write_error, "unknown transport error");
			AppendAcpDiagnostic(session, "write", "remote_output_ack_failed", "", "",
			                    false, 0, message);
			RecoverDisconnectedRemoteAcpTransport(app, session, chat, message);
			return true;
		}
		session.pending_remote_output_ack_line.clear();
		session.pending_remote_stdout_cursor = 0;
		session.pending_remote_stderr_cursor = 0;
		changed = true;
	}
	if (!session.pending_remote_source_exit_ack_line.empty())
	{
		chat.remote_source_exit_pending = true;
		chat.remote_source_exit_code = session.remote_source_exit_code;
		if (!SaveChatQuietly(app, chat))
		{
			app.status_line = "Remote completion is waiting for a successful local save.";
			return true;
		}
		std::string write_error;
		if (!PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(
		        session, session.pending_remote_source_exit_ack_line.data(),
		        session.pending_remote_source_exit_ack_line.size(), &write_error))
		{
			RecoverDisconnectedRemoteAcpTransport(app, session, chat,
			    "Failed to acknowledge persisted remote completion: " +
			    uam::strings::NonEmptyOrFallback(write_error, "unknown transport error"));
			return true;
		}
		session.pending_remote_source_exit_ack_line.clear();
		changed = true;
	}
	return changed;
}

bool DrainStderr(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	bool changed = false;
	session.stderr_poll_pending = false;
	std::size_t bytes_read = 0;
	std::size_t chunks_read = 0;
	const PollClock::time_point deadline = PollClock::now() + kAcpStderrTimePerPoll;
	std::array<char, 4096> buffer{};
	while (bytes_read < kAcpStderrBytesPerPoll && chunks_read < kAcpStderrChunksPerPoll && PollClock::now() < deadline)
	{
		std::string read_error;
		const std::size_t read_capacity = std::min(buffer.size(), kAcpStderrBytesPerPoll - bytes_read);
		const std::ptrdiff_t read_result = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStderr(session, buffer.data(), read_capacity, &read_error);
		if (read_result > 0)
		{
			bytes_read += static_cast<std::size_t>(read_result);
			++chunks_read;
			AppendRecentStderr(session, std::string(buffer.data(), static_cast<std::size_t>(read_result)));
			const std::uintmax_t read_count = static_cast<std::uintmax_t>(read_result);
			session.remote_consumed_stderr_cursor +=
			    std::min(read_count,
			             std::numeric_limits<std::uintmax_t>::max() -
			                 session.remote_consumed_stderr_cursor);
			changed = true;
			continue;
		}

		if (read_result == -2 || read_result == 0)
		{
			session.stderr_poll_pending = false;
			break;
		}

		const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr: " + read_error);
		AppendAcpDiagnostic(session, "read", "stderr_read_failed", "", "", false, 0, message);
		RecoverDisconnectedRemoteAcpTransport(app, session, chat, message);
		changed = true;
		break;
	}
	if (bytes_read >= kAcpStderrBytesPerPoll || chunks_read >= kAcpStderrChunksPerPoll || PollClock::now() >= deadline)
	{
		session.stderr_poll_pending = true;
	}
	return changed;
}

void MarkAcpProcessExited(AcpSessionState& session, ChatSession* chat,
	                      bool has_exit_code, int exit_code,
	                      bool preserve_active_turn)
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
	if (active_turn && !preserve_active_turn)
	{
		const std::string message = uam::strings::NonEmptyOrFallback(session.last_error, std::string(RuntimeDisplayName(session)) + " process exited during an active turn.");
		FailAcpTurnOrSession(session, chat, message);
	}
	else if (!active_turn)
	{
		session.lifecycle_state = kAcpLifecycleStopped;
	}
	if (!preserve_active_turn) session.processing = false;
	session.stdout_poll_pending = false;
	session.stderr_poll_pending = false;
	session.stdout_buffer.clear();
	session.stderr_buffer.clear();
	if (!preserve_active_turn)
	{
		session.prompt_request_id = 0;
		session.cancel_request_id = 0;
		session.pending_request_methods.clear();
		session.current_assistant_message_index = -1;
		session.pending_assistant_thoughts.clear();
		ResetAcpPendingInteractionState(session);
	}
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
}

} // namespace uam::acp_detail
