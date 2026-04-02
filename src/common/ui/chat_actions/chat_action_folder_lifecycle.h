#pragma once

/// <summary>
/// Folder deletion, chat creation, and delete-request dispatch actions.
/// </summary>
static bool DeleteFolderById(AppState& app, const std::string& folder_id)
{
	if (folder_id.empty() || folder_id == kDefaultFolderId)
	{
		app.status_line = "The default folder cannot be deleted.";
		return false;
	}

	const int folder_index = FindFolderIndexById(app, folder_id);

	if (folder_index < 0)
	{
		app.status_line = "Folder no longer exists.";
		return false;
	}

	bool all_chat_saves_ok = true;
	int moved_chat_count = 0;

	for (ChatSession& existing_chat : app.chats)
	{
		if (existing_chat.folder_id != folder_id)
		{
			continue;
		}

		existing_chat.folder_id = kDefaultFolderId;
		existing_chat.updated_at = TimestampNow();

		if (!SaveChat(app, existing_chat))
		{
			all_chat_saves_ok = false;
		}

		++moved_chat_count;
	}

	app.folders.erase(app.folders.begin() + folder_index);
	EnsureDefaultFolder(app);

	if (app.new_chat_folder_id == folder_id)
	{
		app.new_chat_folder_id = kDefaultFolderId;
	}

	SaveFolders(app);

	if (all_chat_saves_ok)
	{
		app.status_line = "Folder deleted. Moved " + std::to_string(moved_chat_count) + " chat(s) to General.";
	}
	else
	{
		app.status_line = "Folder deleted, but failed to persist one or more moved chats.";
	}

	return true;
}

static std::string ResolveNewChatProviderId(const AppState& app, const std::string& preferred_provider_id = std::string())
{
	const std::string preferred = Trim(preferred_provider_id);

	if (!preferred.empty())
	{
		if (const ProviderProfile* preferred_profile = ProviderProfileStore::FindById(app.provider_profiles, preferred); preferred_profile != nullptr && ShouldShowProviderProfileInUi(*preferred_profile))
		{
			return preferred_profile->id;
		}
	}

	const std::string active = Trim(app.settings.active_provider_id);

	if (!active.empty())
	{
		if (const ProviderProfile* active_profile = ProviderProfileStore::FindById(app.provider_profiles, active); active_profile != nullptr && ShouldShowProviderProfileInUi(*active_profile))
		{
			return active_profile->id;
		}
	}

	for (const ProviderProfile& profile : app.provider_profiles)
	{
		if (ShouldShowProviderProfileInUi(profile))
		{
			return profile.id;
		}
	}

	return active.empty() ? std::string("gemini-structured") : active;
}

static void OpenNewChatPopup(AppState& app, const std::string& target_folder_id = std::string())
{
	if (!target_folder_id.empty())
	{
		app.new_chat_folder_id = target_folder_id;
	}

	EnsureNewChatFolderSelection(app);
	app.pending_new_chat_provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);
	app.open_new_chat_popup = true;
}

static void CreateAndSelectChatWithProvider(AppState& app, const std::string& provider_id)
{
	const std::string selected_provider_id = ResolveNewChatProviderId(app, provider_id);
	const std::string target_folder = FolderForNewChat(app);

	for (const ChatSession& existing : app.chats)
	{
		if (existing.folder_id == target_folder && existing.provider_id == selected_provider_id && IsLocalDraftChatId(existing.id) && existing.messages.empty() && !HasPendingCallForChat(app, existing.id))
		{
			SelectChatById(app, existing.id);
			SaveSettings(app);

			if (const ChatSession* selected = SelectedChat(app); selected != nullptr && ChatUsesCliOutput(app, *selected))
			{
				MarkSelectedCliTerminalForLaunch(app);
			}

			app.status_line = "Reused existing empty chat draft.";
			return;
		}
	}

	ChatSession chat = CreateNewChat(target_folder, selected_provider_id);
	app.settings.active_provider_id = selected_provider_id;
	chat.rag_enabled = app.settings.rag_enabled;
	app.chats.push_back(chat);
	NormalizeChatBranchMetadata(app);
	SortChatsByRecent(app.chats);
	SelectChatById(app, chat.id);
	SaveSettings(app);

	if (const ChatSession* selected = SelectedChat(app); selected != nullptr && ChatUsesCliOutput(app, *selected))
	{
		MarkSelectedCliTerminalForLaunch(app);
	}

	if (!SaveChat(app, app.chats[app.selected_chat_index]))
	{
		app.status_line = "Created chat in memory, but failed to persist.";
	}
	else
	{
		app.status_line = "New chat created.";
	}
}

static void CreateAndSelectChat(AppState& app)
{
	OpenNewChatPopup(app, FolderForNewChat(app));
}

static void CreateAndSelectChatInFolder(AppState& app, const std::string& folder_id)
{
	OpenNewChatPopup(app, folder_id);
}

static bool ConfirmCreateNewChat(AppState& app)
{
	const std::string provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);

	if (provider_id.empty())
	{
		app.status_line = "No provider available for new chat.";
		return false;
	}

	app.pending_new_chat_provider_id = provider_id;
	CreateAndSelectChatWithProvider(app, provider_id);
	app.pending_new_chat_provider_id.clear();
	return true;
}

static void RequestDeleteSelectedChat(AppState& app)
{
	const ChatSession* chat = SelectedChat(app);

	if (chat == nullptr)
	{
		app.status_line = "Select a chat to delete.";
		return;
	}

	if (app.settings.confirm_delete_chat)
	{
		app.pending_delete_chat_id = chat->id;
		app.open_delete_chat_popup = true;
		return;
	}

	RemoveChatById(app, chat->id);
}

static void RequestDeleteFolder(AppState& app, const std::string& folder_id)
{
	if (folder_id.empty())
	{
		app.status_line = "No folder selected.";
		return;
	}

	if (app.settings.confirm_delete_folder)
	{
		app.pending_delete_folder_id = folder_id;
		app.open_delete_folder_popup = true;
		return;
	}

	DeleteFolderById(app, folder_id);
}
