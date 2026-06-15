#pragma once

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/paths/app_paths.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"
#include "common/utils/hash_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "cef/cef_push.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam
{
	inline constexpr double kCliNativeHistoryRefreshIntervalSeconds = 1.25;

	inline double LatestCliTransportActivityTime(const uam::CliTerminalState& terminal)
	{
		return std::max(terminal.last_user_input_time_s, terminal.last_ai_output_time_s);
	}

	inline bool ProviderRecentOutputIndicatesInputPrompt(const ProviderProfile& provider, std::string_view recent_output)
	{
		if (uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kCodexCli))
		{
			return CodexCliRecentOutputIndicatesInputPrompt(recent_output);
		}

		return FallbackCliRecentOutputIndicatesInputPrompt(recent_output);
	}

	inline std::string AsyncNativeChatLoadTaskKey(std::string_view provider_id, const std::filesystem::path& chats_dir)
	{
		std::string key(provider_id);
		key += "\n";
		key += uam::paths::NormalizedPortablePathString(chats_dir);
		return key;
	}

	inline std::string NativeHistorySnapshotDigest(const std::vector<ChatSession>& chats)
	{
		std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;

		const auto hash_string = [&](const std::string& value) { uam::hashing::UpdateFnv1a64WithSeparator(hash, value); };

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
				hash_string(last_message.content);
				hash_string(std::to_string(last_message.tool_calls.size()));
			}
		}

		return uam::hashing::Hex64Padded(hash);
	}

	inline uam::platform::AsyncNativeChatLoadTask& AsyncNativeChatLoadTaskFor(uam::AppState& app, const std::string& provider_id, const std::filesystem::path& chats_dir)
	{
		return app.native_chat_load_tasks[AsyncNativeChatLoadTaskKey(provider_id, chats_dir)];
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

		const std::filesystem::path chats_dir_copy = chats_dir;
		const ProviderProfile provider_copy = provider;
		const auto load_chats = [chats_dir_copy, provider_copy](std::stop_token stop_token) { return ChatHistorySyncService().LoadNativeSessionChats(chats_dir_copy, provider_copy, stop_token); };
		const auto build_digest = [](const std::vector<ChatSession>& chats) { return NativeHistorySnapshotDigest(chats); };
		return uam::platform::StartAsyncNativeChatLoadTask(task, provider.id, chats_dir, load_chats, build_digest);
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

		const bool consumed = uam::platform::TryConsumeAsyncNativeChatLoadTask(task, chats_out, &digest_out, error_out);
		if (task.running)
		{
			return false;
		}

		app.native_chat_load_tasks.erase(it);
		return consumed;
	}

	inline bool TryMarkCliTurnCompleteFromSyncedHistory(uam::AppState& app, uam::CliTerminalState& terminal, int previous_message_count, const std::string& selected_chat_id)
	{
		if (terminal.lifecycle_state != uam::CliTerminalLifecycleState::Busy)
		{
			return false;
		}

		if (previous_message_count < 0)
		{
			return false;
		}

		const ChatSession* synced_chat = FindChatForCliTerminal(app, terminal);
		if (synced_chat == nullptr)
		{
			return false;
		}

		if (static_cast<int>(synced_chat->messages.size()) <= previous_message_count)
		{
			return false;
		}

		uam::MarkCliTerminalTurnIdle(terminal);
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "turn_marked_idle_from_synced_history", &terminal, "message_count=" + std::to_string(synced_chat->messages.size()));

		if (synced_chat->id != selected_chat_id)
		{
			uam::MarkChatUnseen(app, synced_chat->id);
		}

		return true;
	}

	inline std::unordered_set<std::string> BlockedNativeSessionIdsForTerminal(const uam::AppState& app, const uam::CliTerminalState& terminal)
	{
		std::unordered_set<std::string> blocked_ids;

		for (const auto& other_terminal : app.cli_terminals)
		{
			if (other_terminal == nullptr || other_terminal.get() == &terminal)
			{
				continue;
			}

			const std::string attached_session_id = CliTerminalAttachedSessionId(*other_terminal);
			if (!attached_session_id.empty())
			{
				blocked_ids.insert(attached_session_id);
			}
		}

		for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
		{
			const std::string_view resolved_session_id = uam::strings::TrimAsciiView(resolved.second);
			if (!resolved_session_id.empty())
			{
				blocked_ids.emplace(resolved_session_id.data(), resolved_session_id.size());
			}
		}

		return blocked_ids;
	}

	inline bool TryAttachNativeSessionFromHistory(uam::AppState& app, uam::CliTerminalState& terminal, const std::vector<ChatSession>& native_chats)
	{
		if (!CliTerminalAttachedSessionId(terminal).empty())
		{
			return false;
		}

		const NativeSessionLinkService native_session_linker;
		const std::unordered_set<std::string> blocked_ids = BlockedNativeSessionIdsForTerminal(app, terminal);
		const std::string previous_chat_id = CliTerminalAttachedChatId(terminal);
		ChatSession* previous_chat = ChatDomainService().FindChatById(app, previous_chat_id);
		std::string discovered;

		if (previous_chat != nullptr && native_session_linker.IsLocalDraftChatId(previous_chat_id))
		{
			if (const auto matched = native_session_linker.MatchNativeSessionIdForLocalDraft(*previous_chat, native_chats, blocked_ids))
			{
				discovered = *matched;
			}
		}
		else
		{
			const std::vector<std::string> candidates = native_session_linker.CollectNewSessionIds(native_chats, terminal.session_ids_before);
			discovered = native_session_linker.PickFirstUnblockedSessionId(candidates, blocked_ids);
		}

		if (discovered.empty())
		{
			return false;
		}

		const std::string previous_session_id = CliTerminalAttachedSessionId(terminal);
		if (previous_chat != nullptr && native_session_linker.IsLocalDraftChatId(previous_chat_id) && !native_session_linker.HasRealNativeSessionId(*previous_chat))
		{
			(void)ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, *previous_chat, discovered);
		}

		terminal.attached_session_id = discovered;
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "native_session_rebound", &terminal, "previous_chat_id=" + previous_chat_id + ", previous_session_id=" + previous_session_id + ", discovered=" + discovered);

		if (!previous_chat_id.empty())
		{
			app.resolved_native_sessions_by_chat_id[previous_chat_id] = discovered;
		}

		return true;
	}

	inline bool ShouldAttemptOpenCodeLocalHistoryRebind(const ProviderProfile& terminal_provider, const uam::CliTerminalState& terminal, const std::vector<ChatSession>& matching_chats);
	inline bool ShouldPollOpenCodeLocalHistoryRebind(const ProviderProfile& terminal_provider, const uam::CliTerminalState& terminal, const std::vector<ChatSession>& matching_chats)
	{
		return ShouldAttemptOpenCodeLocalHistoryRebind(terminal_provider, terminal, matching_chats) || terminal.session_ids_before.empty();
	}

	inline bool PersistRebindDiscoveredSession(uam::AppState& app, const ProviderProfile& terminal_provider, uam::CliTerminalState& terminal, ChatSession* previous_chat, std::string_view discovered, std::string_view previous_session_id, std::string_view event_kind)
	{
		const NativeSessionLinkService native_session_linker;

		ChatSession persisted_previous_chat = *previous_chat;
		uam::provider_ids::NormalizeLegacyLocalHistoryChatProvider(persisted_previous_chat.provider_id, terminal_provider.id);

		if (native_session_linker.IsLocalDraftChatId(previous_chat->id) && !native_session_linker.HasRealNativeSessionId(*previous_chat))
		{
			if (!ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, persisted_previous_chat, std::string(discovered)))
			{
				return false;
			}
		}
		else
		{
			persisted_previous_chat.native_session_id = std::string(discovered);
			persisted_previous_chat.updated_at = uam::time::TimestampNow();
			if (!ChatRepository::SaveChat(app.data_root, persisted_previous_chat))
			{
				return false;
			}
		}

		*previous_chat = persisted_previous_chat;
		terminal.attached_session_id = std::string(discovered);
		app.resolved_native_sessions_by_chat_id[previous_chat->id] = std::string(discovered);
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", event_kind, &terminal, "previous_session_id=" + std::string(previous_session_id) + ", discovered=" + std::string(discovered));
		return true;
	}


	inline bool TryAttachOpenCodeLocalHistorySessionFromChatFile(uam::AppState& app, const ProviderProfile& terminal_provider, uam::CliTerminalState& terminal, const std::vector<ChatSession>& matching_chats)
	{
		if (!uam::provider_ids::IsCliProviderAliasOf(terminal_provider.id, uam::provider_ids::kOpenCodeCli))
		{
			return false;
		}

		const std::string previous_chat_id = CliTerminalAttachedChatId(terminal);
		ChatSession* previous_chat = ChatDomainService().FindChatById(app, previous_chat_id);
		if (previous_chat == nullptr)
		{
			return false;
		}

		const NativeSessionLinkService native_session_linker;
		const std::string previous_workspace_directory = previous_chat->workspace_directory;
		const std::string previous_session_id = CliTerminalAttachedSessionId(terminal);

		const auto loaded_match = std::ranges::find_if(matching_chats, [&](const ChatSession& chat) {
			if (uam::strings::Trim(chat.id) != previous_chat_id)
			{
				return false;
			}

			if (!previous_workspace_directory.empty() && !FolderDirectoryMatches(chat.workspace_directory, previous_workspace_directory))
			{
				return false;
			}

			return native_session_linker.HasRealNativeSessionId(chat);
		});

		if (loaded_match == matching_chats.end())
		{
			return false;
		}

		const std::string discovered = native_session_linker.RealNativeSessionId(*loaded_match);
		if (discovered.empty() || discovered == previous_session_id)
		{
			return false;
		}

		if (!PersistRebindDiscoveredSession(app, terminal_provider, terminal, previous_chat, discovered, previous_session_id, "local_history_session_rebound_from_chat_file"))
		{
			return false;
		}

		return true;
	}


	inline bool LocalHistoryChatMatchesTerminalProvider(const ChatSession& chat, const ProviderProfile& terminal_provider)
	{
		return uam::provider_ids::IsLegacyOpenCodeLocalHistoryProviderId(chat.provider_id, terminal_provider.id);
	}


	inline bool TryAttachLocalHistorySessionFromChats(uam::AppState& app, const ProviderProfile& terminal_provider, uam::CliTerminalState& terminal, const std::vector<ChatSession>& local_chats)
	{
		const std::string previous_chat_id = CliTerminalAttachedChatId(terminal);
		ChatSession* previous_chat = ChatDomainService().FindChatById(app, previous_chat_id);
		if (previous_chat == nullptr)
		{
			return false;
		}

		const NativeSessionLinkService native_session_linker;
		if (!native_session_linker.IsLocalDraftChatId(previous_chat->id) && !LocalHistoryChatMatchesTerminalProvider(*previous_chat, terminal_provider))
		{
			return false;
		}

		std::vector<ChatSession> matching_chats;
		matching_chats.reserve(local_chats.size());
		const std::string previous_workspace_directory = previous_chat->workspace_directory;

		for (const ChatSession& local_chat : local_chats)
		{
			if (!LocalHistoryChatMatchesTerminalProvider(local_chat, terminal_provider))
			{
				continue;
			}

			if (!previous_workspace_directory.empty() && !FolderDirectoryMatches(local_chat.workspace_directory, previous_workspace_directory))
			{
				continue;
			}

			matching_chats.push_back(local_chat);
		}

		if (!ShouldAttemptOpenCodeLocalHistoryRebind(terminal_provider, terminal, matching_chats))
		{
			return TryAttachOpenCodeLocalHistorySessionFromChatFile(app, terminal_provider, terminal, matching_chats);
		}
		const std::unordered_set<std::string> blocked_ids = BlockedNativeSessionIdsForTerminal(app, terminal);

		const std::string attached_session_id = CliTerminalAttachedSessionId(terminal);
		if (!attached_session_id.empty() && native_session_linker.SessionIdExistsInLoadedChats(matching_chats, attached_session_id))
		{
			return false;
		}

		const std::vector<std::string> candidates = native_session_linker.CollectNewSessionIds(matching_chats, terminal.session_ids_before);
		const std::string discovered = native_session_linker.PickFirstUnblockedSessionId(candidates, blocked_ids);
		if (discovered.empty())
		{
			return false;
		}

		const std::string previous_session_id = CliTerminalAttachedSessionId(terminal);

		if (!PersistRebindDiscoveredSession(app, terminal_provider, terminal, previous_chat, discovered, previous_session_id, "local_history_session_rebound"))
		{
			return false;
		}

		return true;
	}

	inline bool ShouldAttemptOpenCodeLocalHistoryRebind(const ProviderProfile& terminal_provider, const uam::CliTerminalState& terminal, const std::vector<ChatSession>& matching_chats)
	{
		if (!uam::provider_ids::IsCliProviderAliasOf(terminal_provider.id, uam::provider_ids::kOpenCodeCli) || terminal.session_ids_before.empty())
		{
			return false;
		}

		const std::string attached_session_id = CliTerminalAttachedSessionId(terminal);
		if (attached_session_id.empty())
		{
			return true;
		}

		return !NativeSessionLinkService().SessionIdExistsInLoadedChats(matching_chats, attached_session_id);
	}

	inline bool UpdateNativeHistorySnapshotDigestIfChanged(uam::CliTerminalState& terminal, std::string_view native_snapshot_digest)
	{
		if (native_snapshot_digest == terminal.last_native_history_snapshot_digest)
		{
			return false;
		}

		terminal.last_native_history_snapshot_digest.assign(native_snapshot_digest);
		return true;
	}

	inline void StopCliTerminalAfterProviderExit(uam::AppState& app, uam::CliTerminalState& terminal, bool was_shutting_down)
	{
		uam::StopCliTerminal(terminal);
		terminal.should_launch = false;

		if (was_shutting_down)
		{
			terminal.last_error.clear();
			return;
		}

		terminal.last_error = "Provider terminal exited.";
		app.status_line = terminal.last_error;
	}

	inline bool PollCliTerminal(CefRefPtr<CefBrowser> browser, uam::AppState& app, uam::CliTerminalState& terminal, bool preserve_selection)
	{
		constexpr std::size_t kRecentOutputBufferLimitBytes = 256 * 1024;
		constexpr double kInputReadyFallbackSeconds = 1.5;
		constexpr int kReadBudgetChunksPerTick = 72;
		constexpr std::size_t kReadBudgetBytesPerTick = 512 * 1024;

		bool changed = false;
		const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
		const ChatSession* terminal_chat = FindChatForCliTerminal(app, terminal);
		const int previous_chat_message_count = (terminal_chat != nullptr) ? static_cast<int>(terminal_chat->messages.size()) : -1;
		const bool terminal_uses_native_history = (terminal_chat != nullptr) && ProviderResolutionService().ChatUsesNativeOverlayHistory(app, *terminal_chat);
		const ProviderProfile terminal_provider = (terminal_chat != nullptr) ? ProviderResolutionService().ProviderForChatOrDefault(app, *terminal_chat) : ProviderResolutionService().ActiveProviderOrDefault(app);
		const std::filesystem::path terminal_native_history_chats_dir = (terminal_chat != nullptr) ? ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, *terminal_chat) : std::filesystem::path{};
		const auto append_recent_output = [&](const char* bytes, std::size_t len)
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
		std::string output_for_frontend;

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
				output_for_frontend.append(buffer, static_cast<std::size_t>(read_bytes));
				uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_read", &terminal, "", static_cast<long long>(read_bytes));
				continue;
			}

			if (read_bytes == 0)
			{
				const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
				uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_exit_eof", &terminal);
				StopCliTerminalAfterProviderExit(app, terminal, was_shutting_down);
				changed = true;
				break;
			}

			if (read_bytes == -2)
			{
				break;
			}

			const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
			uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_read_failed", &terminal);
			uam::StopCliTerminal(terminal);
			terminal.should_launch = false;
			if (!was_shutting_down)
			{
				app.status_line = "Provider terminal read failed.";
			}
			changed = true;
			break;
		}

		if (!output_for_frontend.empty())
		{
			const std::string primary_chat_id = CliTerminalPrimaryChatId(terminal);
			uam::PushCliOutput(browser, primary_chat_id, primary_chat_id, terminal.terminal_id, output_for_frontend);
		}

		const bool terminal_uses_codex_cli = uam::provider_ids::IsCliProviderAliasOf(terminal_provider.id, uam::provider_ids::kCodexCli);
		const bool prompt_indicates_idle = ProviderRecentOutputIndicatesInputPrompt(terminal_provider, terminal.recent_output_bytes);
		if (terminal.running && terminal.lifecycle_state == uam::CliTerminalLifecycleState::Busy && prompt_indicates_idle)
		{
			uam::MarkCliTerminalTurnIdle(terminal);
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
				changed |= uam::SyncChatsFromNative(app, sync_target_id, true);
			}

			StopCliTerminalAfterProviderExit(app, terminal, was_shutting_down);
			changed = true;
		}

		const double now = GetAppTimeSeconds();

		if (terminal.running && !terminal.input_ready && terminal.startup_time_s > 0.0 && (now - terminal.startup_time_s) >= kInputReadyFallbackSeconds)
		{
			terminal.input_ready = true;
			changed = true;
		}

		const bool sync_interval_elapsed = now - terminal.last_sync_time_s > kCliNativeHistoryRefreshIntervalSeconds;
		const bool should_refresh_native_history = terminal_uses_native_history && sync_interval_elapsed;
		if (terminal.running && !terminal_uses_native_history && terminal_uses_codex_cli && CliTerminalAttachedSessionId(terminal).empty() && terminal_chat != nullptr && sync_interval_elapsed)
		{
			terminal.last_sync_time_s = now;
			ChatSession* codex_chat = FindChatForCliTerminal(app, terminal);
			if (codex_chat == nullptr)
			{
				return changed;
			}

			const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, *codex_chat);
			const std::string discovered = uam::codex::PickNewSessionId(terminal.session_ids_before, workspace_root);
			if (!discovered.empty())
			{
				codex_chat->native_session_id = discovered;
				codex_chat->updated_at = uam::time::TimestampNow();
				terminal.attached_session_id = discovered;
				app.resolved_native_sessions_by_chat_id[codex_chat->id] = discovered;
				(void)ProviderRuntime::SaveHistory(terminal_provider, app.data_root, *codex_chat);
				uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "codex_session_rebound", &terminal, "discovered=" + discovered);
				changed = true;
			}
		}
		else if (terminal.running && !terminal_uses_native_history && uam::provider_ids::IsCliProviderAliasOf(terminal_provider.id, uam::provider_ids::kOpenCodeCli) && terminal_chat != nullptr && sync_interval_elapsed)
		{
			const std::vector<ChatSession> local_now = ChatRepository::LoadLocalChats(app.data_root);
			const bool retry_without_snapshot = terminal.session_ids_before.empty();
			if (!retry_without_snapshot)
			{
				terminal.last_sync_time_s = now;
			}
			if (ShouldPollOpenCodeLocalHistoryRebind(terminal_provider, terminal, local_now) && TryAttachLocalHistorySessionFromChats(app, terminal_provider, terminal, local_now))
			{
				if (retry_without_snapshot)
				{
					terminal.last_sync_time_s = now;
				}
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

				if (has_loaded_snapshot && native_load_error.empty() && UpdateNativeHistorySnapshotDigestIfChanged(terminal, native_snapshot_digest))
				{
					changed |= TryAttachNativeSessionFromHistory(app, terminal, native_now);

					const std::string preferred_id = CliTerminalSyncTargetId(terminal);
					changed |= uam::SyncChatsFromLoadedNative(app, std::move(native_now), preferred_id, preserve_selection);
					changed |= TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
				}
			}
			else
			{
				const std::vector<ChatSession> native_now = ChatHistorySyncService().LoadNativeSessionChats(terminal_native_history_chats_dir, terminal_provider);
				const std::string native_snapshot_digest = NativeHistorySnapshotDigest(native_now);

				if (UpdateNativeHistorySnapshotDigestIfChanged(terminal, native_snapshot_digest))
				{
					changed |= TryAttachNativeSessionFromHistory(app, terminal, native_now);

					const std::string preferred_id = CliTerminalSyncTargetId(terminal);
					changed |= uam::SyncChatsFromNative(app, preferred_id, preserve_selection);
					changed |= TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
				}
			}
		}

		return changed;
	}

	inline bool PollAllCliTerminals(CefRefPtr<CefBrowser> browser, uam::AppState& app)
	{
		constexpr double kShutdownFallbackSeconds = 2.5;
		const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
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
					uam::StopCliTerminal(*terminal, false, uam::CliTerminalStopMode::FastExit);
					terminal->should_launch = false;
					terminal->last_error.clear();
					changed = true;
				}
				continue;
			}

			if (uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, *terminal, selected_chat_id, now))
			{
				const double idle_seconds = now - terminal->last_idle_confirmed_time_s;
				uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "idle_timeout_shutdown", terminal.get(), "idle_seconds=" + std::to_string(idle_seconds));
				uam::BeginCliTerminalIdleShutdown(*terminal);
				terminal->should_launch = false;
				const std::string primary_chat_id = CliTerminalPrimaryChatId(*terminal);
				const std::string chat_label = (primary_chat_id.size() > 36) ? (primary_chat_id.substr(0, 36) + "...") : primary_chat_id;
				app.status_line = "Stopping idle background terminal for chat " + chat_label + ".";
				changed = true;
			}
		}

		return changed;
	}

} // namespace uam
