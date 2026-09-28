#include "common/runtime/terminal/terminal_lifecycle.h"

#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/runtime/app_time.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace uam
{

bool DetachCliTerminalUi(CliTerminalState& terminal, std::string_view attachment_id)
{
	if (terminal.ui_attachment_id != attachment_id) return false;
	terminal.ui_attached = false;
	return true;
}

void CloseCliTerminalHandles(CliTerminalState& terminal)
{
	PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
}

void FailCliTerminalTransport(CliTerminalState& terminal, std::string_view message)
{
	StopCliTerminal(terminal);
	terminal.should_launch = false;
	terminal.last_error.assign(message);
}

bool WriteToCliTerminal(CliTerminalState& terminal, const char* bytes, std::size_t len)
{
	const bool wrote = PlatformServicesFactory::Instance().terminal_runtime.WriteToCliTerminal(terminal, bytes, len);
	if (wrote && bytes != nullptr && len > 0)
	{
		const double now = GetAppTimeSeconds();
		terminal.last_activity_time_s = now;
		terminal.last_user_input_time_s = now;
	}
	else if (!wrote && terminal.running)
	{
		FailCliTerminalTransport(terminal, "Provider terminal input connection is unavailable.");
	}
	return wrote;
}

std::string BuildCliTerminalPromptInput(std::string_view prompt)
{
	std::string safe;
	safe.reserve(prompt.size() + 16);
	for (const unsigned char ch : prompt)
	{
		if (ch == '\n' || ch == '\t' || ch >= 0x20) safe.push_back(static_cast<char>(ch));
	}
	safe = uam::strings::Trim(safe);
	return safe.empty() ? std::string{} : "\x1b[200~" + safe + "\x1b[201~\r";
}

bool RequestCliTerminalSteer(CliTerminalState& terminal, std::string_view prompt, bool retry, std::string* error_out)
{
	if (error_out != nullptr) error_out->clear();
	const std::string input = BuildCliTerminalPromptInput(prompt);
	if (input.empty())
	{
		if (error_out != nullptr) *error_out = "Steering prompt is empty.";
		return false;
	}
	if (!terminal.running)
	{
		if (error_out != nullptr) *error_out = "Terminal fallback is not running.";
		return false;
	}
	if (terminal.lifecycle_state == CliTerminalLifecycleState::Unknown)
	{
		if (error_out != nullptr) *error_out = "Terminal activity is unavailable. Send or interrupt prompts in the native CLI.";
		return false;
	}
	if (!terminal.pending_steer_prompt.empty() && !retry)
	{
		if (error_out != nullptr) *error_out = "A terminal steering prompt is already pending.";
		return false;
	}
	if (retry && !terminal.pending_steer_prompt.empty() && BuildCliTerminalPromptInput(terminal.pending_steer_prompt) != input)
	{
		if (error_out != nullptr) *error_out = "Retry must preserve the pending steering prompt.";
		return false;
	}

	terminal.pending_steer_prompt = std::string(prompt);
	terminal.pending_steer_started_time_s = GetAppTimeSeconds();
	terminal.pending_steer_restart_attempted = false;
	terminal.last_error.clear();
	if (CliTerminalLifecycleIsIdleLive(terminal)) return TryDeliverPendingCliTerminalSteer(terminal);
	constexpr char interrupt = '\x03';
	if (!WriteToCliTerminal(terminal, &interrupt, 1))
	{
		terminal.last_error = "Failed to interrupt terminal fallback; steering prompt retained for retry.";
		if (error_out != nullptr) *error_out = terminal.last_error;
		return false;
	}
	return true;
}

bool TryDeliverPendingCliTerminalSteer(CliTerminalState& terminal)
{
	if (terminal.pending_steer_prompt.empty() || !CliTerminalLifecycleIsIdleLive(terminal)) return false;
	const std::string input = BuildCliTerminalPromptInput(terminal.pending_steer_prompt);
	if (input.empty() || !WriteToCliTerminal(terminal, input.data(), input.size()))
	{
		terminal.last_error = "Failed to deliver terminal steering prompt; prompt retained for retry.";
		return false;
	}
	terminal.pending_steer_prompt.clear();
	terminal.pending_steer_started_time_s = 0.0;
	terminal.pending_steer_restart_attempted = false;
	terminal.last_error.clear();
	MarkCliTerminalTurnBusy(terminal);
	return true;
}

CliTerminalSteerRecoveryAction CliTerminalSteerRecovery(const CliTerminalState& terminal, double now_seconds)
{
	if (terminal.lifecycle_state == CliTerminalLifecycleState::Unknown)
		return CliTerminalSteerRecoveryAction::None;
	if (terminal.pending_steer_prompt.empty() || terminal.pending_steer_started_time_s <= 0.0)
	{
		return CliTerminalSteerRecoveryAction::None;
	}
	const double wait_seconds = now_seconds - terminal.pending_steer_started_time_s;
	if (!terminal.pending_steer_restart_attempted && wait_seconds >= kCliTerminalSteerRestartTimeoutSeconds)
	{
		return CliTerminalSteerRecoveryAction::Restart;
	}
	if (terminal.pending_steer_restart_attempted && wait_seconds >= kCliTerminalSteerFailureTimeoutSeconds && terminal.last_error.empty())
	{
		return CliTerminalSteerRecoveryAction::ReportTimeout;
	}
	return CliTerminalSteerRecoveryAction::None;
}

CliTerminalInactivityRecoveryAction CliTerminalInactivityRecovery(const CliTerminalState& terminal, double now_seconds, double timeout_seconds, double interrupt_grace_seconds)
{
	if (!terminal.running || terminal.lifecycle_state != CliTerminalLifecycleState::Busy)
	{
		return CliTerminalInactivityRecoveryAction::None;
	}
	if (terminal.inactivity_interrupt_requested_time_s > 0.0)
	{
		return now_seconds - terminal.inactivity_interrupt_requested_time_s >= interrupt_grace_seconds ? CliTerminalInactivityRecoveryAction::Stop : CliTerminalInactivityRecoveryAction::None;
	}
	// A PTY has no structured progress signal: provider output can be a spinner and input can be
	// an interrupt or control sequence. Enforce a hard ceiling from the busy transition, then retain
	// the existing interrupt grace for cleanup.
	return terminal.last_busy_time_s > 0.0 && now_seconds - terminal.last_busy_time_s >= timeout_seconds ? CliTerminalInactivityRecoveryAction::Interrupt : CliTerminalInactivityRecoveryAction::None;
}

const char* CliTerminalLifecycleStateLabel(CliTerminalLifecycleState state)
{
	switch (state)
	{
	case CliTerminalLifecycleState::Disabled:     return "disabled";
	case CliTerminalLifecycleState::Stopped:      return "stopped";
	case CliTerminalLifecycleState::Unknown:      return "unknown";
	case CliTerminalLifecycleState::Idle:         return "idle";
	case CliTerminalLifecycleState::Busy:         return "busy";
	case CliTerminalLifecycleState::ShuttingDown: return "shuttingDown";
	}

	return "stopped";
}

const char* CliTerminalLifecycleStateLabel(const CliTerminalState& terminal)
{
	return CliTerminalLifecycleStateLabel(terminal.lifecycle_state);
}

bool CliTerminalLifecycleIsProcessing(const CliTerminalState& terminal)
{
	return terminal.running && CliTerminalLifecycleStateIsProcessing(terminal.lifecycle_state);
}

bool CliTerminalLifecycleIsIdleLive(const CliTerminalState& terminal)
{
	return terminal.running && terminal.lifecycle_state == CliTerminalLifecycleState::Idle;
}

bool CliTerminalPromptConfirmsTurnIdle(CliTerminalState& terminal, bool prompt_detected, bool received_output, double now_seconds)
{
	if (!terminal.running || !terminal.uses_prompt_activity_tracking ||
	    terminal.lifecycle_state != CliTerminalLifecycleState::Busy)
	{
		return false;
	}

	if (!terminal.prompt_settle_required)
	{
		return prompt_detected && received_output;
	}

	if (terminal.prompt_settle_candidate_time_s > 0.0)
	{
		if (received_output)
		{
			if (prompt_detected)
			{
				terminal.current_turn_output_bytes.clear();
				terminal.prompt_settle_candidate_time_s = now_seconds;
				return false;
			}

			terminal.prompt_settle_required = false;
			terminal.prompt_settle_candidate_time_s = 0.0;
			return false;
		}

		return now_seconds - terminal.prompt_settle_candidate_time_s >= kCliTerminalPromptSettleSeconds;
	}

	if (!received_output)
	{
		return false;
	}

	if (!prompt_detected)
	{
		terminal.prompt_settle_required = false;
		return false;
	}

	terminal.current_turn_output_bytes.clear();
	terminal.prompt_settle_candidate_time_s = now_seconds;
	return false;
}

void MarkCliTerminalTurnBusy(CliTerminalState& terminal, bool settle_first_prompt)
{
	const double now = GetAppTimeSeconds();
	terminal.current_turn_output_bytes.clear();
	terminal.prompt_settle_required = terminal.uses_prompt_activity_tracking && settle_first_prompt;
	terminal.prompt_settle_candidate_time_s = 0.0;
	terminal.lifecycle_state = terminal.uses_prompt_activity_tracking ? CliTerminalLifecycleState::Busy : CliTerminalLifecycleState::Unknown;
	terminal.turn_state = terminal.uses_prompt_activity_tracking ? CliTerminalTurnState::Busy : CliTerminalTurnState::Unknown;
	terminal.generation_in_progress = terminal.uses_prompt_activity_tracking;
	terminal.last_busy_time_s = terminal.uses_prompt_activity_tracking ? now : 0.0;
	terminal.inactivity_interrupt_requested_time_s = 0.0;
	terminal.shutdown_requested_time_s = 0.0;
}

void MarkCliTerminalTurnIdle(CliTerminalState& terminal)
{
	const double now = GetAppTimeSeconds();
	terminal.prompt_settle_required = false;
	terminal.prompt_settle_candidate_time_s = 0.0;
	terminal.lifecycle_state = terminal.running ? CliTerminalLifecycleState::Idle : CliTerminalLifecycleState::Stopped;
	terminal.turn_state = CliTerminalTurnState::Idle;
	terminal.generation_in_progress = false;
	terminal.last_idle_confirmed_time_s = now;
	terminal.inactivity_interrupt_requested_time_s = 0.0;
	terminal.shutdown_requested_time_s = 0.0;
}

bool IsCliTerminalTurnBusy(const CliTerminalState& terminal)
{
	return terminal.lifecycle_state == CliTerminalLifecycleState::Busy ||
	       terminal.turn_state == CliTerminalTurnState::Busy;
}

void MarkCliTerminalShuttingDown(CliTerminalState& terminal)
{
	const double now = GetAppTimeSeconds();
	terminal.prompt_settle_required = false;
	terminal.prompt_settle_candidate_time_s = 0.0;
	terminal.lifecycle_state = CliTerminalLifecycleState::ShuttingDown;
	terminal.turn_state = CliTerminalTurnState::Busy;
	terminal.generation_in_progress = false;
	terminal.shutdown_requested_time_s = now;
}

void MarkCliTerminalStopped(CliTerminalState& terminal)
{
	terminal.current_turn_output_bytes.clear();
	terminal.prompt_settle_required = false;
	terminal.prompt_settle_candidate_time_s = 0.0;
	terminal.lifecycle_state = CliTerminalLifecycleState::Stopped;
	terminal.turn_state = CliTerminalTurnState::Idle;
	terminal.generation_in_progress = false;
	terminal.last_idle_confirmed_time_s = 0.0;
	terminal.last_busy_time_s = 0.0;
	terminal.shutdown_requested_time_s = 0.0;
	terminal.inactivity_interrupt_requested_time_s = 0.0;
}

void MarkCliTerminalDisabled(CliTerminalState& terminal)
{
	MarkCliTerminalStopped(terminal);
	terminal.lifecycle_state = CliTerminalLifecycleState::Disabled;
}

void ClearCliReadyForChat(AppState& app, std::string_view chat_id)
{
	const std::string_view target_id = TrimCliTerminalIdentityView(chat_id);
	if (target_id.empty())
	{
		return;
	}

	app.chats_with_unseen_updates.erase(std::string(target_id));
}

void RequestCliTerminalQuit(CliTerminalState& terminal)
{
	if (!terminal.running || !platform::CliTerminalHasWritableInput(terminal))
	{
		return;
	}

	(void)WriteToCliTerminal(terminal, kCliTerminalQuitCommand.data(), kCliTerminalQuitCommand.size());
}

void BeginCliTerminalIdleShutdown(CliTerminalState& terminal)
{
	RequestCliTerminalQuit(terminal);
	if (terminal.running)
	{
		MarkCliTerminalShuttingDown(terminal);
	}
}

bool IsCliTerminalEligibleForBackgroundIdleShutdown(const AppState& app,
                                                    const CliTerminalState& terminal,
                                                    std::string_view selected_chat_id,
                                                    double now)
{
	if (!terminal.running || terminal.ui_attached || terminal.lifecycle_state != CliTerminalLifecycleState::Idle)
	{
		return false;
	}

	if (CliTerminalPrimaryChatId(terminal).empty())
	{
		return false;
	}

	if (!selected_chat_id.empty() && CliTerminalMatchesChatId(terminal, selected_chat_id))
	{
		return false;
	}

	return terminal.last_idle_confirmed_time_s > 0.0 &&
	       (now - terminal.last_idle_confirmed_time_s) >= static_cast<double>(app.settings.cli_idle_timeout_seconds);
}

void StopCliTerminal(CliTerminalState& terminal, bool clear_identity, CliTerminalStopMode stop_mode)
{
	if (terminal.native_session_setup_cancel != nullptr)
	{
		terminal.native_session_setup_cancel->request_stop();
		terminal.native_session_setup_cancel.reset();
	}
	PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, stop_mode == CliTerminalStopMode::FastExit);

	CloseCliTerminalHandles(terminal);
	terminal.running = false;
	terminal.input_ready = false;
	terminal.startup_time_s = 0.0;
	MarkCliTerminalStopped(terminal);
	terminal.last_output_time_s = 0.0;
	terminal.recent_output_bytes.clear();
	terminal.last_native_history_snapshot_digest.clear();

	if (clear_identity)
	{
		terminal.attached_chat_id.clear();
		terminal.attached_session_id.clear();
		terminal.frontend_chat_id.clear();
		terminal.terminal_id.clear();
		terminal.session_ids_before.clear();
		terminal.linked_files_snapshot.clear();
		terminal.should_launch = false;
		terminal.ui_attached = false;
		terminal.ui_attachment_id.clear();
	}
}

