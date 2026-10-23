#pragma once

#include "app/startup_native_import_service.h"
#include "common/state/app_state.h"

#include <algorithm>
#include <optional>
#include <string>

namespace uam::remote_install
{
	/// <summary>Only an owned hidden catalogue process without user or goal input may yield to installation.</summary>
	inline bool IsQuiesceableDiscovery(const AppState& app, const AcpSessionState& session, const std::string& host_id)
	{
		if (!session.running || !session.ephemeral_model_discovery || !session.model_discovery_only ||
		    session.process_execution_host_id != host_id || session.remote_runner_protocol_version < 3 ||
		    !session.managed_agent_run_id.empty() || session.goal_internal_session ||
		    !session.goal_turn_kind.empty() || session.goal_review_turn || session.codex_goal_native_active ||
		    !session.queued_prompt.empty() || !session.queued_user_prompts.empty() || session.queue_held ||
		    session.pending_agent_input || session.pending_prompt_delivery || session.pending_queued_history ||
		    session.prompt_request_id != 0 || session.waiting_for_permission || session.waiting_for_user_input ||
		    std::ranges::find(app.chats, session.chat_id, &ChatSession::id) != app.chats.end()) return false;
		const auto chat = std::ranges::find(app.model_discovery_chats, session.chat_id, &ChatSession::id);
		return chat != app.model_discovery_chats.end() && chat->execution_host_id == host_id &&
		    chat->provider_id == session.provider_id && chat->remote_process_exists &&
		    !chat->remote_process_control_token.empty() &&
		    session.remote_output_delivery_token == chat->remote_process_control_token &&
		    chat->messages.empty() && chat->acp_queued_prompts.empty() && !chat->acp_queue_held &&
		    chat->agent_run_id.empty() && chat->active_goal_id.empty() && chat->goal_owner_chat_id.empty() &&
		    chat->goal_iteration_goal_id.empty() && chat->temporary_parent_chat_id.empty();
	}

	enum class HostRuntimeBlockReason
	{
		None,
		VcsLease,
		ChatProcess,
		ChatReconnect,
		ChatStopCleanup,
		ChatRestart,
		SessionRunning,
		SessionLocalStop,
		SessionRemoteStop,
		SessionUnconfirmedStop,
		SessionReconnect,
		SessionRecoverTurn,
		SessionRecoverProcess,
		SessionWorktree,
		SessionLocalLaunch,
		SessionInput,
		SessionDelivery,
		SessionHistory,
		SessionContext,
		UnknownStopOwner,
		PendingRemoteStop,
		PhysicalStop,
		CliVersionCheck,
		CliInstall,
		CliUpdate,
		CliVerification,
		SelectedImport,
		BackgroundScan,
		TerminalRunning,
		TerminalLaunch,
		TerminalWorktree,
		TerminalSessionSetup,
		TerminalContext,
	};

	/// <summary>Fixed privacy-safe codes; no host, chat, path or provider payload is included.</summary>
	inline const char* HostRuntimeBlockReasonCode(HostRuntimeBlockReason reason)
	{
		switch (reason)
		{
		case HostRuntimeBlockReason::None: return "none";
		case HostRuntimeBlockReason::VcsLease: return "vcs-lease";
		case HostRuntimeBlockReason::ChatProcess: return "chat-process";
		case HostRuntimeBlockReason::ChatReconnect: return "chat-reconnect";
		case HostRuntimeBlockReason::ChatStopCleanup: return "chat-stop-cleanup";
		case HostRuntimeBlockReason::ChatRestart: return "chat-restart";
		case HostRuntimeBlockReason::SessionRunning: return "session-running";
		case HostRuntimeBlockReason::SessionLocalStop: return "session-local-stop";
		case HostRuntimeBlockReason::SessionRemoteStop: return "session-remote-stop";
		case HostRuntimeBlockReason::SessionUnconfirmedStop: return "session-unconfirmed-stop";
		case HostRuntimeBlockReason::SessionReconnect: return "session-reconnect";
		case HostRuntimeBlockReason::SessionRecoverTurn: return "session-recover-turn";
		case HostRuntimeBlockReason::SessionRecoverProcess: return "session-recover-process";
		case HostRuntimeBlockReason::SessionWorktree: return "session-worktree";
		case HostRuntimeBlockReason::SessionLocalLaunch: return "session-local-launch";
		case HostRuntimeBlockReason::SessionInput: return "session-input";
		case HostRuntimeBlockReason::SessionDelivery: return "session-delivery";
		case HostRuntimeBlockReason::SessionHistory: return "session-history";
		case HostRuntimeBlockReason::SessionContext: return "session-context";
		case HostRuntimeBlockReason::UnknownStopOwner: return "unknown-stop-owner";
		case HostRuntimeBlockReason::PendingRemoteStop: return "pending-remote-stop";
		case HostRuntimeBlockReason::PhysicalStop: return "physical-stop";
		case HostRuntimeBlockReason::CliVersionCheck: return "cli-version-check";
		case HostRuntimeBlockReason::CliInstall: return "cli-install";
		case HostRuntimeBlockReason::CliUpdate: return "cli-update";
		case HostRuntimeBlockReason::CliVerification: return "cli-verification";
		case HostRuntimeBlockReason::SelectedImport: return "selected-import";
		case HostRuntimeBlockReason::BackgroundScan: return "background-scan";
		case HostRuntimeBlockReason::TerminalRunning: return "terminal-running";
		case HostRuntimeBlockReason::TerminalLaunch: return "terminal-launch";
		case HostRuntimeBlockReason::TerminalWorktree: return "terminal-worktree";
		case HostRuntimeBlockReason::TerminalSessionSetup: return "terminal-session-setup";
		case HostRuntimeBlockReason::TerminalContext: return "terminal-context";
		}
		return "none";
	}

