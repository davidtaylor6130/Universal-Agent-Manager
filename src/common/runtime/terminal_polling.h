#ifndef UAM_COMMON_RUNTIME_TERMINAL_POLLING_H
#define UAM_COMMON_RUNTIME_TERMINAL_POLLING_H

#include "cef/cef_push.h"
#include "common/runtime/app_time.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <sstream>
#include <iomanip>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/chat_domain_service.h"
#include "app/application_core_helpers.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"

inline double LatestCliTransportActivityTime(const uam::CliTerminalState& terminal)
{
	return std::max(terminal.last_user_input_time_s, terminal.last_ai_output_time_s);
}

inline std::string AsyncNativeChatLoadTaskKey(const std::string& provider_id, const std::filesystem::path& chats_dir)
{
	return provider_id + "\n" + chats_dir.lexically_normal().generic_string();
}

inline std::string NativeHistorySnapshotDigest(const std::vector<ChatSession>& chats)
{
	std::uint64_t hash = 1469598103934665603ull;

	const auto hash_bytes = [&](const unsigned char* data, const std::size_t len)
	{
		for (std::size_t i = 0; i < len; ++i)
		{
			hash ^= static_cast<std::uint64_t>(data[i]);
			hash *= 1099511628211ull;
		}
	};

	const auto hash_string = [&](const std::string& value)
	{
		hash_bytes(reinterpret_cast<const unsigned char*>(value.data()), value.size());
		const unsigned char separator = 0xFF;
		hash_bytes(&separator, 1);
	};

	for (const ChatSession& chat : chats)
	{
		hash_string(chat.id);
		hash_string(chat.native_session_id);
		hash_string(chat.provider_id);
		hash_string(chat.folder_id);
		hash_string(chat.updated_at);
		hash_string(std::to_string(chat.messages.size()));

		if (!chat.messages.empty())
		{
			const Message& last_message = chat.messages.back();
			hash_string(std::to_string(static_cast<int>(last_message.role)));
			hash_string(last_message.created_at);
			hash_string(last_message.provider);
			hash_string(std::to_string(last_message.content.size()));
			hash_string(std::to_string(last_message.tool_calls.size()));
		}
	}

	std::ostringstream out;
	out << std::hex << std::setw(16) << std::setfill('0') << hash;
	return out.str();
}

inline uam::platform::AsyncNativeChatLoadTask& AsyncNativeChatLoadTaskFor(uam::AppState& app, const std::string& provider_id, const std::filesystem::path& chats_dir)
{
	return app.native_chat_load_tasks[AsyncNativeChatLoadTaskKey(provider_id, chats_dir)];
}

inline void ResetAsyncNativeChatLoadTask(uam::platform::AsyncNativeChatLoadTask& task)
{
	if (task.worker != nullptr)
	{
		task.worker->request_stop();
		task.worker.reset();
	}

	task.running = false;
	task.provider_id_snapshot.clear();
	task.chats_dir_snapshot.clear();
	task.state.reset();
}

inline bool StartAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir)
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!ProviderRuntime::UsesNativeOverlayHistory(provider) || chats_dir.empty())
	{
		return false;
	}

	uam::platform::AsyncNativeChatLoadTask& task = AsyncNativeChatLoadTaskFor(app, provider.id, chats_dir);

	if (task.running)
	{
		return false;
	}

	ResetAsyncNativeChatLoadTask(task);
	task.running = true;
	task.provider_id_snapshot = provider.id;
	task.chats_dir_snapshot = chats_dir.string();
	task.state = std::make_shared<uam::platform::AsyncNativeChatLoadTask::State>();
	std::shared_ptr<uam::platform::AsyncNativeChatLoadTask::State> state = task.state;
	const std::filesystem::path chats_dir_copy = chats_dir;
	const ProviderProfile provider_copy = provider;
	task.worker = std::make_unique<std::jthread>(
	    [chats_dir_copy, provider_copy, state](std::stop_token stop_token)
	    {
		    try
		    {
			    state->chats = ChatHistorySyncService().LoadNativeSessionChats(chats_dir_copy, provider_copy, stop_token);
			    state->snapshot_digest = NativeHistorySnapshotDigest(state->chats);
		    }
		    catch (const std::exception& ex)
		    {
			    state->error = ex.what();
		    }
		    catch (...)
		    {
			    state->error = "Unknown native chat load failure.";
		    }

		    state->completed.store(true, std::memory_order_release);
	    });
	return true;
}

