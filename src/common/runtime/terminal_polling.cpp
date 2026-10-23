#include "common/runtime/terminal_polling.h"

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/config/execution_host_config.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/hash_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam
{

double LatestCliTransportActivityTime(const uam::CliTerminalState& terminal)
{
	return std::max(terminal.last_user_input_time_s, terminal.last_ai_output_time_s);
}

std::string AsyncNativeChatLoadTaskKey(std::string_view provider_id, const std::filesystem::path& chats_dir)
{
	std::string key(provider_id);
	key += "\n";
	key += uam::paths::NormalizedPortablePathString(chats_dir);
	return key;
}

std::string NativeHistorySnapshotDigest(const std::vector<ChatSession>& chats)
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

		hash_string(uam::StateSerializer::MessageDigest(chat));
	}

	return uam::hashing::Hex64Padded(hash);
}

uam::platform::AsyncNativeChatLoadTask& AsyncNativeChatLoadTaskFor(uam::AppState& app, const std::string& provider_id, const std::filesystem::path& chats_dir)
{
	return app.native_chat_load_tasks[uam::paths::NormalizedPortablePathString(app.data_root) + "\n" + AsyncNativeChatLoadTaskKey(provider_id, chats_dir)];
}

bool TerminalNativeChatLoadOwnerCurrent(const uam::AppState& app, const platform::AsyncNativeChatLoadTask::State& state)
{
	return state.admission_generation == app.terminal_native_history_load_generation &&
	    (!state.worker_retirement_owned || state.owner_root == app.data_root);
}

namespace
{
	std::array<std::string_view, 10> TerminalParserWorkspaceMetadata(const uam::AppState& app, const ChatSession& chat)
	{
		const auto* folder = ChatDomainService().FindFolderById(app, chat.folder_id);
		return {chat.execution_host_id, chat.workspace_directory, chat.workspace_isolation_kind, chat.workspace_source_directory,
		    chat.workspace_base_ref, chat.workspace_branch_name, chat.workspace_worktree_directory, chat.folder_id,
		    folder ? std::string_view(folder->directory) : std::string_view{}, folder ? std::string_view(folder->execution_host_id) : std::string_view{}};
	}

	void CaptureTerminalNativeChatLoadOwners(uam::AppState& app, platform::AsyncNativeChatLoadTask& task)
	{
		if (!task.state || !task.state->worker_retirement_owned || !TerminalNativeChatLoadOwnerCurrent(app, *task.state)) return;
		std::vector<platform::AsyncNativeChatLoadTask::State::TerminalOwnerObservation> owners;
		const auto key = AsyncNativeChatLoadTaskKey(task.provider_id_snapshot, std::filesystem::path(task.chats_dir_snapshot));
		for (const auto& terminal : app.cli_terminals)
		{
			if (!terminal || !terminal->running) continue;
			const auto* chat = FindChatForCliTerminal(app, *terminal);
			if (!chat || !uam::paths::IsControllerLocalWorkspace(*chat)) continue;
			const auto& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *chat);
			if (provider.id != task.provider_id_snapshot || !ProviderRuntime::UsesNativeOverlayHistory(provider) ||
			    AsyncNativeChatLoadTaskKey(provider.id, ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, *chat)) != key) continue;
			auto& observed = owners.emplace_back();
			observed.row_identity = reinterpret_cast<std::uintptr_t>(terminal.get());
			observed.terminal_id = terminal->terminal_id; observed.startup_time_s = terminal->startup_time_s;
			observed.chat_id = chat->id; observed.chat_created_at = chat->created_at;
			observed.chat_incarnation = chat->metadata_incarnation; observed.provider_id = provider.id;
			const auto workspace = TerminalParserWorkspaceMetadata(app, *chat);
			for (std::size_t index = 0; index < workspace.size(); ++index) observed.workspace[index] = workspace[index];
		}
		if (!owners.empty()) task.state->terminal_owner_observed = true;
		// Directory/registry lookup occurs only at existing Start/Consume cadence.
		// Capture every same-key row now, including one before its first poll.
		task.state->terminal_owners.swap(owners);
	}

	bool TerminalNativeChatLoadOrphaned(const uam::AppState& app, const platform::AsyncNativeChatLoadTask& task)
	{
		if (!task.state || !task.state->worker_retirement_owned || !task.state->terminal_owner_observed) return false;
		return std::ranges::none_of(task.state->terminal_owners, [&](const auto& observed)
		{
			const auto row = std::ranges::find_if(app.cli_terminals, [&](const auto& terminal)
			{
				return terminal && terminal->running && reinterpret_cast<std::uintptr_t>(terminal.get()) == observed.row_identity &&
				    terminal->terminal_id == observed.terminal_id && terminal->startup_time_s == observed.startup_time_s;
			});
			if (row == app.cli_terminals.end()) return false;
			const auto* chat = FindChatForCliTerminal(app, **row);
			if (!chat || chat->id != observed.chat_id || chat->created_at != observed.chat_created_at ||
			    chat->metadata_incarnation != observed.chat_incarnation.lock() ||
			    ProviderResolutionService().ProviderForChatOrDefault(app, *chat).id != observed.provider_id) return false;
			const auto workspace = TerminalParserWorkspaceMetadata(app, *chat);
			return std::equal(workspace.begin(), workspace.end(), observed.workspace.begin(),
			    [](std::string_view current, const std::string& saved) { return current == saved; });
		});
	}
}

