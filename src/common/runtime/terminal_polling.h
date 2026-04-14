#ifndef UAM_COMMON_RUNTIME_TERMINAL_POLLING_H
#define UAM_COMMON_RUNTIME_TERMINAL_POLLING_H

#include "cef/cef_push.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"

inline double LatestCliTransportActivityTime(const uam::CliTerminalState& terminal)
{
	return std::max(terminal.last_user_input_time_s, terminal.last_ai_output_time_s);
}

inline bool TryMarkCliTurnCompleteFromSyncedHistory(uam::AppState& app,
                                                    uam::CliTerminalState& terminal,
                                                    const int previous_message_count,
                                                    const std::string& selected_chat_id)
{
	if (terminal.turn_state != uam::CliTerminalTurnState::Busy)
	{
		return false;
	}

	if (previous_message_count < 0)
	{
		return false;
	}

	std::string resolved_chat_id = !terminal.attached_session_id.empty() ? terminal.attached_session_id : terminal.attached_chat_id;

	if (resolved_chat_id.empty())
	{
		resolved_chat_id = terminal.frontend_chat_id;
	}

	const int synced_chat_index = ChatDomainService().FindChatIndexById(app, resolved_chat_id);

	if (synced_chat_index < 0)
	{
		return false;
	}

	const ChatSession& synced_chat = app.chats[static_cast<std::size_t>(synced_chat_index)];

	if (static_cast<int>(synced_chat.messages.size()) <= previous_message_count)
	{
		return false;
	}

	terminal.turn_state = uam::CliTerminalTurnState::Idle;
	terminal.generation_in_progress = false;
	uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "turn_marked_idle_from_synced_history", &terminal, "message_count=" + std::to_string(synced_chat.messages.size()));

	if (synced_chat.id != selected_chat_id)
	{
		MarkChatUnseen(app, synced_chat.id);
	}

	return true;
}