	/// <summary>Strict by default; initial quiesce admission may wait only for owned discovery and background scans.</summary>
	inline bool HostHasActiveRuntimeWork(const AppState& app, const std::string& host_id, bool allow_quiesce = false, HostRuntimeBlockReason* reason_out = nullptr)
	{
		if (reason_out) *reason_out = HostRuntimeBlockReason::None;
		HostRuntimeBlockReason matched_reason = HostRuntimeBlockReason::None;
		const auto matches = [&](bool condition, HostRuntimeBlockReason reason)
		{
			if (condition && reason_out) matched_reason = reason;
			return condition;
		};
		const auto blocked = [&](HostRuntimeBlockReason reason)
		{
			if (reason_out) *reason_out = reason;
			return true;
		};
		const auto lease = app.remote_vcs_operation_leases_by_host_id.find(host_id);
		if (lease != app.remote_vcs_operation_leases_by_host_id.end() && !lease->second.expired())
			return blocked(HostRuntimeBlockReason::VcsLease);
		const auto chat_uses_host = [&](const std::string& chat_id)
		{
			const auto chat = std::ranges::find(app.chats, chat_id, &ChatSession::id);
			if (chat != app.chats.end() && chat->execution_host_id == host_id) return true;
			const auto discovery = std::ranges::find(app.model_discovery_chats, chat_id, &ChatSession::id);
			return discovery != app.model_discovery_chats.end() && discovery->execution_host_id == host_id;
		};
		const auto may_quiesce = [&](const std::string& chat_id)
		{
			if (!allow_quiesce) return false;
			const auto session = std::ranges::find_if(app.acp_sessions, [&](const auto& value)
			    { return value && value->chat_id == chat_id; });
			return session != app.acp_sessions.end() && IsQuiesceableDiscovery(app, **session, host_id);
		};
		const auto active_chat = [&](const ChatSession& chat)
		{
			return chat.execution_host_id == host_id &&
			    (matches(chat.remote_process_exists, HostRuntimeBlockReason::ChatProcess) || matches(chat.remote_turn_reconnect_pending, HostRuntimeBlockReason::ChatReconnect) || matches(chat.remote_stop_cleanup_pending, HostRuntimeBlockReason::ChatStopCleanup) ||
			     matches(chat.remote_restart_pending, HostRuntimeBlockReason::ChatRestart)) && !may_quiesce(chat.id);
		};
		if (std::ranges::any_of(app.chats, active_chat) || std::ranges::any_of(app.model_discovery_chats, active_chat)) return blocked(matched_reason);
		if (std::ranges::any_of(app.acp_sessions, [&](const auto& session)
		    { return session != nullptr && (session->process_execution_host_id == host_id || chat_uses_host(session->chat_id)) &&
		             (matches(session->running, HostRuntimeBlockReason::SessionRunning) || matches(session->local_stop_pending, HostRuntimeBlockReason::SessionLocalStop) || matches(session->remote_stop_pending, HostRuntimeBlockReason::SessionRemoteStop) ||
		              matches(session->remote_stop_unconfirmed, HostRuntimeBlockReason::SessionUnconfirmedStop) || matches(session->reconnect_pending, HostRuntimeBlockReason::SessionReconnect) ||
		              matches(session->recovering_remote_turn, HostRuntimeBlockReason::SessionRecoverTurn) || matches(session->recovering_remote_process, HostRuntimeBlockReason::SessionRecoverProcess) ||
		              matches(session->worktree_launch_pending, HostRuntimeBlockReason::SessionWorktree) || matches(static_cast<bool>(session->pending_local_launch), HostRuntimeBlockReason::SessionLocalLaunch) ||
		              matches(static_cast<bool>(session->pending_agent_input), HostRuntimeBlockReason::SessionInput) || matches(static_cast<bool>(session->pending_prompt_delivery), HostRuntimeBlockReason::SessionDelivery) || matches(static_cast<bool>(session->pending_queued_history), HostRuntimeBlockReason::SessionHistory) ||
		              matches(static_cast<bool>(session->remote_context_preparation), HostRuntimeBlockReason::SessionContext)) && !may_quiesce(session->chat_id); }))
			return blocked(matched_reason);
		if (std::ranges::any_of(app.pending_acp_remote_stops, [&](const auto& stop)
		{
			if (!stop) return false;
			const auto session = std::ranges::find_if(app.acp_sessions, [&](const auto& value)
			    { return value && value->chat_id == stop->chat_id; });
			const bool known_chat = std::ranges::find(app.chats, stop->chat_id, &ChatSession::id) != app.chats.end() ||
			    std::ranges::find(app.model_discovery_chats, stop->chat_id, &ChatSession::id) != app.model_discovery_chats.end();
			const bool unknown_owner = !known_chat && (session == app.acp_sessions.end() || (*session)->process_execution_host_id.empty());
			return (matches(unknown_owner, HostRuntimeBlockReason::UnknownStopOwner) || matches(chat_uses_host(stop->chat_id), HostRuntimeBlockReason::PendingRemoteStop) ||
			    matches((session != app.acp_sessions.end() && (*session)->process_execution_host_id == host_id), HostRuntimeBlockReason::PendingRemoteStop)) && !may_quiesce(stop->chat_id);
		})) return blocked(matched_reason);
		if (std::ranges::any_of(app.acp_process_stop_tasks, [&](const auto& stop)
		    { return stop.execution_host_id == host_id && !may_quiesce(stop.chat_id) &&
		        matches((!stop.finished || !stop.finished->load() || (stop.result && !stop.result->exit_confirmed)), HostRuntimeBlockReason::PhysicalStop); })) return blocked(matched_reason);
		if (app.runtime_cli_version_check_task.running && app.runtime_cli_version_check_task.execution_host.id == host_id) return blocked(HostRuntimeBlockReason::CliVersionCheck);
		if (std::ranges::any_of(app.runtime_cli_install_tasks, [&](const auto& item)
		    { return matches((item.second.running && item.second.execution_host.id == host_id), HostRuntimeBlockReason::CliInstall); }) ||
		    std::ranges::any_of(app.pending_cli_updates, [&](const auto& item)
		    { return matches(item.second.execution_host.id == host_id, HostRuntimeBlockReason::CliUpdate); }) ||
		    std::ranges::any_of(app.runtime_cli_versions_by_provider_id, [&](const auto& item)
		    { return matches((item.second.execution_host.id == host_id && item.second.last_install_status == "awaiting-verification"), HostRuntimeBlockReason::CliVerification); })) return blocked(matched_reason);
		if (app.startup_native_import_service &&
		    (matches(app.startup_native_import_service->SelectedImportPendingOnHost(host_id), HostRuntimeBlockReason::SelectedImport) ||
		     (!allow_quiesce && matches(app.startup_native_import_service->BackgroundScanPendingOnHost(host_id), HostRuntimeBlockReason::BackgroundScan)))) return blocked(matched_reason);
		if (std::ranges::any_of(app.cli_terminals, [&](const auto& terminal)
		{
			return terminal != nullptr && (matches(terminal->running, HostRuntimeBlockReason::TerminalRunning) || matches(terminal->should_launch, HostRuntimeBlockReason::TerminalLaunch) || matches(terminal->worktree_launch_pending, HostRuntimeBlockReason::TerminalWorktree) || matches(terminal->native_session_setup_cancel != nullptr, HostRuntimeBlockReason::TerminalSessionSetup) || matches(terminal->context_preparation != nullptr, HostRuntimeBlockReason::TerminalContext)) &&
			       (chat_uses_host(terminal->attached_chat_id) ||
			        chat_uses_host(terminal->frontend_chat_id));
		})) return blocked(matched_reason);
		return false;
	}

	/// <summary>Release only an unchanged host reservation, preserving every other settings edit.</summary>
	inline bool RestoreReservedHost(std::vector<ExecutionHost>& hosts, const ExecutionHost& reserved, const std::optional<ExecutionHost>& previous)
	{
		const auto current = std::ranges::find(hosts, reserved.id, &ExecutionHost::id);
		if (current == hosts.end() || *current != reserved) return false;
		if (previous) *current = *previous;
		else hosts.erase(current);
		return true;
	}
}