void SweepTerminalNativeChatLoads(uam::AppState& app)
{
	for (auto it = app.native_chat_load_tasks.begin(); it != app.native_chat_load_tasks.end();)
	{
		auto& task = it->second;
		const bool orphaned = TerminalNativeChatLoadOrphaned(app, task);
		if (!task.state)
		{
			// Empty lookup entries need no worker join. An unexpected running
			// worker without its observer is stop-requested, never joined here.
			if (!task.worker) { it = app.native_chat_load_tasks.erase(it); continue; }
			task.worker->request_stop();
		}
		else if (!TerminalNativeChatLoadOwnerCurrent(app, *task.state) || orphaned)
		{
			if (task.worker) task.worker->request_stop();
			if (task.state->completed.load(std::memory_order_acquire))
			{
				platform::ResetAsyncNativeChatLoadTask(task);
				it = app.native_chat_load_tasks.erase(it);
				continue;
			}
		}
		++it;
	}
}

void InvalidateTerminalNativeChatLoads(uam::AppState& app)
{
	++app.terminal_native_history_load_generation;
	for (auto& [_, task] : app.native_chat_load_tasks)
		if (task.worker) task.worker->request_stop();
	SweepTerminalNativeChatLoads(app); // Only acquire-completed parsers may reset/join.
}

bool StartAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir)
{
	// A final capture invalidates earlier parser cohorts and must not be
	// starved by newer completed/orphaned parses occupying both lifetime slots.
	if (ChatHistorySyncService().TerminalNativeMediaFinalPending(app)) return false;
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
		CaptureTerminalNativeChatLoadOwners(app, task);
		return false;
	}

	const std::filesystem::path chats_dir_copy = chats_dir;
	const ProviderProfile provider_copy = provider;
	const auto load_chats = [chats_dir_copy, provider_copy](std::stop_token stop_token) { return ChatHistorySyncService().LoadNativeSessionChats(chats_dir_copy, provider_copy, stop_token); };
	const auto build_digest = [](const std::vector<ChatSession>& chats) { return NativeHistorySnapshotDigest(chats); };
	const bool started = uam::platform::StartAsyncNativeChatLoadTask(task, provider.id, chats_dir, load_chats, build_digest, true);
	if (started && task.state)
	{
		task.state->admission_generation = app.terminal_native_history_load_generation;
		task.state->owner_root = app.data_root;
		CaptureTerminalNativeChatLoadOwners(app, task);
	}
	return started;
}

