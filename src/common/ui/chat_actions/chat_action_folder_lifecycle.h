#ifndef UAM_COMMON_UI_CHAT_ACTIONS_CHAT_ACTION_FOLDER_LIFECYCLE_H
#define UAM_COMMON_UI_CHAT_ACTIONS_CHAT_ACTION_FOLDER_LIFECYCLE_H

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat_folder_store.h"
#include "common/constants/app_constants.h"
#include "common/provider_runtime.h"
#include "common/runtime/terminal_common.h"

/// <summary>
/// Folder deletion, chat creation, and delete-request dispatch actions.
/// </summary>
inline bool DeleteFolderById(AppState& app, const std::string& folder_id)
{
	if (folder_id.empty() || folder_id == uam::constants::kDefaultFolderId)
	{
		app.status_line = "The default folder cannot be deleted.";
		return false;
	}

	const int folder_index = ChatDomainService().FindFolderIndexById(app, folder_id);

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

		existing_chat.folder_id = uam::constants::kDefaultFolderId;
		existing_chat.updated_at = TimestampNow();

		if (!ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, existing_chat), app.data_root, existing_chat))
		{
			all_chat_saves_ok = false;
		}

		++moved_chat_count;
	}

	app.folders.erase(app.folders.begin() + folder_index);
	ChatDomainService().EnsureDefaultFolder(app);

	if (app.new_chat_folder_id == folder_id)
	{
		app.new_chat_folder_id = uam::constants::kDefaultFolderId;
	}

	ChatFolderStore::Save(app.data_root, app.folders);

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

enum class NewChatDuplicatePolicy
{
	Prompt,
	CreateNew,
	ReuseExisting
};

inline void ClearPendingDuplicateNewChatDecision(AppState& app)
{
	app.open_duplicate_new_chat_popup = false;
	app.pending_duplicate_new_chat_existing_id.clear();
	app.pending_duplicate_new_chat_provider_id.clear();
	app.pending_duplicate_new_chat_folder_id.clear();
}

inline std::string FindExistingEmptyDraftChatIdForFolderProvider(const AppState& app, const std::string& folder_id, const std::string& provider_id)
{
	std::string best_chat_id;
	std::string best_updated_at;
	std::string best_created_at;

	for (const ChatSession& existing : app.chats)
	{
		if (existing.folder_id != folder_id || existing.provider_id != provider_id)
		{
			continue;
		}

		if (!NativeSessionLinkService().IsLocalDraftChatId(existing.id) || !existing.messages.empty() || HasPendingCallForChat(app, existing.id))
		{
			continue;
		}

		const bool prefer_current = best_chat_id.empty() || existing.updated_at > best_updated_at || (existing.updated_at == best_updated_at && (existing.created_at > best_created_at || (existing.created_at == best_created_at && existing.id < best_chat_id)));

		if (!prefer_current)
		{
			continue;
		}

		best_chat_id = existing.id;
		best_updated_at = existing.updated_at;
		best_created_at = existing.created_at;
	}

	return best_chat_id;
}

inline std::string ResolveNewChatProviderId(const AppState& app, const std::string& preferred_provider_id = std::string())
{
	const std::string preferred = Trim(preferred_provider_id);

	if (!preferred.empty())
	{
		if (const ProviderProfile* preferred_profile = ProviderProfileStore::FindById(app.provider_profiles, preferred); preferred_profile != nullptr && ProviderProfileMigrationService().ShouldShowProviderProfileInUi(*preferred_profile))
		{
			return preferred_profile->id;
		}
	}

	const std::string active = Trim(app.settings.active_provider_id);

	if (!active.empty())
	{
		if (const ProviderProfile* active_profile = ProviderProfileStore::FindById(app.provider_profiles, active); active_profile != nullptr && ProviderProfileMigrationService().ShouldShowProviderProfileInUi(*active_profile))
		{
			return active_profile->id;
		}
	}

	for (const ProviderProfile& profile : app.provider_profiles)
	{
		if (ProviderProfileMigrationService().ShouldShowProviderProfileInUi(profile))
		{
			return profile.id;
		}
	}

	return active.empty() ? std::string("gemini-structured") : active;
}