inline bool TryConsumeAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir, std::vector<ChatSession>& chats_out, std::string& digest_out, std::string& error_out)
{
	const std::string key = AsyncNativeChatLoadTaskKey(provider.id, chats_dir);
	const auto it = app.native_chat_load_tasks.find(key);

	if (it == app.native_chat_load_tasks.end())
	{
		return false;
	}

	uam::platform::AsyncNativeChatLoadTask& task = it->second;

	if (!task.running)
	{
		ResetAsyncNativeChatLoadTask(task);
		app.native_chat_load_tasks.erase(it);
		return false;
	}

	if (task.state == nullptr)
	{
		ResetAsyncNativeChatLoadTask(task);
		app.native_chat_load_tasks.erase(it);
		chats_out.clear();
		digest_out.clear();
		error_out.clear();
		return true;
	}

	if (!task.state->completed.load(std::memory_order_acquire))
	{
		return false;
	}

	chats_out = task.state->chats;
	digest_out = task.state->snapshot_digest;
	error_out = task.state->error;
	ResetAsyncNativeChatLoadTask(task);
	app.native_chat_load_tasks.erase(it);
	return true;
}

inline bool TryMarkCliTurnCompleteFromSyncedHistory(uam::AppState& app, uam::CliTerminalState& terminal, const int previous_message_count, const std::string& selected_chat_id)
{
	if (terminal.lifecycle_state != uam::CliTerminalLifecycleState::Busy)
	{
		return false;
	}

	if (previous_message_count < 0)
	{
		return false;
	}

	const int synced_chat_index = FindChatIndexForCliTerminal(app, terminal);

	if (synced_chat_index < 0)
	{
		return false;
	}

	const ChatSession& synced_chat = app.chats[static_cast<std::size_t>(synced_chat_index)];

	if (static_cast<int>(synced_chat.messages.size()) <= previous_message_count)
	{
		return false;
	}

	MarkCliTerminalTurnIdle(terminal);
	uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "turn_marked_idle_from_synced_history", &terminal, "message_count=" + std::to_string(synced_chat.messages.size()));

	if (synced_chat.id != selected_chat_id)
	{
		MarkChatUnseen(app, synced_chat.id);
	}

	return true;
}

