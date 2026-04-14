#pragma once

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_repository.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_chat_sync.h"

inline const ProviderProfile& ResolvePendingCallProviderOrDefault(const uam::AppState& app, const PendingRuntimeCall& call)
{
	if (!call.provider_id_snapshot.empty())
	{
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, call.provider_id_snapshot); profile != nullptr)
		{
			return *profile;
		}
	}

	return ProviderResolutionService().ActiveProviderOrDefault(app);
}

inline void ResetPendingRuntimeCallWorker(PendingRuntimeCall& call)
{
	if (call.worker != nullptr)
	{
		call.worker->request_stop();
		call.worker->detach();
		call.worker.reset();
	}

	call.state.reset();
}

/// <summary>
/// Polls asynchronous provider calls and syncs results back into chat state.
/// </summary>
inline void PollPendingRuntimeCall(uam::AppState& app)
{
	if (app.pending_calls.empty())
	{
		return;
	}

	std::unordered_set<std::string> claimed_new_session_ids;

	for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
	{
		if (!resolved.second.empty())
		{
			claimed_new_session_ids.insert(resolved.second);
		}
	}

	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && !terminal->attached_session_id.empty())
		{
			claimed_new_session_ids.insert(terminal->attached_session_id);
		}
	}

	for (std::size_t i = 0; i < app.pending_calls.size();)
	{
		PendingRuntimeCall& call = app.pending_calls[i];

		if (call.state == nullptr)
		{
			ResetPendingRuntimeCallWorker(call);
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}

		if (!call.state->completed.load(std::memory_order_acquire))
		{
			++i;
			continue;
		}

		const std::string output = call.state->result.output;
		const std::string pending_chat_id = call.chat_id;
		const ChatSession* lcp_selectedBefore = ChatDomainService().SelectedChat(app);
		const std::string selected_before_id = (lcp_selectedBefore != nullptr) ? lcp_selectedBefore->id : "";
		const int pending_chat_index = ChatDomainService().FindChatIndexById(app, pending_chat_id);
		const ProviderProfile& call_provider = ResolvePendingCallProviderOrDefault(app, call);
		ChatSession pending_chat_snapshot;

		if (pending_chat_index >= 0)
		{
			pending_chat_snapshot = app.chats[pending_chat_index];
		}

		if (!ProviderRuntime::UsesNativeOverlayHistory(call_provider))
		{
			if (pending_chat_index >= 0)
			{
				// Use System role for any failure so that shell errors, timeouts,
				// and cancellations are never displayed as model replies (Bug #2 fix).
				const bool call_failed = call.state->result.exit_code != 0 || call.state->result.timed_out || call.state->result.canceled || !call.state->result.error.empty();
				const MessageRole result_role = call_failed ? MessageRole::System : MessageRole::Assistant;

				const auto completion_time = std::chrono::steady_clock::now();
				const int64_t processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(completion_time - call.state->launch_time).count();
				const int64_t output_chars = static_cast<int64_t>(output.size());

				ChatDomainService().AddMessageWithAnalytics(app.chats[pending_chat_index], result_role, output, call.state->provider_id, call.state->estimated_input_tokens, output_chars, 0, processing_time_ms, call.state->result.canceled || call.state->result.timed_out);
				ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, app.chats[pending_chat_index]), app.data_root, app.chats[pending_chat_index]);

				if (pending_chat_id != selected_before_id)
				{
					MarkChatUnseen(app, pending_chat_id);
				}

				app.status_line = call_failed ? "Provider command failed." : "Provider response appended to local chat history.";
				app.scroll_to_bottom = true;
			}
			else
			{
				app.status_line = "Provider command completed, but chat no longer exists.";
			}

			app.resolved_native_sessions_by_chat_id.erase(pending_chat_id);
			ResetPendingRuntimeCallWorker(call);
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}

			const std::filesystem::path native_history_chats_dir = call.native_history_chats_dir_snapshot.empty() ? std::filesystem::path{} : std::filesystem::path(call.native_history_chats_dir_snapshot);
			std::vector<ChatSession> native_after = ChatHistorySyncService().LoadNativeSessionChats(native_history_chats_dir, call_provider);
			ChatHistorySyncService().ApplyLocalOverrides(app, native_after);
			for (ChatSession& chat : native_after)
			{
				ChatRepository::SaveChat(app.data_root, chat);
			}
		app.chats = ChatRepository::LoadLocalChats(app.data_root);
		app.chats = ChatDomainService().DeduplicateChatsById(std::move(app.chats));
		ChatBranching::Normalize(app.chats);
		ChatDomainService().NormalizeChatFolderAssignments(app);

		std::string selected_id = call.resume_session_id;

		if (NativeSessionLinkService().IsLocalDraftChatId(selected_id))
		{
			selected_id.clear();
		}

		if (selected_id.empty())
		{
			const auto resolved_it = app.resolved_native_sessions_by_chat_id.find(pending_chat_id);

			if (resolved_it != app.resolved_native_sessions_by_chat_id.end() && NativeSessionLinkService().SessionIdExistsInLoadedChats(native_after, resolved_it->second))
			{
				selected_id = resolved_it->second;
			}

			if (selected_id.empty())
			{
				if (NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id))
				{
					if (const auto matched = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(pending_chat_snapshot, native_after, claimed_new_session_ids); matched.has_value())
					{
						selected_id = matched.value();
					}
				}
				else
				{
					const std::vector<std::string> candidates = NativeSessionLinkService().CollectNewSessionIds(native_after, call.session_ids_before);
					selected_id = NativeSessionLinkService().PickFirstUnblockedSessionId(candidates, claimed_new_session_ids);
				}
			}
		}

		if (!selected_id.empty())
		{
			claimed_new_session_ids.insert(selected_id);

			if (call.resume_session_id.empty())
			{
				app.resolved_native_sessions_by_chat_id[pending_chat_id] = selected_id;
			}

			const bool should_follow_to_result = (selected_before_id == pending_chat_id);
			const int selected_index = ChatDomainService().FindChatIndexById(app, selected_id);
			const int refreshed_pending_chat_index = ChatDomainService().FindChatIndexById(app, pending_chat_id);

			if (refreshed_pending_chat_index >= 0 &&
			    selected_id != pending_chat_id &&
			    NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id) &&
			    !NativeSessionLinkService().HasRealNativeSessionId(app.chats[refreshed_pending_chat_index]))
			{
				ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, app.chats[refreshed_pending_chat_index], selected_id);
			}

			const bool transfer_overrides_to_resolved_chat = pending_chat_index >= 0 && selected_index >= 0 && selected_id != pending_chat_id && NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id);

			if (transfer_overrides_to_resolved_chat)
			{
				app.chats[selected_index].linked_files = pending_chat_snapshot.linked_files;
				app.chats[selected_index].template_override_id = pending_chat_snapshot.template_override_id;
				app.chats[selected_index].prompt_profile_bootstrapped = pending_chat_snapshot.prompt_profile_bootstrapped;
				app.chats[selected_index].parent_chat_id = pending_chat_snapshot.parent_chat_id;
				app.chats[selected_index].branch_root_chat_id = pending_chat_snapshot.branch_root_chat_id;
				app.chats[selected_index].branch_from_message_index = pending_chat_snapshot.branch_from_message_index;

				if (!pending_chat_snapshot.folder_id.empty())
				{
					app.chats[selected_index].folder_id = pending_chat_snapshot.folder_id;
				}

				ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, app.chats[selected_index]), app.data_root, app.chats[selected_index]);
			}

			if (selected_id != pending_chat_id)
			{
				app.chats.erase(std::remove_if(app.chats.begin(), app.chats.end(), [&](const ChatSession& chat) { return chat.id == pending_chat_id; }), app.chats.end());
			}

			if (should_follow_to_result)
			{
				ChatDomainService().SelectChatById(app, selected_id);
				app.scroll_to_bottom = true;
			}
			else
			{
				if (!selected_before_id.empty())
				{
					const int keep_index = ChatDomainService().FindChatIndexById(app, selected_before_id);

					if (keep_index >= 0)
					{
						app.selected_chat_index = keep_index;
						app.chats_with_unseen_updates.erase(selected_before_id);
						ChatDomainService().RefreshRememberedSelection(app);
					}
				}

				if (selected_id != selected_before_id)
				{
					MarkChatUnseen(app, selected_id);
				}
			}

			app.status_line = "Provider response synced from native session.";
		}
		else
		{
			app.resolved_native_sessions_by_chat_id.erase(pending_chat_id);
			const int fallback_index = ChatDomainService().FindChatIndexById(app, pending_chat_id);

			if (fallback_index >= 0)
			{
				ChatDomainService().AddMessage(app.chats[fallback_index], MessageRole::System, output);

				if (pending_chat_id != selected_before_id)
				{
					MarkChatUnseen(app, pending_chat_id);
				}

				app.status_line = "Provider command completed, but no native session was detected.";
				app.scroll_to_bottom = true;
			}
			else
			{
				app.status_line = "Provider command completed, but no native session was detected.";
			}
		}

		ChatBranching::Normalize(app.chats);
		ResetPendingRuntimeCallWorker(call);
		app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
	}
}