bool TryConsumeAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir, std::vector<ChatSession>& chats_out, std::string& digest_out, std::string& error_out, std::shared_ptr<platform::AsyncNativeChatLoadTask::State>* owner_out)
{
	const std::string key = uam::paths::NormalizedPortablePathString(app.data_root) + "\n" + AsyncNativeChatLoadTaskKey(provider.id, chats_dir);
	const auto it = app.native_chat_load_tasks.find(key);

	if (it == app.native_chat_load_tasks.end())
	{
		return false;
	}

	uam::platform::AsyncNativeChatLoadTask& task = it->second;
	CaptureTerminalNativeChatLoadOwners(app, task);
	const bool orphaned = TerminalNativeChatLoadOrphaned(app, task);

	// Final/launch cohorts and retired actual terminal keys cannot be adopted.
	if (task.state && (!TerminalNativeChatLoadOwnerCurrent(app, *task.state) || orphaned))
	{
		if (task.worker) task.worker->request_stop();
		if (task.state->completed.load(std::memory_order_acquire))
		{
			uam::platform::ResetAsyncNativeChatLoadTask(task);
			app.native_chat_load_tasks.erase(it);
		}
		return false;
	}
	const bool consumed = uam::platform::TryConsumeAsyncNativeChatLoadTask(task, chats_out, &digest_out, error_out, owner_out);
	if (task.running)
	{
		return false;
	}

	app.native_chat_load_tasks.erase(it);
	return consumed;
}

bool TryMarkCliTurnCompleteFromSyncedHistory(uam::AppState& app, uam::CliTerminalState& terminal, int previous_message_count, const std::string& selected_chat_id)
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

	const std::size_t synced_message_count = synced_chat->messages_loaded ? synced_chat->messages.size() : synced_chat->persisted_message_count;
	if (synced_message_count <= static_cast<std::size_t>(previous_message_count))
	{
		return false;
	}

	uam::MarkCliTerminalTurnIdle(terminal);
	if (ChatSession* chat = ChatDomainService().FindChatById(app, synced_chat->id))
	{
		chat->interaction_at = uam::time::InteractionTimestampNow();
		if (!ChatRepository::SaveLastOpenedAt(app.data_root, *chat)) app.pending_chat_save_at_by_chat_id[chat->id] = 0.0;
	}
	uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "turn_marked_idle_from_synced_history", &terminal, "message_count=" + std::to_string(synced_message_count));

	uam::MarkChatUnseen(app, synced_chat->id);

	return true;
}

std::unordered_set<std::string> BlockedNativeSessionIdsForTerminal(const uam::AppState& app, const uam::CliTerminalState& terminal)
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

bool TryAttachNativeSessionFromHistory(uam::AppState& app, uam::CliTerminalState& terminal, const std::vector<ChatSession>& native_chats)
{
	if (!CliTerminalAttachedSessionId(terminal).empty())
	{
		return false;
	}

	terminal.native_session_discovery_ambiguous = terminal.native_identity_requires_owned_reply || HasCompetingUnboundCliSession(app, terminal);
	if (terminal.native_session_discovery_ambiguous) return false;
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
		std::vector<std::string> candidates = native_session_linker.CollectNewSessionIds(native_chats, terminal.session_ids_before);
		std::erase_if(candidates, [&](const std::string& identity) { return blocked_ids.contains(identity); });
		if (candidates.size() > 1)
		{
			terminal.native_session_discovery_ambiguous = true;
			return false;
		}
		if (candidates.size() == 1) discovered = candidates.front();
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

void StopCliTerminalAfterProviderExit(uam::AppState& app, uam::CliTerminalState& terminal, bool was_shutting_down)
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

void ReconcileAndStopCliTerminalAfterProviderExit(uam::AppState& app, uam::CliTerminalState& terminal)
{
	const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
	// Retain the exact owned launch witness until its final transcript is reconciled.
	(void)uam::SyncChatsFromFinalCliTerminal(app, terminal, true);
	StopCliTerminalAfterProviderExit(app, terminal, was_shutting_down);
}

bool HandleCliTerminalInactivityTimeout(uam::AppState& app, uam::CliTerminalState& terminal, double now_seconds)
{
	const CliTerminalInactivityRecoveryAction action = CliTerminalInactivityRecovery(terminal, now_seconds, static_cast<double>(app.settings.active_turn_inactivity_timeout_seconds));
	if (action == CliTerminalInactivityRecoveryAction::None)
	{
		return false;
	}
	if (action == CliTerminalInactivityRecoveryAction::Interrupt)
	{
		constexpr char interrupt = '\x03';
		terminal.inactivity_interrupt_requested_time_s = now_seconds;
		const std::string message = "Provider terminal turn exceeded the " + std::to_string(app.settings.active_turn_inactivity_timeout_seconds) + "-second busy limit. Interrupting it now; the delivered prompt will not be replayed.";
		if (!WriteToCliTerminal(terminal, &interrupt, 1))
		{
			StopCliTerminal(terminal, false, CliTerminalStopMode::FastExit);
			terminal.should_launch = false;
		}
		terminal.last_error = message;
		app.status_line = message;
		uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "turn_inactivity_interrupt", &terminal, message);
		return true;
	}

	const std::string message = "Provider terminal stopped after the inactivity interrupt grace period. The delivered prompt was not replayed.";
	uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "turn_inactivity_force_stop", &terminal, message);
	StopCliTerminal(terminal, false, CliTerminalStopMode::FastExit);
	terminal.should_launch = false;
	terminal.last_error = message;
	app.status_line = message;
	return true;
}

