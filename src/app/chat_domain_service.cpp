#include "chat_domain_service.h"

#include "app/application_core_helpers.h"
#include "common/constants/app_constants.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_map>

using uam::AppState;
using uam::constants::kDefaultFolderId;
using uam::constants::kDefaultFolderTitle;

std::string ChatDomainService::NewFolderId() const
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	std::ostringstream id;
	id << "folder-" << epoch_ms;
	return id.str();
}

int ChatDomainService::FindFolderIndexById(const AppState& app, const std::string& folder_id) const
{
	for (int i = 0; i < static_cast<int>(app.folders.size()); ++i)
	{
		if (app.folders[i].id == folder_id)
		{
			return i;
		}
	}

	return -1;
}

ChatFolder* ChatDomainService::FindFolderById(AppState& app, const std::string& folder_id) const
{
	const int index = FindFolderIndexById(app, folder_id);
	return (index >= 0) ? &app.folders[index] : nullptr;
}

const ChatFolder* ChatDomainService::FindFolderById(const AppState& app, const std::string& folder_id) const
{
	const int index = FindFolderIndexById(app, folder_id);
	return (index >= 0) ? &app.folders[index] : nullptr;
}

void ChatDomainService::EnsureDefaultFolder(AppState& app) const
{
	if (FindFolderIndexById(app, kDefaultFolderId) >= 0)
	{
		return;
	}

	ChatFolder folder;
	folder.id = kDefaultFolderId;
	folder.title = kDefaultFolderTitle;
	folder.directory = std::filesystem::current_path().string();
	folder.collapsed = false;
	app.folders.push_back(std::move(folder));
}

void ChatDomainService::EnsureNewChatFolderSelection(AppState& app) const
{
	EnsureDefaultFolder(app);

	if (app.new_chat_folder_id.empty() || FindFolderById(app, app.new_chat_folder_id) == nullptr)
	{
		app.new_chat_folder_id = kDefaultFolderId;
	}
}

void ChatDomainService::NormalizeChatFolderAssignments(AppState& app) const
{
	EnsureDefaultFolder(app);

	for (ChatSession& chat : app.chats)
	{
		if (chat.folder_id.empty() || FindFolderById(app, chat.folder_id) == nullptr)
		{
			chat.folder_id = kDefaultFolderId;
		}
	}

	bool any_expanded_with_chats = false;

	for (const ChatFolder& folder : app.folders)
	{
		if (!folder.collapsed && CountChatsInFolder(app, folder.id) > 0)
		{
			any_expanded_with_chats = true;
			break;
		}
	}

	if (!any_expanded_with_chats)
	{
		for (ChatFolder& folder : app.folders)
		{
			if (CountChatsInFolder(app, folder.id) > 0)
			{
				folder.collapsed = false;
			}
		}
	}

	EnsureNewChatFolderSelection(app);
}

std::string ChatDomainService::FolderForNewChat(const AppState& app) const
{
	if (!app.new_chat_folder_id.empty())
	{
		return app.new_chat_folder_id;
	}

	return kDefaultFolderId;
}

int ChatDomainService::CountChatsInFolder(const AppState& app, const std::string& folder_id) const
{
	int count = 0;

	for (const ChatSession& chat : app.chats)
	{
		if (chat.folder_id == folder_id)
		{
			++count;
		}
	}

	return count;
}

std::string ChatDomainService::FolderTitleOrFallback(const ChatFolder& folder) const
{
	const std::string trimmed = Trim(folder.title);
	return trimmed.empty() ? "Untitled Folder" : trimmed;
}

int ChatDomainService::FindChatIndexById(const AppState& app, const std::string& chat_id) const
{
	for (int i = 0; i < static_cast<int>(app.chats.size()); ++i)
	{
		if (app.chats[i].id == chat_id)
		{
			return i;
		}
	}

	return -1;
}

ChatSession* ChatDomainService::SelectedChat(AppState& app) const
{
	if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size()))
	{
		return nullptr;
	}

	return &app.chats[app.selected_chat_index];
}

const ChatSession* ChatDomainService::SelectedChat(const AppState& app) const
{
	if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size()))
	{
		return nullptr;
	}

	return &app.chats[app.selected_chat_index];
}

void ChatDomainService::SortChatsByRecent(std::vector<ChatSession>& chats) const
{
	std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) { return a.updated_at > b.updated_at; });
}

bool ChatDomainService::ShouldReplaceChatForDuplicateId(const ChatSession& candidate, const ChatSession& existing) const
{
	if (candidate.uses_native_session != existing.uses_native_session)
	{
		return candidate.uses_native_session && !existing.uses_native_session;
	}

	if (candidate.messages.size() != existing.messages.size())
	{
		return candidate.messages.size() > existing.messages.size();
	}

	if (candidate.updated_at != existing.updated_at)
	{
		return candidate.updated_at > existing.updated_at;
	}

	if (candidate.created_at != existing.created_at)
	{
		return candidate.created_at > existing.created_at;
	}

	if (candidate.linked_files.size() != existing.linked_files.size())
	{
		return candidate.linked_files.size() > existing.linked_files.size();
	}

	if (candidate.provider_id != existing.provider_id)
	{
		return !candidate.provider_id.empty();
	}

	if (candidate.template_override_id != existing.template_override_id)
	{
		return !candidate.template_override_id.empty();
	}

	if (candidate.parent_chat_id != existing.parent_chat_id)
	{
		return !candidate.parent_chat_id.empty();
	}

	if (candidate.branch_root_chat_id != existing.branch_root_chat_id)
	{
		return !candidate.branch_root_chat_id.empty();
	}

	if (candidate.branch_from_message_index != existing.branch_from_message_index)
	{
		return candidate.branch_from_message_index > existing.branch_from_message_index;
	}

	return false;
}

