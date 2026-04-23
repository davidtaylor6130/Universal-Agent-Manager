#include "chat_domain_service.h"

#include "app/application_core_helpers.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_branching.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal_common.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <unordered_map>

using uam::AppState;
namespace
{
	bool ShouldAutoReplaceTitleFromFirstUserMessage(const ChatSession& chat, const MessageRole role)
	{
		return chat.messages.empty() && role == MessageRole::User && Trim(chat.title) == "New Session";
	}

	void AutoReplaceTitleFromFirstUserMessage(ChatSession& chat, const std::string& text)
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

	std::string NormalizeNativeIdentityWorkspace(const ChatSession& chat)
	{
		const std::string trimmed_workspace = Trim(chat.workspace_directory);

		if (!trimmed_workspace.empty())
		{
			return std::filesystem::path(trimmed_workspace).lexically_normal().generic_string();
		}

		const std::string trimmed_folder = Trim(chat.folder_id);
		return trimmed_folder.empty() ? "" : trimmed_folder;
	}

	std::string NativeIdentityKey(const ChatSession& chat)
	{
		return Trim(chat.provider_id) + "|" + NormalizeNativeIdentityWorkspace(chat) + "|" + Trim(chat.native_session_id);
	}

	std::string RecentChatTimestamp(const ChatSession& chat)
	{
		return chat.last_opened_at.empty() ? chat.updated_at : chat.last_opened_at;
	}

	std::string HashNativeIdentityKey(const std::string& key)
	{
		std::uint64_t hash = 1469598103934665603ull;

		for (const unsigned char ch : key)
		{
			hash ^= ch;
			hash *= 1099511628211ull;
		}

		std::ostringstream out;
		out << std::hex << hash;
		return out.str();
	}
} // namespace

std::string ChatDomainService::NewFolderId() const
{
	const std::string uuid = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (!uuid.empty())
	{
		return "folder-" + uuid;
	}

	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	std::mt19937 rng(std::random_device{}());
	std::uniform_int_distribution<int> hex_digit(0, 15);
	std::ostringstream id;
	id << "folder-" << epoch_ms << "-";

	for (int i = 0; i < 8; ++i)
	{
		id << std::hex << hex_digit(rng);
	}

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
	(void)app;
}

void ChatDomainService::EnsureNewChatFolderSelection(AppState& app) const
{
	if (!app.new_chat_folder_id.empty() && FindFolderById(app, app.new_chat_folder_id) == nullptr)
	{
		app.new_chat_folder_id.clear();
	}
}

void ChatDomainService::NormalizeChatFolderAssignments(AppState& app) const
{
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
	if (!app.new_chat_folder_id.empty() && FindFolderById(app, app.new_chat_folder_id) != nullptr)
	{
		return app.new_chat_folder_id;
	}

	return "";
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
	std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) {
		const std::string a_recent = RecentChatTimestamp(a);
		const std::string b_recent = RecentChatTimestamp(b);
		if (a_recent != b_recent)
		{
			return a_recent > b_recent;
		}

		if (a.updated_at != b.updated_at)
		{
			return a.updated_at > b.updated_at;
		}

		return a.created_at > b.created_at;
	});
}

