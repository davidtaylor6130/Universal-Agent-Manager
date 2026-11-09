#pragma once

#include "app/agent_run_scheduler.h"
#include "app/startup_native_import_service.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/state/app_state.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
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

	/// <summary>Input for this chat still being saved, routed, prepared or delegated; a launch or a stop could race it.</summary>
	inline bool ChatAdmittingInput(const AppState& app, const std::string& chat_id)
	{
		const auto branch = app.branch_preparations.find(chat_id);
		return app.pending_chat_save_tasks.contains(chat_id) || app.pending_chat_save_at_by_chat_id.contains(chat_id) ||
		    app.worktree_operation_chat_ids.contains(chat_id) || app.worktree_launch_preparations.contains(chat_id) ||
		    app.execution_routing_waits.contains(chat_id) || (branch != app.branch_preparations.end() && !branch->second.expired()) ||
		    AgentRunScheduler::HasPendingInitialMention(app, chat_id); // also covers managed follow-ups
	}

	/// <summary>An unsent-prompt record whose row the chat still shows as unsent, so the user can send or remove it.
	/// A record with no such row can be neither, so it holds nothing. Unloaded history counts: its rows are not known yet.</summary>
	inline bool ChatHasActionableUnsentPrompt(const AppState& app, const ChatSession& chat)
	{
		if (chat.acp_unsent_prompt_recoveries.empty() || !chat.messages_loaded) return !chat.acp_unsent_prompt_recoveries.empty();
		const std::vector<int> shown = KnownUnsentAcpMessageIndices(app, chat);
		return std::ranges::any_of(chat.acp_unsent_prompt_recoveries, [&](const AcpUnsentPromptRecovery& value)
		    { return std::ranges::find(shown, value.user_message_index) != shown.end() &&
		          chat.messages[static_cast<std::size_t>(value.user_message_index)].AcpPromptAwaitsDecision(); });
	}

	/// <summary>Saved chat fields that mean a turn, its input, a restart or a compaction may still be live on the host.</summary>
	inline bool ChatHasRemoteTurnState(const AppState& app, const ChatSession& chat)
	{
		return chat.remote_turn_reconnect_pending || chat.remote_restart_pending || chat.remote_source_exit_pending ||
		    chat.remote_manual_compaction_pending || !chat.remote_active_turn_id.empty() ||
		    !chat.remote_prompt_delivery_id.empty() || !chat.remote_prompt_delivery_session_id.empty() ||
		    !chat.remote_pending_requests.empty() || !chat.remote_interaction_responses.empty() ||
		    !chat.acp_queued_prompts.empty() || chat.acp_dispatched_queued_prompt_count != 0 ||
		    ChatHasActionableUnsentPrompt(app, chat);
	}

	/// <summary>A stop that is queued, running or creating a process for this chat; an unconfirmed remote stop alone is not counted.</summary>
	inline bool ChatStopInFlight(const AppState& app, const std::string& chat_id)
	{
		return std::ranges::any_of(app.pending_acp_remote_stops, [&](const auto& stop) { return stop && stop->chat_id == chat_id; }) ||
		    std::ranges::any_of(app.acp_process_stop_tasks, [&](const auto& stop)
		        { return stop.chat_id == chat_id && (!stop.finished || !stop.finished->load() || (stop.result && !stop.result->exit_confirmed)); }) ||
		    acp_detail::AbandonedAcpProcessCreationPending(app, chat_id);
	}

	/// <summary>No turn, input, launch, stop, compaction, goal, managed run or provider background work is held by this runtime.
	/// Session setup requests are allowed, so a reattaching process can qualify. With saved_stop_only, a stopped runtime that
	/// is only retrying a stop the host never confirmed qualifies too; the caller has checked the chat's saved turn fields.</summary>
	inline bool SessionHoldsNoTurn(const AcpSessionState& session, bool saved_stop_only = false)
	{
		// The stop retry marks its reattach as a turn recovery; a turn this runtime still holds keeps processing set.
		const bool stop_retry_only = saved_stop_only && session.remote_stop_unconfirmed && !session.running;
		return !AcpSessionHasActiveTurn(session) && !AcpSessionHasPendingCancel(session) && session.prompt_request_id == 0 &&
		    (stop_retry_only || (!session.recovering_remote_turn && !session.remote_stop_unconfirmed)) &&
		    !session.local_stop_pending && !session.remote_stop_pending &&
		    !session.worktree_launch_pending && !session.pending_local_launch && !session.pending_agent_input &&
		    !session.pending_prompt_delivery && !session.pending_queued_history && !session.remote_context_preparation &&
		    session.pending_compaction == nullptr && !session.manual_compaction_pending && !session.context_compaction_in_progress &&
		    !session.turn_checkpoint_preflight_pending && !session.turn_checkpoint_commit_pending &&
		    session.pending_steer_requests.empty() && session.managed_agent_run_id.empty() && !session.goal_internal_session &&
		    session.goal_turn_kind.empty() && !session.goal_review_scheduled && !session.codex_goal_native_active &&
		    !session.restart_after_remote_stop_cleanup && !session.restart_marker_save_pending && !session.queued_rollback_save_pending &&
		    !session.queue_held && session.claude_live_background_task_ids.empty() && !session.managed_parent_continuation &&
		    session.goal_queue_admission == AcpSessionState::GoalQueueAdmission::None &&
		    !session.attached_skill_preparation && !session.pending_local_session_receipt &&
		    ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpBackgroundTaskCount(session) == 0;
	}

	/// <summary>A non-running runtime holding no turn and no provider request.</summary>
	inline bool SessionIsInertOffline(const AcpSessionState& session, bool saved_stop_only = false)
	{
		return !session.running && SessionHoldsNoTurn(session, saved_stop_only) && !AcpSessionHasBlockingRuntimeWork(session);
	}

	/// <summary>A usage-limit continuation that is pending, held or not yet settled; a stop would cancel or orphan it.</summary>
	inline bool ChatHasQuotaContinuation(const ChatSession& chat)
	{
		const std::string& state = chat.claude_quota_continuation.state;
		return state == "waiting" || state == "held" || state == "claimed" || state == "continued";
	}

	/// <summary>A reattach this install's wait retried at once: the attempt count and time it was retried.</summary>
	struct ReattachProbe
	{
		int attempts = 0;
		double retried_at = 0.0;
	};
	using ReattachProbes = std::unordered_map<std::string, ReattachProbe>;

	/// <summary>How a helper update treats one chat's helper-owned provider process (#771).</summary>
	enum class RetainedProcess
	{
		/// No saved process authority, or a turn, input, stop, restart or setup is still live: the normal checks apply.
		NotRetained,
		/// A ready provider process kept alive between turns: the update stops it through the confirmed remote stop and waits.
		StopIdle,
		/// A provider process still reattaching with no turn: admission waits for it to become idle or unreachable.
		Connecting,
		/// A reattach loop with no turn that this install has not yet retried: the wait retries it once first.
		Reattaching,
		/// Saved authority, a saved stop cleanup, or a reattach loop whose retry failed, with no live process, turn or running stop: it does
		/// not block the update. The saved fields are kept, reattach waits while the host is installing and retries once the new helper is committed.
		Unreachable,
	};

	inline RetainedProcess ClassifyRetainedProcess(const AppState& app, const ChatSession& chat, const std::string& host_id, const ReattachProbes* probes = nullptr)
	{
		// A waiting or claimed usage-limit continuation can still dispatch during the install, and input still being saved,
		// routed or prepared can still launch, so both keep blocking. A continued one with no live runtime has nothing to continue in.
		const std::string& quota = chat.claude_quota_continuation.state;
		if (chat.execution_host_id != host_id || host_id == execution_hosts::kLocalHostId || ChatHasRemoteTurnState(app, chat) ||
		    quota == "waiting" || quota == "claimed" || !chat.goal_pending_continuation_id.empty() ||
		    ChatStopInFlight(app, chat.id) || ChatAdmittingInput(app, chat.id)) return RetainedProcess::NotRetained;
		const auto found = std::ranges::find_if(app.acp_sessions, [&](const auto& value) { return value && value->chat_id == chat.id; });
		const AcpSessionState* session = found == app.acp_sessions.end() ? nullptr : found->get();
		if (session != nullptr && (session->model_discovery_only || session->ephemeral_model_discovery || session->provider_id != chat.provider_id ||
		    (!session->process_execution_host_id.empty() && session->process_execution_host_id != host_id))) return RetainedProcess::NotRetained;
		if (session != nullptr && session->running)
		{
			// A stop would cancel any live usage-limit continuation.
			if (!chat.remote_process_exists || ChatHasQuotaContinuation(chat) || !SessionHoldsNoTurn(*session)) return RetainedProcess::NotRetained;
			// The idle shutdown rule, without its grace period.
			if (AcpSessionCanIdleShutdown(*session, chat)) return RetainedProcess::StopIdle;
			return session->recovering_remote_process || !session->session_ready ? RetainedProcess::Connecting : RetainedProcess::NotRetained;
		}
		// A saved stop cleanup with no saved turn, input or restart (checked above) and no running stop is authority UAM cannot
		// reach, like any other: holding the update for it would be permanent, because only the host can settle it. The helper
		// refuses to shut down while it owns a process, so the update cannot end one that is in fact still alive.
		const bool saved_authority = chat.remote_process_exists || chat.remote_stop_cleanup_pending;
		const bool reattaching = session != nullptr && (session->reconnect_pending || session->recovering_remote_process);
		if (!saved_authority && !reattaching) return RetainedProcess::NotRetained;
		if (session != nullptr && !SessionIsInertOffline(*session, chat.remote_stop_cleanup_pending)) return RetainedProcess::NotRetained;
		// A process that only lost its connection may still be alive under the current helper; once a newer helper is
		// committed it could no longer be reached, so a pending reattach or unconfirmed stop gets one fresh attempt before it is set aside.
		// Only a reattach to an existing process can succeed during the wait; a plain relaunch is refused while the host is reserved.
		if (session != nullptr && (session->remote_stop_unconfirmed || (session->reconnect_pending && session->recovering_remote_process)))
		{
			const auto probe = probes == nullptr ? ReattachProbes::const_iterator{} : probes->find(chat.id);
			// Only a recorded failed attempt counts; a launch gate or routing wait that merely deferred the retry does not.
			const bool failed = probes != nullptr && probe != probes->end() && session->reconnect_attempts > probe->second.attempts;
			if (!failed) return RetainedProcess::Reattaching;
		}
		return RetainedProcess::Unreachable;
	}

	/// <summary>A non-running runtime on this host whose chat no longer exists, left retrying a reattach.</summary>
	inline bool IsStaleOrphanSession(const AppState& app, const AcpSessionState& session, const std::string& host_id)
	{
		return session.process_execution_host_id == host_id && host_id != execution_hosts::kLocalHostId &&
		    std::ranges::find(app.chats, session.chat_id, &ChatSession::id) == app.chats.end() &&
		    std::ranges::find(app.model_discovery_chats, session.chat_id, &ChatSession::id) == app.model_discovery_chats.end() &&
		    (session.reconnect_pending || session.recovering_remote_process) &&
		    SessionIsInertOffline(session) && !ChatStopInFlight(app, session.chat_id);
	}

	/// <summary>Which retained work a check may set aside: admission also yields to work the install stops; the install wait only to unreachable authority.</summary>
	enum class HostWorkScope
	{
		Strict,
		InstallAdmission,
		InstallWait,
	};

	/// <summary>Strict by default; initial quiesce admission may wait for owned discovery, background scans and idle kept-alive
	/// provider processes; both install scopes set aside unreachable saved process authority (#771).</summary>
	inline bool HostHasRuntimeWork(const AppState& app, const std::string& host_id, HostWorkScope scope, HostRuntimeBlockReason* reason_out = nullptr,
	    std::string* chat_id_out = nullptr, const ReattachProbes* probes = nullptr)
	{
		const bool allow_quiesce = scope == HostWorkScope::InstallAdmission;
		if (reason_out) *reason_out = HostRuntimeBlockReason::None;
		if (chat_id_out) chat_id_out->clear();
		HostRuntimeBlockReason matched_reason = HostRuntimeBlockReason::None;
		std::string matched_chat_id;
		const auto matches = [&](bool condition, HostRuntimeBlockReason reason)
		{
			if (condition && reason_out) matched_reason = reason;
			return condition;
		};
		const auto blocked = [&](HostRuntimeBlockReason reason)
		{
			if (reason_out) *reason_out = reason;
			if (chat_id_out) *chat_id_out = matched_chat_id;
			return true;
		};
		const auto owner = [&](const std::string& chat_id)
		{
			matched_chat_id = chat_id;
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
			if (scope == HostWorkScope::Strict) return false;
			const auto session = std::ranges::find_if(app.acp_sessions, [&](const auto& value)
			    { return value && value->chat_id == chat_id; });
			if (allow_quiesce && session != app.acp_sessions.end() && IsQuiesceableDiscovery(app, **session, host_id)) return true;
			if (session != app.acp_sessions.end() && IsStaleOrphanSession(app, **session, host_id)) return true;
			const auto chat = std::ranges::find(app.chats, chat_id, &ChatSession::id);
			if (chat == app.chats.end()) return false;
			const RetainedProcess retained = ClassifyRetainedProcess(app, *chat, host_id, probes);
			return retained == RetainedProcess::Unreachable ||
			    (allow_quiesce && (retained == RetainedProcess::StopIdle || retained == RetainedProcess::Connecting || retained == RetainedProcess::Reattaching));
		};
		const auto active_chat = [&](const ChatSession& chat)
		{
			return chat.execution_host_id == host_id &&
			    (matches(chat.remote_process_exists, HostRuntimeBlockReason::ChatProcess) || matches(chat.remote_turn_reconnect_pending, HostRuntimeBlockReason::ChatReconnect) || matches(chat.remote_stop_cleanup_pending, HostRuntimeBlockReason::ChatStopCleanup) ||
			     matches(chat.remote_restart_pending, HostRuntimeBlockReason::ChatRestart)) && !may_quiesce(chat.id) && owner(chat.id);
		};
		if (std::ranges::any_of(app.chats, active_chat) || std::ranges::any_of(app.model_discovery_chats, active_chat)) return blocked(matched_reason);
		if (std::ranges::any_of(app.acp_sessions, [&](const auto& session)
		    { return session != nullptr && (session->process_execution_host_id == host_id || chat_uses_host(session->chat_id)) &&
		             (matches(session->running, HostRuntimeBlockReason::SessionRunning) || matches(session->local_stop_pending, HostRuntimeBlockReason::SessionLocalStop) || matches(session->remote_stop_pending, HostRuntimeBlockReason::SessionRemoteStop) ||
		              matches(session->remote_stop_unconfirmed, HostRuntimeBlockReason::SessionUnconfirmedStop) || matches(session->reconnect_pending, HostRuntimeBlockReason::SessionReconnect) ||
		              matches(session->recovering_remote_turn, HostRuntimeBlockReason::SessionRecoverTurn) || matches(session->recovering_remote_process, HostRuntimeBlockReason::SessionRecoverProcess) ||
		              matches(session->worktree_launch_pending, HostRuntimeBlockReason::SessionWorktree) || matches(static_cast<bool>(session->pending_local_launch), HostRuntimeBlockReason::SessionLocalLaunch) ||
		              matches(static_cast<bool>(session->pending_agent_input), HostRuntimeBlockReason::SessionInput) || matches(static_cast<bool>(session->pending_prompt_delivery), HostRuntimeBlockReason::SessionDelivery) || matches(static_cast<bool>(session->pending_queued_history), HostRuntimeBlockReason::SessionHistory) ||
		              matches(static_cast<bool>(session->remote_context_preparation), HostRuntimeBlockReason::SessionContext)) && !may_quiesce(session->chat_id) && owner(session->chat_id); }))
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
			    matches((session != app.acp_sessions.end() && (*session)->process_execution_host_id == host_id), HostRuntimeBlockReason::PendingRemoteStop)) && !may_quiesce(stop->chat_id) && owner(known_chat ? stop->chat_id : std::string());
		})) return blocked(matched_reason);
		if (std::ranges::any_of(app.acp_process_stop_tasks, [&](const auto& stop)
		    { return stop.execution_host_id == host_id && !may_quiesce(stop.chat_id) &&
		        matches((!stop.finished || !stop.finished->load() || (stop.result && !stop.result->exit_confirmed)), HostRuntimeBlockReason::PhysicalStop) && owner(stop.chat_id); })) return blocked(matched_reason);
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
			       ((chat_uses_host(terminal->attached_chat_id) && owner(terminal->attached_chat_id)) ||
			        (chat_uses_host(terminal->frontend_chat_id) && owner(terminal->frontend_chat_id)));
		})) return blocked(matched_reason);
		return false;
	}

	inline bool HostHasActiveRuntimeWork(const AppState& app, const std::string& host_id, bool allow_quiesce = false, HostRuntimeBlockReason* reason_out = nullptr, std::string* chat_id_out = nullptr)
	{
		return HostHasRuntimeWork(app, host_id, allow_quiesce ? HostWorkScope::InstallAdmission : HostWorkScope::Strict, reason_out, chat_id_out);
	}

	/// <summary>User-facing refusal that names the blocking chat and how to clear it; the fixed code stays last for support.</summary>
	inline std::string HostRuntimeBlockMessage(const AppState& app, HostRuntimeBlockReason reason, const std::string& chat_id)
	{
		const auto chat = chat_id.empty() ? app.chats.end() : std::ranges::find(app.chats, chat_id, &ChatSession::id);
		const bool named = chat != app.chats.end() && !chat->title.empty();
		const bool discovery = chat == app.chats.end() && !chat_id.empty() &&
		    std::ranges::find(app.model_discovery_chats, chat_id, &ChatSession::id) != app.model_discovery_chats.end();
		const std::string name = named ? "\xE2\x80\x9C" + chat->title + "\xE2\x80\x9D" : chat != app.chats.end() ? std::string("An untitled chat")
		    : discovery ? std::string("Model discovery") : std::string("A chat");
		const std::string inner_name = named ? name : chat != app.chats.end() ? std::string("an untitled chat")
		    : discovery ? std::string("model discovery") : std::string("a chat");
		const auto found_session = chat_id.empty() ? app.acp_sessions.end() : std::ranges::find_if(app.acp_sessions, [&](const auto& value) { return value && value->chat_id == chat_id; });
		const AcpSessionState* session = found_session == app.acp_sessions.end() ? nullptr : found_session->get();
		// The first saved or runtime flag that matched is often not the real hold, so describe what the chat is actually doing.
		const auto chat_hold = [&]() -> std::string
		{
			// An unconfirmed stop alone no longer holds the update, so input the user can still act on is the hold to name.
			if (chat != app.chats.end() && !ChatStopInFlight(app, chat_id) && ChatHasActionableUnsentPrompt(app, *chat))
				return name + " has queued or unsent input for this host. Send or remove it in that chat, then update the helper.";
			if (discovery && AcpStopInProgress(app, chat_id))
				return "Model discovery is still stopping on this host. UAM is confirming the stop; update the helper once the host answers.";
			if (session != nullptr && session->remote_stop_unconfirmed && !session->running && !session->reconnect_pending && !ChatStopInFlight(app, chat_id))
				return "UAM could not confirm that " + inner_name + " stopped on this host. Restart UAM so it retries the stop, then update the helper.";
			if (session == nullptr && chat != app.chats.end() && chat->remote_stop_cleanup_pending)
				return "UAM could not confirm that " + inner_name + " stopped on this host. Restart UAM so it retries the stop, then update the helper.";
			if (!chat_id.empty() && AcpStopInProgress(app, chat_id))
				return name + " is still stopping on this host. UAM is confirming the stop; update the helper once the host answers.";
			if (session != nullptr && (session->processing || AcpSessionIsWaitingForInput(*session) || session->prompt_request_id != 0 ||
			    !session->queued_prompt.empty() || session->recovering_remote_turn))
				return name + " has a turn in progress on this host. Stop it or let it finish, then update the helper.";
			if (chat != app.chats.end() && (chat->claude_quota_continuation.state == "waiting" || chat->claude_quota_continuation.state == "held"))
				return name + " is waiting to continue after a usage limit. Cancel that continuation in the chat or let it run, then update the helper.";
			if (chat != app.chats.end() && ChatHasQuotaContinuation(*chat))
				return name + " is continuing after a usage limit on this host. Let that turn finish or stop it in the chat, then update the helper.";
			if ((session != nullptr && (session->queue_held || !session->queued_user_prompts.empty())) ||
			    (chat != app.chats.end() && (chat->acp_queue_held || !chat->acp_queued_prompts.empty() || ChatHasActionableUnsentPrompt(app, *chat))))
				return name + " has queued or unsent input for this host. Send or remove it in that chat, then update the helper.";
			if (session != nullptr && (!session->claude_live_background_task_ids.empty() || ProviderRuntimeRegistry::ResolveById(session->provider_id).AcpBackgroundTaskCount(*session) > 0))
				return name + " has background tasks running on this host. Let them finish or stop the chat, then update the helper.";
			if ((chat != app.chats.end() && !chat->goal_pending_continuation_id.empty()) ||
			    (session != nullptr && session->goal_queue_admission != AcpSessionState::GoalQueueAdmission::None))
				return name + " has a goal continuation waiting to run on this host. Let it run or pause the goal, then update the helper.";
			if (chat != app.chats.end() && ChatAdmittingInput(app, chat_id))
				return name + " is saving or preparing input on this host. Try the update again in a moment.";
			if (session != nullptr && !session->running && session->reconnect_pending)
				return "UAM is reconnecting to the process for " + inner_name + " on this host before updating its helper. Try the update again in a moment.";
			if (session != nullptr && session->running && (session->recovering_remote_process || !session->session_ready))
				return name + " is still connecting to its process on this host. Try the update again in a moment; if it keeps connecting, check the SSH connection or stop its runtime in that chat.";
			if (chat != app.chats.end() && ChatHasRemoteTurnState(app, *chat))
				return name + " has a turn in progress on this host. Stop it or let it finish, then update the helper.";
			return name + " still has work on this host. Stop it in that chat, then update the helper.";
		};
		std::string text;
		switch (reason)
		{
		case HostRuntimeBlockReason::None: return {};
		case HostRuntimeBlockReason::VcsLease:
			text = "A Git operation is running on this host. Wait for it to finish, then update the helper.";
			break;
		case HostRuntimeBlockReason::ChatReconnect:
		case HostRuntimeBlockReason::SessionRecoverTurn:
			text = name + " has a turn on this host that UAM is reconnecting to. Let it finish or stop it in that chat, then update the helper.";
			break;
		case HostRuntimeBlockReason::ChatStopCleanup:
		case HostRuntimeBlockReason::SessionUnconfirmedStop:
			text = chat_hold();
			break;
		case HostRuntimeBlockReason::SessionLocalStop:
		case HostRuntimeBlockReason::SessionRemoteStop:
		case HostRuntimeBlockReason::PendingRemoteStop:
		case HostRuntimeBlockReason::PhysicalStop:
		case HostRuntimeBlockReason::UnknownStopOwner:
			text = name + " is still stopping on this host. UAM is confirming the stop; update the helper once the host answers.";
			break;
		case HostRuntimeBlockReason::ChatRestart:
			text = name + " is restarting on this host. Let the restart finish, then update the helper.";
			break;
		case HostRuntimeBlockReason::SessionReconnect:
		case HostRuntimeBlockReason::ChatProcess:
		case HostRuntimeBlockReason::SessionRunning:
		case HostRuntimeBlockReason::SessionRecoverProcess:
		case HostRuntimeBlockReason::SessionWorktree:
		case HostRuntimeBlockReason::SessionLocalLaunch:
		case HostRuntimeBlockReason::SessionInput:
		case HostRuntimeBlockReason::SessionDelivery:
		case HostRuntimeBlockReason::SessionHistory:
		case HostRuntimeBlockReason::SessionContext:
			text = chat_hold();
			break;
		case HostRuntimeBlockReason::CliVersionCheck:
		case HostRuntimeBlockReason::CliInstall:
		case HostRuntimeBlockReason::CliUpdate:
		case HostRuntimeBlockReason::CliVerification:
			text = "A provider CLI check or update is running on this host. Wait for it to finish, then update the helper.";
			break;
		case HostRuntimeBlockReason::SelectedImport:
		case HostRuntimeBlockReason::BackgroundScan:
			text = "A chat history import from this host is running. Wait for it to finish, then update the helper.";
			break;
		case HostRuntimeBlockReason::TerminalRunning:
		case HostRuntimeBlockReason::TerminalLaunch:
		case HostRuntimeBlockReason::TerminalWorktree:
		case HostRuntimeBlockReason::TerminalSessionSetup:
		case HostRuntimeBlockReason::TerminalContext:
			text = "A terminal for " + inner_name + " is open on this host. Close it, then update the helper.";
			break;
		}
		return text + " (" + HostRuntimeBlockReasonCode(reason) + ")";
	}

	/// <summary>Before a helper update: confirmed-stops idle kept-alive provider processes. Saved chat fields and unreachable
	/// reattach loops are left alone; their reattach retries against the committed helper.</summary>
	/// Pending reattaches and unconfirmed stops are retried once at once and recorded in probes. Returns how many stops are in progress.
	inline std::size_t QuiesceRetainedProcessesOnHost(AppState& app, const std::string& host_id, ReattachProbes* probes = nullptr)
	{
		std::size_t stopping = 0;
		std::vector<std::string> stop_ids;
		std::vector<std::string> stop_retry_ids;
		for (const ChatSession& chat : app.chats)
		{
			const RetainedProcess retained = ClassifyRetainedProcess(app, chat, host_id, probes);
			if (retained == RetainedProcess::StopIdle) stop_ids.push_back(chat.id);
			if (retained != RetainedProcess::Reattaching || probes == nullptr || probes->contains(chat.id)) continue;
			const auto session = std::ranges::find_if(app.acp_sessions, [&](const auto& value) { return value && value->chat_id == chat.id; });
			if (session == app.acp_sessions.end()) continue;
			const double now = GetAppTimeSeconds();
			(*probes)[chat.id] = ReattachProbe{(*session)->reconnect_attempts, now};
			if ((*session)->reconnect_pending) (*session)->reconnect_not_before_time_s = now;
			else stop_retry_ids.push_back(chat.id); // an unconfirmed stop that was waiting for a restart to retry it
		}
		for (const std::string& chat_id : stop_retry_ids) (void)EnsureAcpStopProgress(app, chat_id);
		for (const std::string& chat_id : stop_ids)
		{
			(void)StopAcpSession(app, chat_id, AcpStopPurpose::ProviderUpdate);
			if (AcpStopInProgress(app, chat_id)) ++stopping;
		}
		return stopping;
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