inline bool PollCliTerminal(CefRefPtr<CefBrowser> browser, uam::AppState& app, uam::CliTerminalState& terminal, const bool preserve_selection)
{
	constexpr std::size_t kRecentOutputBufferLimitBytes = 256 * 1024;
	constexpr double kInputReadyFallbackSeconds = 1.5;
	constexpr int kReadBudgetChunksPerTick = 72;
	constexpr std::size_t kReadBudgetBytesPerTick = 512 * 1024;

	bool changed = false;
	const ChatSession* lcp_selectedChat = ChatDomainService().SelectedChat(app);
	const std::string selected_chat_id = (lcp_selectedChat != nullptr) ? lcp_selectedChat->id : "";
	const int terminal_chat_index = FindChatIndexForCliTerminal(app, terminal);
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
	{
		return false;
	}

	char buffer[8192];
	int chunks_read = 0;
	std::size_t bytes_read_total = 0;

	while (true)
	{
		if (chunks_read >= kReadBudgetChunksPerTick || bytes_read_total >= kReadBudgetBytesPerTick)
		{
			break;
		}

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
			changed = true;
			append_recent_output(buffer, static_cast<std::size_t>(read_bytes));
			uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_read", &terminal, "", static_cast<long long>(read_bytes));

			uam::PushCliOutput(browser, CliTerminalPrimaryChatId(terminal), CliTerminalPrimaryChatId(terminal), terminal.terminal_id, std::string(buffer, static_cast<std::size_t>(read_bytes)));
			continue;
		}

		if (read_bytes == 0)
		{
			const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
			uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_exit_eof", &terminal);
			StopCliTerminal(terminal);
			terminal.should_launch = false;
			if (was_shutting_down)
			{
				terminal.last_error.clear();
			}
			else
			{
				terminal.last_error = "Provider terminal exited.";
				app.status_line = terminal.last_error;
			}
			changed = true;
			break;
		}

		if (read_bytes == -2)
		{
			break;
		}

		const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_read_failed", &terminal);
		StopCliTerminal(terminal);
		terminal.should_launch = false;
		if (!was_shutting_down)
		{
			app.status_line = "Provider terminal read failed.";
		}
		changed = true;
		break;
	}

	const bool prompt_indicates_idle = terminal_provider.id == "codex-cli"
		? CodexCliRecentOutputIndicatesInputPrompt(terminal.recent_output_bytes)
		: GeminiCliRecentOutputIndicatesInputPrompt(terminal.recent_output_bytes);
	if (terminal.running && terminal.lifecycle_state == uam::CliTerminalLifecycleState::Busy && prompt_indicates_idle)
	{
		MarkCliTerminalTurnIdle(terminal);
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "turn_marked_idle_from_prompt", &terminal);
		changed = true;
	}

	if (terminal.running && platform_terminal_runtime.PollCliTerminalProcessExited(terminal))
	{
		const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "process_exited", &terminal);

		const std::string sync_target_id = CliTerminalSyncTargetId(terminal);
		if (!sync_target_id.empty())
		{
			changed |= SyncChatsFromNative(app, sync_target_id, true);
		}

		StopCliTerminal(terminal);
		terminal.should_launch = false;
		if (was_shutting_down)
		{
			terminal.last_error.clear();
		}
		else
		{
			terminal.last_error = "Provider terminal exited.";
			app.status_line = terminal.last_error;
		}
		changed = true;
	}

	const double now = GetAppTimeSeconds();

	if (terminal.running && !terminal.input_ready && terminal.startup_time_s > 0.0 && (now - terminal.startup_time_s) >= kInputReadyFallbackSeconds)
	{
		terminal.input_ready = true;
		changed = true;
	}

	const bool should_refresh_native_history = terminal_uses_gemini_history && (now - terminal.last_sync_time_s > 1.25);
	if (terminal.running &&
	    !terminal_uses_gemini_history &&
	    terminal_provider.id == "codex-cli" &&
	    terminal.attached_session_id.empty() &&
	    terminal_chat_index >= 0 &&
	    (now - terminal.last_sync_time_s > 1.25))
	{
		terminal.last_sync_time_s = now;
		ChatSession& codex_chat = app.chats[static_cast<std::size_t>(terminal_chat_index)];
		const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, codex_chat);
		const std::string discovered = uam::codex::PickNewSessionId(terminal.session_ids_before, workspace_root);
		if (!discovered.empty())
		{
			codex_chat.native_session_id = discovered;
			codex_chat.updated_at = TimestampNow();
			terminal.attached_session_id = discovered;
			app.resolved_native_sessions_by_chat_id[codex_chat.id] = discovered;
			(void)ProviderRuntime::SaveHistory(terminal_provider, app.data_root, codex_chat);
			uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "codex_session_rebound", &terminal, "discovered=" + discovered);
			changed = true;
		}
	}

	if (should_refresh_native_history)
	{
		terminal.last_sync_time_s = now;

		if (platform_terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
		{
			std::vector<ChatSession> native_now;
			std::string native_snapshot_digest;
			std::string native_load_error;
			const bool has_loaded_snapshot = TryConsumeAsyncNativeChatLoadForTerminal(app, terminal_provider, terminal_native_history_chats_dir, native_now, native_snapshot_digest, native_load_error);
			(void)StartAsyncNativeChatLoadForTerminal(app, terminal_provider, terminal_native_history_chats_dir);

			if (!native_load_error.empty())
			{
				app.status_line = "Native chat refresh failed: " + native_load_error;
				changed = true;
			}

			if (has_loaded_snapshot && native_load_error.empty() && native_snapshot_digest != terminal.last_native_history_snapshot_digest)
			{
				terminal.last_native_history_snapshot_digest = native_snapshot_digest;
				if (terminal.attached_session_id.empty())
				{
					std::unordered_set<std::string> blocked_ids;

					for (const auto& other_terminal : app.cli_terminals)
					{
						if (other_terminal == nullptr || other_terminal.get() == &terminal || other_terminal->attached_session_id.empty())
						{
							continue;
						}
						blocked_ids.insert(other_terminal->attached_session_id);
					}

					for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
					{
						if (!resolved.second.empty())
						{
							blocked_ids.insert(resolved.second);
						}
					}

					const std::string previous_chat_id = terminal.attached_chat_id;
					const int previous_chat_index = ChatDomainService().FindChatIndexById(app, previous_chat_id);
					std::string discovered;

					if (previous_chat_index >= 0 && NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id))
					{
						if (const auto matched = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(app.chats[previous_chat_index], native_now, blocked_ids); matched.has_value())
						{
							discovered = matched.value();
						}
					}
					else
					{
						const std::vector<std::string> candidates = NativeSessionLinkService().CollectNewSessionIds(native_now, terminal.session_ids_before);
						discovered = NativeSessionLinkService().PickFirstUnblockedSessionId(candidates, blocked_ids);
					}

					if (!discovered.empty())
					{
						const std::string previous_session_id = terminal.attached_session_id;
						if (previous_chat_index >= 0 && NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id) && !NativeSessionLinkService().HasRealNativeSessionId(app.chats[previous_chat_index]))
						{
							changed |= ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, app.chats[previous_chat_index], discovered);
						}

						terminal.attached_session_id = discovered;
						changed = true;
						uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "native_session_rebound", &terminal, "previous_chat_id=" + previous_chat_id + ", previous_session_id=" + previous_session_id + ", discovered=" + discovered);

						if (!previous_chat_id.empty())
						{
							app.resolved_native_sessions_by_chat_id[previous_chat_id] = discovered;
						}
					}
				}

				const std::string preferred_id = CliTerminalSyncTargetId(terminal);
				changed |= SyncChatsFromLoadedNative(app, std::move(native_now), preferred_id, preserve_selection);
				changed |= TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
			}
		}
		else
		{
			const std::vector<ChatSession> native_now = ChatHistorySyncService().LoadNativeSessionChats(terminal_native_history_chats_dir, terminal_provider);
			const std::string native_snapshot_digest = NativeHistorySnapshotDigest(native_now);

			if (native_snapshot_digest != terminal.last_native_history_snapshot_digest)
			{
				terminal.last_native_history_snapshot_digest = native_snapshot_digest;

				if (terminal.attached_session_id.empty())
				{
					std::unordered_set<std::string> blocked_ids;

					for (const auto& other_terminal : app.cli_terminals)
					{
						if (other_terminal == nullptr || other_terminal.get() == &terminal || other_terminal->attached_session_id.empty())
						{
							continue;
						}
						blocked_ids.insert(other_terminal->attached_session_id);
					}

					for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
					{
						if (!resolved.second.empty())
						{
							blocked_ids.insert(resolved.second);
						}
					}

					const std::string previous_chat_id = terminal.attached_chat_id;
					const int previous_chat_index = ChatDomainService().FindChatIndexById(app, previous_chat_id);
					std::string discovered;

					if (previous_chat_index >= 0 && NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id))
					{
						if (const auto matched = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(app.chats[previous_chat_index], native_now, blocked_ids); matched.has_value())
						{
							discovered = matched.value();
						}
					}
					else
					{
						const std::vector<std::string> candidates = NativeSessionLinkService().CollectNewSessionIds(native_now, terminal.session_ids_before);
						discovered = NativeSessionLinkService().PickFirstUnblockedSessionId(candidates, blocked_ids);
					}

					if (!discovered.empty())
					{
						const std::string previous_session_id = terminal.attached_session_id;
						if (previous_chat_index >= 0 && NativeSessionLinkService().IsLocalDraftChatId(previous_chat_id) && !NativeSessionLinkService().HasRealNativeSessionId(app.chats[previous_chat_index]))
						{
							changed |= ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, app.chats[previous_chat_index], discovered);
						}

						terminal.attached_session_id = discovered;
						changed = true;
						uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "native_session_rebound", &terminal, "previous_chat_id=" + previous_chat_id + ", previous_session_id=" + previous_session_id + ", discovered=" + discovered);

						if (!previous_chat_id.empty())
						{
							app.resolved_native_sessions_by_chat_id[previous_chat_id] = discovered;
						}
					}
				}

				const std::string preferred_id = CliTerminalSyncTargetId(terminal);
				changed |= SyncChatsFromNative(app, preferred_id, preserve_selection);
				changed |= TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
			}
		}
	}

	return changed;
}

