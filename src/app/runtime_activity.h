#pragma once

#include "app/chat_domain_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/app_time.h"
#include "common/state/app_state.h"

namespace uam
{

// A stale local wait may let the machine sleep: the local process survives it.
// A remote wait may not: sleep drops SSH and can lose the turn awaiting the answer.
inline bool AcpStaleWaitMaySleep(const AcpSessionState& session)
{
	const bool remote = !session.process_execution_host_id.empty() && session.process_execution_host_id != execution_hosts::kLocalHostId;
	return !remote && session.wait_is_stale && AcpSessionIsWaitingForInput(session);
}

inline bool RuntimeShouldKeepSystemAwake(const AppState& app)
{
	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && terminal->running && (terminal->generation_in_progress || terminal->turn_state == CliTerminalTurnState::Busy || terminal->lifecycle_state == CliTerminalLifecycleState::Busy || terminal->lifecycle_state == CliTerminalLifecycleState::Unknown))
		{
			return true;
		}
	}

	for (const auto& session : app.acp_sessions)
	{
		const bool stale_wait = session != nullptr && AcpStaleWaitMaySleep(*session);
		if (session != nullptr && session->running && !stale_wait && AcpSessionHasBlockingRuntimeWork(*session))
		{
			return true;
		}
	}

	for (const ChatSession& chat : app.chats)
	{
		if (chat.active_goal_id.empty())
		{
			continue;
		}

		bool stale_wait = false;
		for (const auto& session : app.acp_sessions)
		{
			if (session != nullptr && session->chat_id == chat.id && AcpStaleWaitMaySleep(*session))
			{
				stale_wait = true;
				break;
			}
		}
		if (stale_wait)
		{
			continue;
		}

		for (const Goal& goal : chat.goals)
		{
			if (goal.id == chat.active_goal_id && goal.status == GoalStatus::Active)
			{
				return true;
			}
		}
	}

	return false;
}

/// A running terminal that is idle at its prompt with no input or output for a few seconds has nothing for a poll to do.
inline bool CliTerminalHasPendingWork(const CliTerminalState& terminal, double now_s)
{
	constexpr double kRecentActivitySeconds = 5.0;
	return terminal.running &&
	    (terminal.lifecycle_state != CliTerminalLifecycleState::Idle || terminal.turn_state != CliTerminalTurnState::Idle ||
	        terminal.generation_in_progress || terminal.prompt_settle_required || !terminal.pending_steer_prompt.empty() ||
	        terminal.repository_baseline_pending || terminal.worktree_launch_pending || terminal.context_preparation != nullptr ||
	        terminal.native_identity_query_phase != 0 || !terminal.native_identity_deferred_input.empty() ||
	        now_s - terminal.last_activity_time_s < kRecentActivitySeconds);
}

/// Output a poll left unread: bytes over its per-poll budget, or complete lines it had no time to handle.
inline bool AcpSessionHasUnreadOutput(const AcpSessionState& session)
{
	return session.stdout_poll_pending || session.stderr_poll_pending || session.stdout_buffer.find('\n') != std::string::npos;
}

/// <summary>
/// Work that needs the 250 ms poll. A process that is only alive is not work: an idle provider stays up for
/// `cli_idle_timeout_seconds` after each turn, and polling four times a second for that long was most of UAM's idle CPU.
/// A running session counts once it could not enter its idle grace period (a turn, permission or question, queued
/// prompt, stop or restart cleanup, agent run, goal review or background task), or when a poll left output unread.
/// A stop still draining output counts after the process has ended. Bytes an idle provider writes unprompted wait for
/// the 1 s idle poll.
/// </summary>
inline bool RuntimeHasPendingWork(const AppState& app)
{
	if (!app.memory_extraction_tasks.empty() || !app.memory_extraction_queue.empty() || !app.worktree_operation_chat_ids.empty() ||
	    !app.queued_agent_run_ids.empty() || !app.pending_agent_run_cancellation_ids.empty())
	{
		return true;
	}

	const double now_s = GetAppTimeSeconds();
	for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && CliTerminalHasPendingWork(*terminal, now_s)) return true;
	}

	for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
	{
		if (session == nullptr) continue;
		if (session->local_stop_pending || session->remote_stop_pending || AcpSessionHasUnreadOutput(*session)) return true;
		if (!session->running) continue;
		const ChatSession* chat = ChatDomainService().FindChatById(app, session->chat_id);
		if (chat == nullptr || !chat->active_goal_id.empty() || !AcpSessionCanIdleShutdown(*session, *chat)) return true;
	}
	return false;
}

/// Keep terminal input responsive while a visible provider is waiting at its prompt.
inline bool NeedsInteractiveRuntimePolling(const AppState& app, bool terminal_ui_visible)
{
	for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
	{
		if (terminal_ui_visible && terminal != nullptr && terminal->running && terminal->ui_attached) return true;
	}
	const ChatSession* selected_chat = ChatDomainService().SelectedChat(app);
	if (selected_chat == nullptr)
	{
		return false;
	}

	const AcpSessionState* acp = FindAcpSessionForChat(app, selected_chat->id);
	return acp != nullptr && acp->running && AcpSessionHasBlockingRuntimeWork(*acp);
}

inline int NextPollDelayMs(const AppState& app, bool dictation_running, bool terminal_ui_visible)
{
	if (ChatHistorySyncService().TerminalNativeMediaPending(app) || NeedsInteractiveRuntimePolling(app, terminal_ui_visible))
	{
		return 16;
	}
	if (dictation_running)
	{
		return 50;
	}
	return RuntimeHasPendingWork(app) ? 250 : 1000;
}

} // namespace uam