bool ChatDomainService::ShouldReplaceChatForDuplicateId(const ChatSession& candidate, const ChatSession& existing) const
{
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
	std::unordered_map<std::string, std::size_t> index_by_native_identity;

	for (ChatSession& chat : chats)
	{
		chat.id = Trim(chat.id);

		if (chat.id.empty())
		{
			continue;
		}

		const std::string native_session_id = Trim(chat.native_session_id);
		const bool has_native_identity = !native_session_id.empty();
		const std::string native_key = has_native_identity ? NativeIdentityKey(chat) : std::string{};

		if (has_native_identity)
		{
			const auto native_it = index_by_native_identity.find(native_key);

			if (native_it != index_by_native_identity.end())
			{
				ChatSession& existing = deduped[native_it->second];

				if (ShouldReplaceChatForDuplicateId(chat, existing))
				{
					const std::string previous_id = existing.id;
					existing = std::move(chat);
					if (existing.id != previous_id)
					{
						index_by_id.erase(previous_id);
						index_by_id[existing.id] = native_it->second;
					}
				}

				continue;
			}

			const auto id_it = index_by_id.find(chat.id);
			if (id_it != index_by_id.end())
			{
				ChatSession& existing = deduped[id_it->second];
				const std::string existing_native_key = Trim(existing.native_session_id).empty() ? std::string{} : NativeIdentityKey(existing);

				if (!existing_native_key.empty() && existing_native_key == native_key)
				{
					if (ShouldReplaceChatForDuplicateId(chat, existing))
					{
						const std::string previous_id = existing.id;
						existing = std::move(chat);
						if (existing.id != previous_id)
						{
							index_by_id.erase(previous_id);
							index_by_id[existing.id] = id_it->second;
						}
						index_by_native_identity[native_key] = id_it->second;
					}

					continue;
				}

				chat.id = chat.id + "--" + HashNativeIdentityKey(native_key);
				while (index_by_id.find(chat.id) != index_by_id.end())
				{
					chat.id.push_back('_');
				}
			}

			const std::size_t next_index = deduped.size();
			index_by_native_identity[native_key] = next_index;
			index_by_id[chat.id] = next_index;
			deduped.push_back(std::move(chat));
			continue;
		}

		const auto it = index_by_id.find(chat.id);

		if (it == index_by_id.end())
		{
			const std::size_t next_index = deduped.size();
			index_by_id[chat.id] = next_index;

			deduped.push_back(std::move(chat));
			continue;
		}

		ChatSession& existing = deduped[it->second];

		if (ShouldReplaceChatForDuplicateId(chat, existing))
		{
			existing = std::move(chat);
		}

		if (!existing.native_session_id.empty())
		{
			index_by_native_identity[NativeIdentityKey(existing)] = it->second;
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
	chat.last_opened_at = chat.created_at;
	chat.title = "Chat " + chat.created_at;
	return chat;
}

bool ChatDomainService::CreateBranchFromMessage(AppState& app, const std::string& source_chat_id, const int message_index) const
{
	const int source_index = FindChatIndexById(app, source_chat_id);

	if (source_index < 0)
	{
		app.status_line = "Branch source chat no longer exists.";
		return false;
	}

	const ChatSession source = app.chats[source_index];

	if (message_index < 0 || message_index >= static_cast<int>(source.messages.size()))
	{
		app.status_line = "Branch source message is no longer valid.";
		return false;
	}

	if (source.messages[message_index].role != MessageRole::User)
	{
		app.status_line = "Branching is currently supported for user messages only.";
		return false;
	}

	ChatSession branch = CreateNewChat(source.folder_id, source.provider_id);
	branch.native_session_id.clear();
	branch.parent_chat_id = source.id;
	branch.branch_root_chat_id = source.branch_root_chat_id.empty() ? source.id : source.branch_root_chat_id;
	branch.branch_from_message_index = message_index;
	branch.linked_files = source.linked_files;
	branch.workspace_directory = ResolveWorkspaceRootPath(app, source).string();
	branch.messages.assign(source.messages.begin(), source.messages.begin() + message_index + 1);
	branch.updated_at = TimestampNow();
	branch.last_opened_at = branch.updated_at;
	branch.title = Trim(source.messages[message_index].content);

	if (branch.title.size() > 40)
	{
		branch.title = branch.title.substr(0, 37) + "...";
	}

	branch.title = branch.title.empty() ? "Branch Chat" : ("Branch: " + branch.title);

	app.chats.push_back(branch);
	ChatBranching::Normalize(app.chats);
	SortChatsByRecent(app.chats);
	SelectChatById(app, branch.id);
	PersistenceCoordinator().SaveSettings(app);

	if (app.selected_chat_index >= 0 && app.selected_chat_index < static_cast<int>(app.chats.size()) && ProviderResolutionService().ChatUsesCliOutput(app, app.chats[app.selected_chat_index]))
	{
		MarkSelectedCliTerminalForLaunch(app);
	}

	const ProviderProfile& branch_provider = ProviderResolutionService().ProviderForChatOrDefault(app, branch);

	if (!ProviderRuntime::SaveHistory(branch_provider, app.data_root, branch))
	{
		app.status_line = "Branch created in memory, but failed to save.";
		return false;
	}

	app.status_line = "Branch chat created.";
	return true;
}

void ChatDomainService::ConsumePendingBranchRequest(AppState& app) const
{
	if (app.pending_branch_chat_id.empty())
	{
		return;
	}

	const std::string chat_id = app.pending_branch_chat_id;
	const int message_index = app.pending_branch_message_index;
	app.pending_branch_chat_id.clear();
	app.pending_branch_message_index = -1;
	CreateBranchFromMessage(app, chat_id, message_index);
}

void ChatDomainService::AddMessage(ChatSession& chat, const MessageRole role, const std::string& text) const
{
	const bool should_auto_replace_title = ShouldAutoReplaceTitleFromFirstUserMessage(chat, role);

	Message message;
	message.role = role;
	message.content = text;
	message.created_at = TimestampNow();
	chat.messages.push_back(std::move(message));
	chat.updated_at = TimestampNow();

	if (should_auto_replace_title)
	{
		AutoReplaceTitleFromFirstUserMessage(chat, text);
	}
}

void ChatDomainService::AddMessageWithAnalytics(ChatSession& chat, const MessageRole role, const std::string& text, const std::string& provider, const int64_t input_tokens, const int64_t output_chars, const int64_t time_to_first_token_ms, const int64_t processing_time_ms, const bool interrupted) const
{
	const bool should_auto_replace_title = ShouldAutoReplaceTitleFromFirstUserMessage(chat, role);

	Message message;
	message.role = role;
	message.content = text;
	message.created_at = TimestampNow();
	message.provider = provider;
	message.tokens_input = static_cast<int>(input_tokens);
	message.tokens_output = static_cast<int>(output_chars / 4);
	message.time_to_first_token_ms = static_cast<int>(time_to_first_token_ms);
	message.processing_time_ms = static_cast<int>(processing_time_ms);
	message.interrupted = interrupted;

	static const double kCostPerMillionInputTokens = 0.075;
	static const double kCostPerMillionOutputTokens = 0.30;
	message.estimated_cost_usd = (input_tokens * kCostPerMillionInputTokens + (output_chars / 4) * kCostPerMillionOutputTokens) / 1000000.0;

	chat.messages.push_back(std::move(message));
	chat.updated_at = TimestampNow();

	if (should_auto_replace_title)
	{
		AutoReplaceTitleFromFirstUserMessage(chat, text);
	}
}