bool PollCliTerminal(CefRefPtr<CefBrowser> browser, uam::AppState& app, uam::CliTerminalState& terminal, bool preserve_selection)
{
	if (!terminal.running)
	{
		return false;
	}

	constexpr std::size_t kRecentOutputBufferLimitBytes = 256 * 1024;
	constexpr double kInputReadyFallbackSeconds = 1.5;
	// Keep PTY draining and CEF delivery within a small UI-thread slice.
	constexpr int kReadBudgetChunksPerTick = 4;
	constexpr std::size_t kReadBudgetBytesPerTick = 32 * 1024;

	const bool first_launch_output = terminal.last_output_time_s <= 0.0;
	bool changed = false;
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const ChatSession* terminal_chat = FindChatForCliTerminal(app, terminal);
	const bool terminal_is_controller_local = terminal_chat != nullptr &&
	    uam::paths::IsControllerLocalWorkspace(*terminal_chat);
	const int previous_chat_message_count = terminal_chat != nullptr ? static_cast<int>(terminal_chat->messages_loaded ? terminal_chat->messages.size() : terminal_chat->persisted_message_count) : -1;
	const bool terminal_uses_native_history = (terminal_chat != nullptr) && ProviderResolutionService().ChatUsesNativeOverlayHistory(app, *terminal_chat);
	const ProviderProfile terminal_provider = (terminal_chat != nullptr) ? ProviderResolutionService().ProviderForChatOrDefault(app, *terminal_chat) : ProviderResolutionService().ActiveProviderOrDefault(app);
	const std::filesystem::path terminal_native_history_chats_dir = (terminal_chat != nullptr) ? ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, *terminal_chat) : std::filesystem::path{};

	const IPlatformTerminalRuntime& platform_terminal_runtime = PlatformServicesFactory::Instance().terminal_runtime;

	std::string input_error;
	if (terminal.input_writer != nullptr && terminal.input_writer->FailureOrStall(&input_error))
	{
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "async_input_failed", &terminal, input_error);
		uam::FailCliTerminalTransport(terminal, "Provider terminal input failed: " + uam::strings::NonEmptyOrFallback(input_error, "unknown transport error"));
		app.status_line = terminal.last_error;
		return true;
	}
	if (!platform_terminal_runtime.HasReadableTerminalOutputHandle(terminal))
	{
		const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_output_unavailable", &terminal);
		if (was_shutting_down)
		{
			StopCliTerminalAfterProviderExit(app, terminal, true);
		}
		else
		{
			uam::FailCliTerminalTransport(terminal, "Provider terminal output connection is unavailable.");
			app.status_line = terminal.last_error;
		}
		return true;
	}

	char buffer[8192];
	int chunks_read = 0;
	std::size_t bytes_read_total = 0;
	std::string output_for_frontend;
	bool output_reached_eof = false;

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
			output_for_frontend.append(buffer, static_cast<std::size_t>(read_bytes));
			continue;
		}

		if (read_bytes == 0)
		{
			uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_exit_eof", &terminal);
			output_reached_eof = true;
			break;
		}

		if (read_bytes == -2)
		{
			break;
		}

		const bool was_shutting_down = terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown;
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "pty_read_failed", &terminal);
		if (was_shutting_down)
		{
			StopCliTerminalAfterProviderExit(app, terminal, true);
		}
		else
		{
			uam::FailCliTerminalTransport(terminal, "Provider terminal read failed.");
			app.status_line = terminal.last_error;
		}
		changed = true;
		break;
	}

	if (!output_for_frontend.empty())
	{
		terminal.recent_output_bytes.append(output_for_frontend);
		terminal.output_cursor += output_for_frontend.size();
		if (terminal.recent_output_bytes.size() > kRecentOutputBufferLimitBytes)
		{
			terminal.recent_output_bytes.erase(0, terminal.recent_output_bytes.size() - kRecentOutputBufferLimitBytes);
		}
		terminal.current_turn_output_bytes.append(output_for_frontend);
		if (terminal.current_turn_output_bytes.size() > kTerminalPromptScanLimit)
		{
			terminal.current_turn_output_bytes.erase(0, terminal.current_turn_output_bytes.size() - kTerminalPromptScanLimit);
		}

		// Publish the launch binding before its first bytes, including background restarts.
		if (first_launch_output) uam::PushStateUpdateIfChanged(browser, app);
		const std::string primary_chat_id = CliTerminalPrimaryChatId(terminal);
		uam::PushCliOutput(browser, primary_chat_id, primary_chat_id, terminal.terminal_id, terminal.startup_time_s, output_for_frontend);
	}

	if (output_reached_eof)
	{
		// Deliver the drained final bytes before stop clears its output buffers.
		// Reconciliation can replace app.chats, so do not reuse terminal_chat afterward.
		ReconcileAndStopCliTerminalAfterProviderExit(app, terminal);
		return true;
	}

	const ExecutionHost* observation_host = terminal_chat ? uam::execution_hosts::Find(app.settings.execution_hosts, terminal_chat->execution_host_id) : nullptr;
	const IProviderRuntime& observation_runtime = ProviderRuntimeRegistry::Resolve(terminal_provider);
	const bool owned_native_observer = observation_host && observation_runtime.InteractiveObservationMatchesHost(terminal, *observation_host);
	if (terminal.opencode_activity_observer && !owned_native_observer) terminal.opencode_activity_observer.reset();
	const bool native_observation_available = terminal_is_controller_local || owned_native_observer;
	terminal.native_activity_provider_id = native_observation_available ? terminal_provider.id : std::string{};
	const CliTerminalLifecycleState before_native_lifecycle = terminal.lifecycle_state;
	const bool accepts_activity = terminal.running && terminal.lifecycle_state != CliTerminalLifecycleState::ShuttingDown;
	const ProviderTerminalActivity native_activity = accepts_activity && native_observation_available
	    ? observation_runtime.PollInteractiveActivity(terminal, CliTerminalAttachedSessionId(terminal), uam::paths::ResolveWorkspaceRootPath(app, *terminal_chat), terminal.native_session_discovery_ambiguous)
	    : ProviderTerminalActivity::Unavailable;
	changed |= terminal.lifecycle_state != before_native_lifecycle;
	if (native_activity == ProviderTerminalActivity::Busy)
	{
		terminal.prompt_settle_candidate_time_s = 0.0;
		if (terminal.lifecycle_state != CliTerminalLifecycleState::Busy)
		{
			uam::MarkCliTerminalTurnBusy(terminal, false);
			changed = true;
		}
	}
	const bool prompt_indicates_idle = ProviderRuntimeRegistry::Resolve(terminal_provider).RecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes);
	if (accepts_activity && ((native_activity == ProviderTerminalActivity::Complete && terminal.lifecycle_state != CliTerminalLifecycleState::Idle) || (native_activity != ProviderTerminalActivity::Busy &&
	    uam::CliTerminalPromptConfirmsTurnIdle(terminal, prompt_indicates_idle, !output_for_frontend.empty(), GetAppTimeSeconds()))))
	{
		uam::MarkCliTerminalTurnIdle(terminal);
		if (ChatSession* chat = ChatDomainService().FindChatById(app, terminal_chat->id))
		{
			chat->interaction_at = uam::time::InteractionTimestampNow();
			if (!ChatRepository::SaveLastOpenedAt(app.data_root, *chat)) app.pending_chat_save_at_by_chat_id[chat->id] = 0.0;
		}
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", native_activity == ProviderTerminalActivity::Complete ? "turn_marked_idle_from_native_event" : "turn_marked_idle_from_prompt", &terminal);
		changed = true;
	}

	if (terminal.running && platform_terminal_runtime.PollCliTerminalProcessExited(terminal))
	{
		uam::LogCliDiagnosticEvent(app, "poll_cli_terminal", "process_exited", &terminal);
		ReconcileAndStopCliTerminalAfterProviderExit(app, terminal);
		return true;
	}

	const double now = GetAppTimeSeconds();

	if (terminal.running && !terminal.input_ready && terminal.startup_time_s > 0.0 && (now - terminal.startup_time_s) >= kInputReadyFallbackSeconds)
	{
		terminal.input_ready = true;
		changed = true;
	}

	const bool sync_interval_elapsed = now - terminal.last_sync_time_s > kCliNativeHistoryRefreshIntervalSeconds;
	const bool should_refresh_native_history = terminal_uses_native_history && sync_interval_elapsed;
	if (terminal.running && terminal_is_controller_local && !terminal_uses_native_history && CliTerminalAttachedSessionId(terminal).empty() && sync_interval_elapsed)
	{
		terminal.last_sync_time_s = now;
		changed |= DiscoverCliTerminalNativeSession(app, terminal);
	}

	if (should_refresh_native_history)
	{
		terminal.last_sync_time_s = now;

		if (platform_terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
		{
			std::vector<ChatSession> native_now;
			std::shared_ptr<platform::AsyncNativeChatLoadTask::State> native_owner;
			std::string native_snapshot_digest;
			std::string native_load_error;
			const bool has_loaded_snapshot = TryConsumeAsyncNativeChatLoadForTerminal(app, terminal_provider, terminal_native_history_chats_dir, native_now, native_snapshot_digest, native_load_error, &native_owner);
			(void)StartAsyncNativeChatLoadForTerminal(app, terminal_provider, terminal_native_history_chats_dir);

			if (!native_load_error.empty())
			{
				LogCliDiagnosticEvent(app, "sync_native_history", "native_load_failed", &terminal, native_load_error);
			}

			if (has_loaded_snapshot && native_load_error.empty() && native_snapshot_digest != terminal.last_native_history_snapshot_digest)
			{
				changed = true;
				(void)TryAttachNativeSessionFromHistory(app, terminal, native_now);

				const std::string preferred_id = CliTerminalSyncTargetId(terminal);
				if (uam::SyncChatsFromLoadedNative(app, std::move(native_now), preferred_id, preserve_selection, native_owner, native_snapshot_digest, terminal.native_history_generation))
				{
					terminal.last_native_history_snapshot_digest = native_snapshot_digest;
					(void)TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
				}
			}
			if (native_owner) native_owner->chats = std::move(native_now);
		}
		else
		{
			const std::vector<ChatSession> native_now = ChatHistorySyncService().LoadNativeSessionChats(terminal_native_history_chats_dir, terminal_provider);
			const std::string native_snapshot_digest = NativeHistorySnapshotDigest(native_now);

			if (native_snapshot_digest != terminal.last_native_history_snapshot_digest)
			{
				changed = true;
				(void)TryAttachNativeSessionFromHistory(app, terminal, native_now);

				const std::string preferred_id = CliTerminalSyncTargetId(terminal);
				if (uam::SyncChatsFromNative(app, preferred_id, preserve_selection))
				{
					terminal.last_native_history_snapshot_digest = native_snapshot_digest;
					(void)TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, previous_chat_message_count, selected_chat_id);
				}
			}
		}
	}

	changed = PollCliNativeIdentityQuery(app, terminal, terminal_provider.id, output_for_frontend, GetAppTimeSeconds()) || changed;

	return changed;
}

