#pragma once

/// <summary>
/// Polls asynchronous provider calls and syncs results back into chat state.
/// </summary>
inline void PollPendingRuntimeCall(AppState& app)
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

		if (call.completed == nullptr || call.output == nullptr)
		{
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}

		if (!call.completed->load(std::memory_order_acquire))
		{
			++i;
			continue;
		}

		const std::string output = *call.output;
		const std::string pending_chat_id = call.chat_id;
		const std::string selected_before_id = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
		const int pending_chat_index = FindChatIndexById(app, pending_chat_id);
		ChatSession pending_chat_snapshot;

		if (pending_chat_index >= 0)
		{
			pending_chat_snapshot = app.chats[pending_chat_index];
		}

		if (!ActiveProviderUsesNativeOverlayHistory(app))
		{
			if (pending_chat_index >= 0)
			{
				AddMessage(app.chats[pending_chat_index], MessageRole::Assistant, output);
				SaveChat(app, app.chats[pending_chat_index]);

				if (pending_chat_id != selected_before_id)
				{
					MarkChatUnseen(app, pending_chat_id);
				}

				app.status_line = "Provider response appended to local chat history.";
				app.scroll_to_bottom = true;
			}
			else
			{
				app.status_line = "Provider command completed, but chat no longer exists.";
			}

			app.resolved_native_sessions_by_chat_id.erase(pending_chat_id);
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}

		RefreshNativeSessionDirectory(app);
		std::vector<ChatSession> native_after = LoadNativeSessionChats(app.native_history_chats_dir, ActiveProviderOrDefault(app));
		ApplyLocalOverrides(app, native_after);

		std::string selected_id = call.resume_session_id;

		if (selected_id.empty())
		{
			const auto resolved_it = app.resolved_native_sessions_by_chat_id.find(pending_chat_id);

			if (resolved_it != app.resolved_native_sessions_by_chat_id.end() && SessionIdExistsInLoadedChats(native_after, resolved_it->second))
			{
				selected_id = resolved_it->second;
			}

			if (selected_id.empty())
			{
				const std::vector<std::string> candidates = CollectNewSessionIds(native_after, call.session_ids_before);
				selected_id = PickFirstUnblockedSessionId(candidates, claimed_new_session_ids);
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
			const int selected_index = FindChatIndexById(app, selected_id);

			if (pending_chat_index >= 0 && selected_id != pending_chat_id && IsLocalDraftChatId(pending_chat_id) && !app.chats[pending_chat_index].uses_native_session)
			{
				PersistLocalDraftNativeSessionLink(app, app.chats[pending_chat_index], selected_id);
			}

			const bool transfer_overrides_to_resolved_chat = pending_chat_index >= 0 && selected_index >= 0 && selected_id != pending_chat_id && IsLocalDraftChatId(pending_chat_id);

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

				SaveChat(app, app.chats[selected_index]);
			}

			if (selected_id != pending_chat_id)
			{
				app.chats.erase(std::remove_if(app.chats.begin(), app.chats.end(), [&](const ChatSession& chat) { return chat.id == pending_chat_id; }), app.chats.end());
			}

			if (should_follow_to_result)
			{
				SelectChatById(app, selected_id);
				app.scroll_to_bottom = true;
			}
			else
			{
				if (!selected_before_id.empty())
				{
					const int keep_index = FindChatIndexById(app, selected_before_id);

					if (keep_index >= 0)
					{
						app.selected_chat_index = keep_index;
						app.chats_with_unseen_updates.erase(selected_before_id);
						RefreshRememberedSelection(app);
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
			const int fallback_index = FindChatIndexById(app, pending_chat_id);

			if (fallback_index >= 0)
			{
				AddMessage(app.chats[fallback_index], MessageRole::System, output);

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

		NormalizeChatBranchMetadata(app);
		app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
	}
}
