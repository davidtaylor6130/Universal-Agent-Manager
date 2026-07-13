#include "runtime_orchestration_services.h"
#include "runtime_orchestration_internal.h"

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"

#include "common/chat/chat_branching.h"
#include "common/chat/chat_repository.h"
#include "common/models/app_models.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_chat_sync.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

using namespace uam::runtime_orch_impl;

bool PollPendingRuntimeCall(uam::AppState& app)
{
	if (app.pending_calls.empty())
	{
		return false;
	}

	bool changed = false;
	std::unordered_set<std::string> claimed_new_session_ids = ClaimedNativeSessionIds(app);

	for (std::size_t i = 0; i < app.pending_calls.size();)
	{
		PendingRuntimeCall& call = app.pending_calls[i];

		if (call.state == nullptr)
		{
			ResetPendingRuntimeCall(call);
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			changed = true;
			continue;
		}

		if (!call.state->completed.load(std::memory_order_acquire))
		{
			++i;
			continue;
		}

		const std::string output = call.state->result.output;
		const std::string pending_chat_id = call.chat_id;
		const std::string selected_before_id = ChatDomainService().SelectedChatId(app);
		ChatSession* pending_chat = ChatDomainService().FindChatById(app, pending_chat_id);
		const bool pending_chat_existed_before_sync = pending_chat != nullptr;
		const ProviderProfile& call_provider = ResolvePendingCallProviderOrDefault(app, call);
		ChatSession pending_chat_snapshot;

		if (pending_chat != nullptr)
		{
			pending_chat_snapshot = *pending_chat;
		}

		if (!ProviderRuntime::UsesNativeOverlayHistory(call_provider))
		{
			if (pending_chat != nullptr)
			{
				const bool call_failed = call.state->result.exit_code != 0 || call.state->result.timed_out || call.state->result.canceled || !call.state->result.error.empty();
				const MessageRole result_role = call_failed ? MessageRole::System : MessageRole::Assistant;
				const auto completion_time = std::chrono::steady_clock::now();
				const int64_t processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(completion_time - call.state->launch_time).count();
				const int64_t output_chars = static_cast<int64_t>(output.size());

				ChatDomainService::MessageAnalytics analytics;
				analytics.provider = call.state->provider_id;
				analytics.input_tokens = call.state->estimated_input_tokens;
				analytics.output_chars = output_chars;
				analytics.processing_time_ms = processing_time_ms;
				analytics.interrupted = call.state->result.canceled || call.state->result.timed_out;
				ChatDomainService().AddMessageWithAnalytics(*pending_chat, result_role, output, analytics);
				ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, *pending_chat), app.data_root, *pending_chat);

				if (pending_chat_id != selected_before_id)
				{
					uam::MarkChatUnseen(app, pending_chat_id);
				}

				app.status_line = call_failed ? "Provider command failed." : "Provider response appended to local chat history.";
			}
			else
			{
				app.status_line = "Provider command completed, but chat no longer exists.";
			}

			app.resolved_native_sessions_by_chat_id.erase(pending_chat_id);
			ResetPendingRuntimeCall(call);
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			changed = true;
			continue;
		}

		const std::filesystem::path native_history_chats_dir = call.native_history_chats_dir_snapshot.empty() ? std::filesystem::path{} : std::filesystem::path(call.native_history_chats_dir_snapshot);
		std::vector<ChatSession> native_after = ChatHistorySyncService().LoadNativeSessionChats(native_history_chats_dir, call_provider);
		ChatHistorySyncService().ApplyLocalOverrides(app, native_after);
		for (ChatSession& chat : native_after)
		{
			ChatRepository::SaveChat(app.data_root, chat);
		}
		ReplaceAppChatsWithNormalized(app, ChatRepository::LoadLocalChats(app.data_root));

		std::string selected_id = uam::strings::Trim(call.resume_session_id);

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
					if (const auto matched = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(pending_chat_snapshot, native_after, claimed_new_session_ids))
					{
						selected_id = *matched;
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

			const bool should_follow_to_result = selected_before_id == pending_chat_id;
			ChatSession* selected_chat = ChatDomainService().FindChatById(app, selected_id);
			ChatSession* refreshed_pending_chat = ChatDomainService().FindChatById(app, pending_chat_id);

			if (refreshed_pending_chat != nullptr && selected_id != pending_chat_id && NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id) && !NativeSessionLinkService().HasRealNativeSessionId(*refreshed_pending_chat))
			{
				changed |= ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, *refreshed_pending_chat, selected_id);
			}

			const bool transfer_overrides_to_resolved_chat = pending_chat_existed_before_sync && selected_chat != nullptr && selected_id != pending_chat_id && NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id);

			if (transfer_overrides_to_resolved_chat)
			{
				selected_chat->linked_files = pending_chat_snapshot.linked_files;
				selected_chat->parent_chat_id = pending_chat_snapshot.parent_chat_id;
				selected_chat->branch_root_chat_id = pending_chat_snapshot.branch_root_chat_id;
				selected_chat->branch_from_message_index = pending_chat_snapshot.branch_from_message_index;
				selected_chat->branch_message_edited = pending_chat_snapshot.branch_message_edited;

				if (!pending_chat_snapshot.folder_id.empty())
				{
					selected_chat->folder_id = pending_chat_snapshot.folder_id;
				}

				ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, *selected_chat), app.data_root, *selected_chat);
			}

			if (selected_id != pending_chat_id)
			{
				std::erase_if(app.chats, [&](const ChatSession& chat) { return chat.id == pending_chat_id; });
			}

			if (should_follow_to_result)
			{
				ChatDomainService().SelectChatById(app, selected_id);
			}
			else
			{
				if (!selected_before_id.empty())
				{
					const int keep_index = ChatDomainService().FindChatIndexById(app, selected_before_id);

					if (keep_index >= 0)
					{
						ChatDomainService().SetSelectedChatIndexOrNearest(app, keep_index);
						app.chats_with_unseen_updates.erase(selected_before_id);
					}
				}

				if (selected_id != selected_before_id)
				{
					uam::MarkChatUnseen(app, selected_id);
				}
			}

			app.status_line = "Provider response synced from native session.";
		}
		else
		{
			app.resolved_native_sessions_by_chat_id.erase(pending_chat_id);
			ChatSession* fallback_chat = ChatDomainService().FindChatById(app, pending_chat_id);

			if (fallback_chat != nullptr)
			{
				ChatDomainService().AddMessage(*fallback_chat, MessageRole::System, output);

				if (pending_chat_id != selected_before_id)
				{
					uam::MarkChatUnseen(app, pending_chat_id);
				}

				app.status_line = "Provider command completed, but no native session was detected.";
			}
			else
			{
				app.status_line = "Provider command completed, but no native session was detected.";
			}
		}

		ChatBranching::Normalize(app.chats);
		ResetPendingRuntimeCall(call);
		app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
		changed = true;
	}

	return changed;
}