inline void PollCliTerminal(CefRefPtr<CefBrowser> browser, uam::AppState& app, uam::CliTerminalState& terminal, const bool preserve_selection)
{
	constexpr std::size_t kRecentOutputBufferLimitBytes = 256 * 1024;
	constexpr double kGenerationIdleSeconds = 1.15;
	constexpr double kStructuredInputReadyFallbackSeconds = 1.5;
	constexpr int kReadBudgetChunksPerTick = 72;
	constexpr std::size_t kReadBudgetBytesPerTick = 512 * 1024;
	const ChatSession* lcp_selectedChat = ChatDomainService().SelectedChat(app);
	const std::string selected_chat_id = (lcp_selectedChat != nullptr) ? lcp_selectedChat->id : "";
	const int terminal_chat_index = ChatDomainService().FindChatIndexById(app, terminal.attached_chat_id);
	const ChatSession* terminal_chat = (terminal_chat_index >= 0) ? &app.chats[terminal_chat_index] : nullptr;
	const int previous_chat_message_count = (terminal_chat != nullptr) ? static_cast<int>(terminal_chat->messages.size()) : -1;
	const bool terminal_uses_gemini_history = (terminal_chat != nullptr) && ProviderResolutionService().ChatUsesNativeOverlayHistory(app, *terminal_chat);
	const ProviderProfile terminal_provider = (terminal_chat != nullptr) ? ProviderResolutionService().ProviderForChatOrDefault(app, *terminal_chat) : ProviderResolutionService().ActiveProviderOrDefault(app);
	const std::filesystem::path terminal_native_history_chats_dir = (terminal_chat != nullptr) ? ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, *terminal_chat) : std::filesystem::path{};
	const auto append_recent_output = [&](const char* bytes, const std::size_t len)
	{
		if (bytes == nullptr || len == 0)
		{
			return;
		}

		terminal.recent_output_bytes.append(bytes, len);

		if (terminal.recent_output_bytes.size() > kRecentOutputBufferLimitBytes)
		{
			const std::size_t trim_count = terminal.recent_output_bytes.size() - kRecentOutputBufferLimitBytes;
			terminal.recent_output_bytes.erase(0, trim_count);
		}
	};
	const IPlatformTerminalRuntime& platform_terminal_runtime = PlatformServicesFactory::Instance().terminal_runtime;

	if (!terminal.running || !platform_terminal_runtime.HasReadableTerminalOutputHandle(terminal))
		return;

	char buffer[8192];
	int chunks_read = 0;
	std::size_t bytes_read_total = 0;

	while (true)
	{
		if (chunks_read >= kReadBudgetChunksPerTick || bytes_read_total >= kReadBudgetBytesPerTick)
			break;

		const std::ptrdiff_t read_bytes = platform_terminal_runtime.ReadCliTerminalOutput(terminal, buffer, sizeof(buffer));

		if (read_bytes > 0)
		{
			++chunks_read;
			bytes_read_total += static_cast<std::size_t>(read_bytes);
			const double now = GetAppTimeSeconds();
			terminal.input_ready = true;
			terminal.last_output_time_s = now;
			terminal.last_activity_time_s = now;
			terminal.last_ai_output_time_s = now;
			terminal.generation_in_progress = true;
			append_recent_output(buffer, static_cast<std::size_t>(read_bytes));
			uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_read", &terminal, "", static_cast<long long>(read_bytes));

			// Forward raw PTY bytes to xterm.js in the React frontend.
			uam::PushCliOutput(
				browser,
				terminal.frontend_chat_id.empty() ? terminal.attached_chat_id : terminal.frontend_chat_id,
				terminal.attached_chat_id,
				terminal.terminal_id,
				std::string(buffer, static_cast<std::size_t>(read_bytes)));
			continue;
		}

		if (read_bytes == 0)
		{
			ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal exited before input was ready.");
			uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_exit_eof", &terminal);
			StopCliTerminal(terminal);
			terminal.should_launch = false;
			terminal.last_error = "Provider terminal exited.";
			app.status_line = terminal.last_error;
			break;
		}

		if (read_bytes == -2)
			break;

		ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal read failed before input was ready.");
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_read_failed", &terminal);
		StopCliTerminal(terminal);
		terminal.should_launch = false;
		app.status_line = "Provider terminal read failed.";
		break;
	}

	if (platform_terminal_runtime.PollCliTerminalProcessExited(terminal))
	{
		ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal exited before input was ready.");
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "process_exited", &terminal);

		if (!terminal.attached_chat_id.empty())
			SyncChatsFromNative(app, terminal.attached_chat_id, true);

		StopCliTerminal(terminal);
		terminal.should_launch = false;
		terminal.last_error = "Provider terminal exited.";
		app.status_line = terminal.last_error;
	}

	const double now = GetAppTimeSeconds();

	if (terminal.running && !terminal.input_ready && terminal.startup_time_s > 0.0 && (now - terminal.startup_time_s) >= kStructuredInputReadyFallbackSeconds)
		terminal.input_ready = true;

	if (terminal.running && terminal.input_ready && !terminal.pending_structured_prompts.empty())
	{
		std::string flush_error;
		if (!FlushQueuedStructuredPromptsForTerminal(terminal, &flush_error))
			app.status_line = "Provider terminal prompt flush failed: " + (flush_error.empty() ? std::string("unknown error.") : flush_error);
	}

	const bool should_refresh_native_history = terminal_uses_gemini_history && (now - terminal.last_sync_time_s > 1.25);

	if (should_refresh_native_history)
	{
		terminal.last_sync_time_s = now;

		if (platform_terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
		{
			std::vector<ChatSession> native_now;
			std::string native_load_error;
			const bool has_loaded_snapshot = ChatHistorySyncService().TryConsumeAsyncNativeChatLoad(app, native_now, native_load_error);
			ChatHistorySyncService().StartAsyncNativeChatLoad(app, terminal_provider, terminal_native_history_chats_dir);

			if (!native_load_error.empty())
				app.status_line = "Native chat refresh failed: " + native_load_error;

			if (has_loaded_snapshot && native_load_error.empty())
			{
				if (terminal.attached_session_id.empty())
				{
					std::unordered_set<std::string> blocked_ids;

					for (const auto& other_terminal : app.cli_terminals)
					{
						if (other_terminal == nullptr || other_terminal.get() == &terminal || other_terminal->attached_session_id.empty())
							continue;
						blocked_ids.insert(other_terminal->attached_session_id);
					}

					for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
					{
						if (!resolved.second.empty())
							blocked_ids.insert(resolved.second);
					}

					const std::string previous_chat_id = terminal.attached_chat_id;
					const int previous_chat_index = ChatDomainService().FindChatIndexById(app, previous_chat_id);
					std::string discovered;

					if (previous_chat_index >= 0 && NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id))
					{
						if (const auto matched = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(app.chats[previous_chat_index], native_now, blocked_ids); matched.has_value())
							discovered = matched.value();
					}
					else
					{
						const std::vector<std::string> candidates = NativeSessionLinkService().CollectNewSessionIds(native_now, terminal.session_ids_before);
						discovered = NativeSessionLinkService().PickFirstUnblockedSessionId(candidates, blocked_ids);
					}

					if (!discovered.empty())
					{
						const std::string previous_session_id = terminal.attached_session_id;
						if (previous_chat_index >= 0 &&
						    NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id) &&
						    !NativeSessionLinkService().HasRealNativeSessionId(app.chats[previous_chat_index]))
						{
							ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, app.chats[previous_chat_index], discovered);
						}

						terminal.attached_session_id = discovered;
						terminal.attached_chat_id = discovered;
						uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "native_session_rebound", &terminal, "previous_chat_id=" + previous_chat_id + ", previous_session_id=" + previous_session_id + ", discovered=" + discovered);

						if (!previous_chat_id.empty())
							app.resolved_native_sessions_by_chat_id[previous_chat_id] = discovered;
					}
				}

				const std::string preferred_id = terminal.attached_session_id.empty() ? terminal.attached_chat_id : terminal.attached_session_id;
				SyncChatsFromLoadedNative(app, std::move(native_now), preferred_id, preserve_selection);
				(void)TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
			}
		}
		else
		{
			const std::vector<ChatSession> native_now = ChatHistorySyncService().LoadNativeSessionChats(terminal_native_history_chats_dir, terminal_provider);

			if (terminal.attached_session_id.empty())
			{
				std::unordered_set<std::string> blocked_ids;

				for (const auto& other_terminal : app.cli_terminals)
				{
					if (other_terminal == nullptr || other_terminal.get() == &terminal || other_terminal->attached_session_id.empty())
						continue;
					blocked_ids.insert(other_terminal->attached_session_id);
				}

				for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
				{
					if (!resolved.second.empty())
						blocked_ids.insert(resolved.second);
				}

				const std::string previous_chat_id = terminal.attached_chat_id;
				const int previous_chat_index = ChatDomainService().FindChatIndexById(app, previous_chat_id);
				std::string discovered;

				if (previous_chat_index >= 0 && NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id))
				{
					if (const auto matched = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(app.chats[previous_chat_index], native_now, blocked_ids); matched.has_value())
						discovered = matched.value();
				}
				else
				{
					const std::vector<std::string> candidates = NativeSessionLinkService().CollectNewSessionIds(native_now, terminal.session_ids_before);
					discovered = NativeSessionLinkService().PickFirstUnblockedSessionId(candidates, blocked_ids);
				}

				if (!discovered.empty())
				{
					const std::string previous_session_id = terminal.attached_session_id;
					if (previous_chat_index >= 0 &&
					    NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id) &&
					    !NativeSessionLinkService().HasRealNativeSessionId(app.chats[previous_chat_index]))
					{
						ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, app.chats[previous_chat_index], discovered);
					}

					terminal.attached_session_id = discovered;
					terminal.attached_chat_id = discovered;
					uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "native_session_rebound", &terminal, "previous_chat_id=" + previous_chat_id + ", previous_session_id=" + previous_session_id + ", discovered=" + discovered);

					if (!previous_chat_id.empty())
						app.resolved_native_sessions_by_chat_id[previous_chat_id] = discovered;
				}
			}

			const std::string preferred_id = terminal.attached_session_id.empty() ? terminal.attached_chat_id : terminal.attached_session_id;
			SyncChatsFromNative(app, preferred_id, preserve_selection);
			(void)TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
		}
	}

	if (terminal.running && terminal.generation_in_progress && terminal.last_output_time_s > 0.0 && (now - terminal.last_output_time_s) > kGenerationIdleSeconds)
	{
		terminal.generation_in_progress = false;
	}
}