inline bool PollAllCliTerminals(CefRefPtr<CefBrowser> browser, uam::AppState& app)
{
	constexpr double kShutdownFallbackSeconds = 2.5;
	const ChatSession* lcp_selectedChat = ChatDomainService().SelectedChat(app);
	const std::string selected_chat_id = (lcp_selectedChat != nullptr) ? lcp_selectedChat->id : "";
	const double now = GetAppTimeSeconds();
	bool changed = false;

	for (auto& terminal : app.cli_terminals)
	{
		if (terminal == nullptr)
		{
			continue;
		}

		const bool selected_terminal = (!selected_chat_id.empty() && CliTerminalMatchesChatId(*terminal, selected_chat_id));
		const double min_poll_interval_s = selected_terminal ? 0.05 : 0.25;

		if (terminal->last_polled_time_s > 0.0 && (now - terminal->last_polled_time_s) < min_poll_interval_s)
		{
			continue;
		}

		terminal->last_polled_time_s = now;
		const bool preserve_selection = !selected_chat_id.empty() && !CliTerminalMatchesChatId(*terminal, selected_chat_id);
		changed |= PollCliTerminal(browser, app, *terminal, preserve_selection);

		if (!terminal->running)
		{
			continue;
		}

		if (terminal->lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown)
		{
			if (terminal->shutdown_requested_time_s > 0.0 && (now - terminal->shutdown_requested_time_s) >= kShutdownFallbackSeconds)
			{
				uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "idle_shutdown_force_stop", terminal.get(), "shutdown_wait_seconds=" + std::to_string(now - terminal->shutdown_requested_time_s));
				StopCliTerminal(*terminal, false, CliTerminalStopMode::FastExit);
				terminal->should_launch = false;
				terminal->last_error.clear();
				changed = true;
			}
			continue;
		}

		if (IsCliTerminalEligibleForBackgroundIdleShutdown(app, *terminal, selected_chat_id, now))
		{
			const double idle_seconds = now - terminal->last_idle_confirmed_time_s;
			uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "idle_timeout_shutdown", terminal.get(), "idle_seconds=" + std::to_string(idle_seconds));
			BeginCliTerminalIdleShutdown(*terminal);
			terminal->should_launch = false;
			const std::string primary_chat_id = CliTerminalPrimaryChatId(*terminal);
			const std::string l_chatLabel = (primary_chat_id.size() > 36) ? (primary_chat_id.substr(0, 36) + "...") : primary_chat_id;
			app.status_line = "Stopping idle background terminal for chat " + l_chatLabel + ".";
			changed = true;
		}
	}

	return changed;
}

#endif // UAM_COMMON_RUNTIME_TERMINAL_POLLING_H