std::vector<ChatSession> ChatDomainService::DeduplicateChatsById(std::vector<ChatSession> chats) const
{
	std::vector<ChatSession> deduped;
	deduped.reserve(chats.size());
	std::unordered_map<std::string, std::size_t> index_by_id;
	std::unordered_map<std::string, std::size_t> index_by_native_session_id;

	for (ChatSession& chat : chats)
	{
		chat.id = Trim(chat.id);

		if (chat.id.empty())
		{
			continue;
		}

		const std::string native_session_id = Trim(chat.native_session_id);
		const bool has_native_identity = chat.uses_native_session && !native_session_id.empty();
		const std::string native_key = has_native_identity ? ("native:" + native_session_id) : std::string{};

		if (has_native_identity)
		{
			const auto native_it = index_by_native_session_id.find(native_key);

			if (native_it != index_by_native_session_id.end())
			{
				ChatSession& existing = deduped[native_it->second];

				if (ShouldReplaceChatForDuplicateId(chat, existing) || existing.id != existing.native_session_id)
				{
					existing = std::move(chat);

					if (existing.id != existing.native_session_id && !existing.native_session_id.empty())
					{
						existing.id = existing.native_session_id;
					}

					index_by_id[existing.id] = native_it->second;
				}

				continue;
			}
		}

		const auto it = index_by_id.find(chat.id);

		if (it == index_by_id.end())
		{
			if (has_native_identity && chat.id != native_session_id)
			{
				chat.id = native_session_id;
			}

			const std::size_t next_index = deduped.size();
			index_by_id[chat.id] = next_index;

			if (has_native_identity)
			{
				index_by_native_session_id[native_key] = next_index;
			}

			deduped.push_back(std::move(chat));
			continue;
		}

		ChatSession& existing = deduped[it->second];

		if (ShouldReplaceChatForDuplicateId(chat, existing))
		{
			existing = std::move(chat);

			if (existing.uses_native_session && !existing.native_session_id.empty() && existing.id != existing.native_session_id)
			{
				existing.id = existing.native_session_id;
			}
		}

		if (existing.uses_native_session && !existing.native_session_id.empty())
		{
			index_by_native_session_id["native:" + existing.native_session_id] = it->second;
		}
	}

	SortChatsByRecent(deduped);
	return deduped;
}

void ChatDomainService::RefreshRememberedSelection(AppState& app) const
{
	if (!app.settings.remember_last_chat)
	{
		app.settings.last_selected_chat_id.clear();
		return;
	}

	const ChatSession* selected = SelectedChat(app);
	app.settings.last_selected_chat_id = (selected != nullptr) ? selected->id : "";
}

void ChatDomainService::SelectChatById(AppState& app, const std::string& chat_id) const
{
	const ChatSession* previously_selected = SelectedChat(app);
	const std::string previous_id = (previously_selected != nullptr) ? previously_selected->id : "";
	app.selected_chat_index = FindChatIndexById(app, chat_id);

	if (app.selected_chat_index >= 0)
	{
		app.chats_with_unseen_updates.erase(app.chats[app.selected_chat_index].id);
	}

	if (previous_id != chat_id)
	{
		app.composer_text.clear();
	}

	RefreshRememberedSelection(app);
}

ChatSession ChatDomainService::CreateNewChat(const std::string& folder_id, const std::string& provider_id) const
{
	ChatSession chat;
	chat.id = NewSessionId();
	chat.provider_id = provider_id;
	chat.parent_chat_id.clear();
	chat.branch_root_chat_id = chat.id;
	chat.branch_from_message_index = -1;
	chat.folder_id = folder_id;
	chat.created_at = TimestampNow();
	chat.updated_at = chat.created_at;
	chat.title = "Chat " + chat.created_at;
	return chat;
}

void ChatDomainService::AddMessage(ChatSession& chat, const MessageRole role, const std::string& text) const
{
	Message message;
	message.role = role;
	message.content = text;
	message.created_at = TimestampNow();
	chat.messages.push_back(std::move(message));
	chat.updated_at = TimestampNow();

	if (chat.messages.size() == 1 && role == MessageRole::User)
	{
		std::string maybe_title = Trim(text);

		if (maybe_title.size() > 48)
		{
			maybe_title = maybe_title.substr(0, 45) + "...";
		}

		if (!maybe_title.empty())
		{
			chat.title = maybe_title;
		}
	}
}

ChatDomainService& GetChatDomainService()
{
	static ChatDomainService service;
	return service;
}