CliTerminalState* FindCliTerminalForChat(AppState& app, std::string_view chat_id)
{
	return FindCliTerminalForRoutingKey(app, chat_id, "");
}

bool PrepareCliTerminalForAcpLaunch(AppState& app, std::string_view chat_id, std::string* error_out)
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}

	if (const ChatSession* chat = ChatDomainService().FindChatById(app, std::string(chat_id)))
	{
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *chat);
		if (const std::string update_error = ProviderCliLaunchBlockReason(app, provider.id, chat->execution_host_id); !update_error.empty())
		{
			if (error_out != nullptr) *error_out = update_error;
			return false;
		}
	}
	CliTerminalState* terminal = FindCliTerminalForChat(app, chat_id);
	if (terminal != nullptr && terminal->native_session_setup_cancel != nullptr)
	{
		StopCliTerminal(*terminal, false, CliTerminalStopMode::FastExit);
	}
	if (terminal == nullptr || !terminal->running)
	{
		return true;
	}

	const bool shutting_down = terminal->lifecycle_state == CliTerminalLifecycleState::ShuttingDown;
	if (terminal->lifecycle_state == CliTerminalLifecycleState::Unknown)
	{
		if (error_out != nullptr) *error_out = "Terminal activity is unavailable. Exit the native CLI before starting structured chat.";
		return false;
	}
	const bool has_blocking_work = !shutting_down && (terminal->lifecycle_state == CliTerminalLifecycleState::Busy || terminal->turn_state == CliTerminalTurnState::Busy || terminal->generation_in_progress || !terminal->pending_steer_prompt.empty());
	if (has_blocking_work)
	{
		if (error_out != nullptr)
		{
			*error_out = "Cannot start structured chat while terminal fallback is busy.";
		}
		return false;
	}

	StopCliTerminal(*terminal, false, CliTerminalStopMode::FastExit);
	terminal->should_launch = false;
	terminal->last_error.clear();
	return true;
}