inline void PollAllCliTerminals(CefRefPtr<CefBrowser> browser, uam::AppState& app)
{
	const ChatSession* lcp_selectedChat = ChatDomainService().SelectedChat(app);
	const std::string selected_chat_id = (lcp_selectedChat != nullptr) ? lcp_selectedChat->id : "";
	const double now = GetAppTimeSeconds();

	for (auto& terminal : app.cli_terminals)
	{
		if (terminal == nullptr)
			continue;

		const bool selected_terminal = (!selected_chat_id.empty() && terminal->attached_chat_id == selected_chat_id);
		const double min_poll_interval_s = selected_terminal ? 0.0 : 0.08;

		if (terminal->last_polled_time_s > 0.0 && (now - terminal->last_polled_time_s) < min_poll_interval_s)
			continue;

		terminal->last_polled_time_s = now;
		const bool preserve_selection = !selected_chat_id.empty() && terminal->attached_chat_id != selected_chat_id;
		PollCliTerminal(browser, app, *terminal, preserve_selection);

		if (!terminal->running || selected_terminal || terminal->attached_chat_id.empty())
			continue;

		if (HasPendingCallForChat(app, terminal->attached_chat_id) || terminal->turn_state == uam::CliTerminalTurnState::Busy)
			continue;

		const double terminal_activity = LatestCliTransportActivityTime(*terminal);
		const double idle_timeout = static_cast<double>(std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600));

		if (terminal_activity > 0.0 && (now - terminal_activity) > idle_timeout)
		{
			uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "idle_timeout_stop", terminal.get(), "idle_seconds=" + std::to_string(now - terminal_activity));
			ReportDroppedQueuedStructuredPromptsForTerminal(app, *terminal, "Provider terminal stopped due to idle timeout before queued prompt delivery.");
			StopCliTerminal(*terminal, false);
			terminal->should_launch = false;
			const std::string l_chatLabel = (terminal->attached_chat_id.size() > 36) ? (terminal->attached_chat_id.substr(0, 36) + "...") : terminal->attached_chat_id;
			app.status_line = "Stopped idle background terminal for chat " + l_chatLabel + ".";
		}
	}
}

#endif // UAM_COMMON_RUNTIME_TERMINAL_POLLING_H
