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

	/// <summary>Strict by default; initial quiesce admission may wait only for owned discovery and background scans.</summary>
	inline bool HostHasActiveRuntimeWork(const AppState& app, const std::string& host_id, bool allow_quiesce = false)
	{
		const auto lease = app.remote_vcs_operation_leases_by_host_id.find(host_id);
		if (lease != app.remote_vcs_operation_leases_by_host_id.end() && !lease->second.expired())
			return true;
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
			    (chat.remote_process_exists || chat.remote_turn_reconnect_pending || chat.remote_stop_cleanup_pending ||
			     chat.remote_restart_pending) && !may_quiesce(chat.id);
		};
		if (std::ranges::any_of(app.chats, active_chat) || std::ranges::any_of(app.model_discovery_chats, active_chat)) return true;
		if (std::ranges::any_of(app.acp_sessions, [&](const auto& session)
		    { return session != nullptr && (session->process_execution_host_id == host_id || chat_uses_host(session->chat_id)) &&
		             (session->running || session->local_stop_pending || session->remote_stop_pending ||
		              session->remote_stop_unconfirmed || session->reconnect_pending ||
		              session->recovering_remote_turn || session->recovering_remote_process ||
		              session->worktree_launch_pending || session->pending_local_launch ||
		              session->pending_agent_input || session->pending_prompt_delivery || session->pending_queued_history ||
		              session->remote_context_preparation) && !may_quiesce(session->chat_id); }))
			return true;
		if (std::ranges::any_of(app.pending_acp_remote_stops, [&](const auto& stop)
		{
			if (!stop) return false;
			const auto session = std::ranges::find_if(app.acp_sessions, [&](const auto& value)
			    { return value && value->chat_id == stop->chat_id; });
			const bool known_chat = std::ranges::find(app.chats, stop->chat_id, &ChatSession::id) != app.chats.end() ||
			    std::ranges::find(app.model_discovery_chats, stop->chat_id, &ChatSession::id) != app.model_discovery_chats.end();
			const bool unknown_owner = !known_chat && (session == app.acp_sessions.end() || (*session)->process_execution_host_id.empty());
			return (unknown_owner || chat_uses_host(stop->chat_id) ||
			    (session != app.acp_sessions.end() && (*session)->process_execution_host_id == host_id)) && !may_quiesce(stop->chat_id);
		})) return true;
		if (std::ranges::any_of(app.acp_process_stop_tasks, [&](const auto& stop)
		    { return stop.execution_host_id == host_id && !may_quiesce(stop.chat_id) &&
		        (!stop.finished || !stop.finished->load() || (stop.result && !stop.result->exit_confirmed)); })) return true;
		if (app.runtime_cli_version_check_task.running && app.runtime_cli_version_check_task.execution_host.id == host_id) return true;
		if (std::ranges::any_of(app.runtime_cli_install_tasks, [&](const auto& item)
		    { return item.second.running && item.second.execution_host.id == host_id; }) ||
		    std::ranges::any_of(app.pending_cli_updates, [&](const auto& item)
		    { return item.second.execution_host.id == host_id; }) ||
		    std::ranges::any_of(app.runtime_cli_versions_by_provider_id, [&](const auto& item)
		    { return item.second.execution_host.id == host_id && item.second.last_install_status == "awaiting-verification"; })) return true;
		if (app.startup_native_import_service &&
		    (app.startup_native_import_service->SelectedImportPendingOnHost(host_id) ||
		     (!allow_quiesce && app.startup_native_import_service->BackgroundScanPendingOnHost(host_id)))) return true;
		return std::ranges::any_of(app.cli_terminals, [&](const auto& terminal)
		{
			return terminal != nullptr && (terminal->running || terminal->should_launch || terminal->worktree_launch_pending || terminal->native_session_setup_cancel != nullptr || terminal->context_preparation != nullptr) &&
			       (chat_uses_host(terminal->attached_chat_id) ||
			        chat_uses_host(terminal->frontend_chat_id));
		});
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