bool PollAllCliTerminals(CefRefPtr<CefBrowser> browser, uam::AppState& app, bool terminal_ui_visible)
{
	RetryCliProviderContextCleanup(app, GetAppTimeSeconds());
	std::erase_if(app.cli_context_preparation_tasks, [](const CliContextPreparationTask& task) { return task.state->finished.load(); });
	constexpr double kShutdownFallbackSeconds = 2.5;
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const double now = GetAppTimeSeconds();
	uam::PollGitPortRequests(app);
	bool changed = ChatHistorySyncService().PollTerminalNativeMedia(app);
	changed |= uam::PollWorktreeLaunchPreparations(app);

	for (auto& terminal : app.cli_terminals)
	{
		if (terminal == nullptr)
		{
			continue;
		}

		const double min_poll_interval_s = terminal_ui_visible && terminal->ui_attached ? 0.016 : 0.25;

		if (terminal->last_polled_time_s > 0.0 && (now - terminal->last_polled_time_s) < min_poll_interval_s)
		{
			continue;
		}

		terminal->last_polled_time_s = now;
		const bool preserve_selection = !selected_chat_id.empty() && !CliTerminalMatchesChatId(*terminal, selected_chat_id);
		changed |= PollCliTerminal(browser, app, *terminal, preserve_selection);

		if (!terminal->pending_steer_prompt.empty())
		{
			if (uam::TryDeliverPendingCliTerminalSteer(*terminal))
			{
				uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "steer_prompt_delivered", terminal.get());
				changed = true;
			}
			else
			{
				const CliTerminalSteerRecoveryAction recovery = CliTerminalSteerRecovery(*terminal, now);
				if (recovery == CliTerminalSteerRecoveryAction::Restart)
				{
					terminal->pending_steer_restart_attempted = true;
					terminal->pending_steer_started_time_s = now;
					if (ChatSession* chat = FindChatForCliTerminal(app, *terminal); chat != nullptr && StartCliTerminalForChat(app, *terminal, *chat, terminal->rows, terminal->cols))
					{
						uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "steer_hard_restart", terminal.get());
					}
					else
					{
						terminal->last_error = "Terminal steering restart failed; prompt retained for retry.";
					}
					changed = true;
				}
				else if (recovery == CliTerminalSteerRecoveryAction::ReportTimeout)
				{
					terminal->last_error = "Terminal steering timed out after restart; prompt retained for retry.";
					uam::LogCliDiagnosticEvent(app, "poll_all_cli_terminals", "steer_timeout", terminal.get());
					changed = true;
				}
			}
		}

		if (terminal->context_preparation != nullptr && terminal->context_preparation->finished.load())
		{
			if (ChatSession* chat = FindChatForCliTerminal(app, *terminal); chat != nullptr)
			{
				(void)StartCliTerminalForChat(app, *terminal, *chat, terminal->rows, terminal->cols);
			}
			else StopCliTerminal(*terminal);
			changed = true;
		}
		if (!terminal->running && terminal->worktree_launch_pending)
		{
			if (!terminal->native_session_setup_cancel || terminal->native_session_setup_cancel->stop_requested())
			{
				uam::CancelWorktreeLaunchPreparation(app, terminal->frontend_chat_id, terminal->worktree_launch_cancellation);
				terminal->worktree_launch_pending = false;
				terminal->worktree_launch_cancellation.reset();
				terminal->should_launch = false;
			}
			else if (terminal->should_launch)
				if (ChatSession* chat = FindChatForCliTerminal(app, *terminal); chat != nullptr)
					(void)StartCliTerminalForChat(app, *terminal, *chat, terminal->rows, terminal->cols);
			changed = true;
		}
		if (!terminal->running)
		{
			continue;
		}

		if (HandleCliTerminalInactivityTimeout(app, *terminal, now))
		{
			changed = true;
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

		if (!terminal->running || terminal->lifecycle_state != uam::CliTerminalLifecycleState::Idle || !terminal->pending_steer_prompt.empty())
			{
			if (terminal->idle_interaction_started_time_s > 0.0) changed = true;
			terminal->idle_interaction_started_time_s = 0.0;
		}
		else if (terminal->idle_interaction_started_time_s <= 0.0)
			{
			terminal->idle_interaction_started_time_s = now;
			changed = true;
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