void SyncCliTerminalToNativeHistory(AppState& app, const CliTerminalState& terminal)
{
	const ChatSession* chat = ChatDomainService().FindChatById(app, terminal.attached_chat_id);
	if (chat != nullptr && chat->execution_host_id != uam::execution_hosts::kLocalHostId) return;
	const std::string sync_target_id = CliTerminalSyncTargetId(terminal);
	if (!sync_target_id.empty())
	{
		SyncChatsFromNative(app, sync_target_id, true);
	}
}

void StopAndEraseCliTerminalForChat(AppState& app, std::string_view chat_id, bool sync_to_history)
{
	auto matches_chat_terminal = [&](std::unique_ptr<CliTerminalState>& terminal)
	{
		if (terminal == nullptr || !CliTerminalMatchesChatId(*terminal, chat_id))
		{
			return false;
		}

		if (sync_to_history)
		{
			SyncCliTerminalToNativeHistory(app, *terminal);
		}

		StopCliTerminal(*terminal, true, CliTerminalStopMode::FastExit);
		return true;
	};

	std::erase_if(app.cli_terminals, matches_chat_terminal);
}

void ClearStoppedCliTerminalAttachmentForChat(AppState& app, std::string_view chat_id)
{
	const std::string_view target_chat_id = TrimCliTerminalIdentityView(chat_id);
	if (target_chat_id.empty())
	{
		return;
	}

	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && !terminal->running && CliTerminalMatchesChatId(*terminal, target_chat_id))
		{
			StopCliTerminal(*terminal, true);
		}
	}
}

void StopAllCliTerminals(AppState& app, bool clear_identity)
{
	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr)
		{
			SyncCliTerminalToNativeHistory(app, *terminal);
			StopCliTerminal(*terminal, clear_identity);
		}
	}
}

void FastStopCliTerminalsForExit(AppState& app)
{
	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
		{
			continue;
		}

		SyncCliTerminalToNativeHistory(app, *terminal_ptr);
		StopCliTerminal(*terminal_ptr, true, CliTerminalStopMode::FastExit);
	}
}

} // namespace uam