inline void OpenNewChatPopup(AppState& app, const std::string& target_folder_id = std::string())
{
	if (!target_folder_id.empty())
	{
		app.new_chat_folder_id = target_folder_id;
	}

	ChatDomainService().EnsureNewChatFolderSelection(app);
	ClearPendingDuplicateNewChatDecision(app);
	app.pending_new_chat_provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);
	app.open_new_chat_popup = true;
}

#endif // UAM_COMMON_UI_CHAT_ACTIONS_CHAT_ACTION_FOLDER_LIFECYCLE_H

inline bool CreateAndSelectChatWithProvider(AppState& app, const std::string& provider_id, const NewChatDuplicatePolicy duplicate_policy = NewChatDuplicatePolicy::Prompt)
{
	const std::string selected_provider_id = ResolveNewChatProviderId(app, provider_id);

	if (selected_provider_id.empty())
	{
		app.status_line = "No provider available for new chat.";
		return false;
	}

	const std::string target_folder = ChatDomainService().FolderForNewChat(app);
	const std::string existing_draft_chat_id = FindExistingEmptyDraftChatIdForFolderProvider(app, target_folder, selected_provider_id);

	if (!existing_draft_chat_id.empty())
	{
		if (duplicate_policy == NewChatDuplicatePolicy::Prompt)
		{
			app.pending_duplicate_new_chat_existing_id = existing_draft_chat_id;
			app.pending_duplicate_new_chat_provider_id = selected_provider_id;
			app.pending_duplicate_new_chat_folder_id = target_folder;
			app.open_duplicate_new_chat_popup = true;
			app.status_line = "An empty draft already exists for this folder and provider.";
			return true;
		}

		if (duplicate_policy == NewChatDuplicatePolicy::ReuseExisting)
		{
			ChatDomainService().SelectChatById(app, existing_draft_chat_id);
			PersistenceCoordinator().SaveSettings(app);

			if (const ChatSession* selected = ChatDomainService().SelectedChat(app); selected != nullptr && ProviderResolutionService().ChatUsesCliOutput(app, *selected))
			{
				MarkSelectedCliTerminalForLaunch(app);
			}

			ClearPendingDuplicateNewChatDecision(app);
			app.status_line = "Reused existing empty chat draft.";
			return true;
		}
	}

	ChatSession chat = ChatDomainService().CreateNewChat(target_folder, selected_provider_id);
	app.settings.active_provider_id = selected_provider_id;
	chat.rag_enabled = app.settings.rag_enabled;
	app.chats.push_back(chat);
	ChatBranching::Normalize(app.chats);
	ChatDomainService().SortChatsByRecent(app.chats);
	ChatDomainService().SelectChatById(app, chat.id);
	PersistenceCoordinator().SaveSettings(app);

	if (const ChatSession* selected = ChatDomainService().SelectedChat(app); selected != nullptr && ProviderResolutionService().ChatUsesCliOutput(app, *selected))
	{
		MarkSelectedCliTerminalForLaunch(app);
	}

	if (!ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, app.chats[app.selected_chat_index]), app.data_root, app.chats[app.selected_chat_index]))
	{
		ClearPendingDuplicateNewChatDecision(app);
		app.status_line = "Created chat in memory, but failed to persist.";
		return false;
	}

	ClearPendingDuplicateNewChatDecision(app);
	app.status_line = "New chat created.";
	return true;
}

inline void CreateAndSelectChat(AppState& app)
{
	OpenNewChatPopup(app, ChatDomainService().FolderForNewChat(app));
}

inline void CreateAndSelectChatInFolder(AppState& app, const std::string& folder_id)
{
	OpenNewChatPopup(app, folder_id);
}

inline bool ConfirmCreateNewChat(AppState& app)
{
	const std::string provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);

	if (provider_id.empty())
	{
		app.status_line = "No provider available for new chat.";
		return false;
	}

	app.pending_new_chat_provider_id = provider_id;
	const bool handled = CreateAndSelectChatWithProvider(app, provider_id, NewChatDuplicatePolicy::Prompt);
	app.pending_new_chat_provider_id.clear();
	return handled;
}

inline void RequestDeleteSelectedChat(AppState& app)
{
	const ChatSession* chat = ChatDomainService().SelectedChat(app);

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

inline void RequestDeleteFolder(AppState& app, const std::string& folder_id)
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
