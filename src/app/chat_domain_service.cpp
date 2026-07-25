#include "chat_domain_service.h"

#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_ids.h"
#include "common/chat/chat_repository.h"
#include "common/chat/native_chat_identity.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
	namespace chat_identity = uam::chat_identity;

	constexpr std::string_view kPlaceholderNewChatTitle = "New Session";
	constexpr std::string_view kFallbackBranchChatTitle = "Branch Chat";
	constexpr int kFirstUserMessageTitleMaxChars = 48;
	constexpr int kBranchTitleMessageMaxChars = 40;
	constexpr int kEstimatedOutputCharsPerToken = 4;
	constexpr double kTokensPerMillion = 1000000.0;
	constexpr double kCostPerMillionInputTokens = 0.075;
	constexpr double kCostPerMillionOutputTokens = 0.30;

	bool ShouldAutoReplaceTitleFromFirstUserMessage(const ChatSession& chat, const MessageRole role)
	{
		return chat.messages.empty() && role == MessageRole::User && uam::strings::Trim(chat.title) == kPlaceholderNewChatTitle;
	}

	void AutoReplaceTitleFromFirstUserMessage(ChatSession& chat, const std::string& text)
	{
		const std::string maybe_title = uam::strings::TrimAndElide(text, kFirstUserMessageTitleMaxChars);

		if (!maybe_title.empty())
		{
			chat.title = maybe_title;
		}
	}

	void ApplyMessageAnalytics(Message& message, const ChatDomainService::MessageAnalytics& analytics)
	{
		const auto bounded_non_negative_int = [](int64_t value)
		{
			return static_cast<int>(std::clamp<int64_t>(value, 0, std::numeric_limits<int>::max()));
		};
		const int64_t estimated_output_tokens = std::max<int64_t>(0, analytics.output_chars / kEstimatedOutputCharsPerToken);

		message.provider = analytics.provider;
		message.tokens_input = bounded_non_negative_int(analytics.input_tokens);
		message.tokens_output = bounded_non_negative_int(estimated_output_tokens);
		message.time_to_first_token_ms = bounded_non_negative_int(analytics.time_to_first_token_ms);
		message.processing_time_ms = bounded_non_negative_int(analytics.processing_time_ms);
		message.interrupted = analytics.interrupted;
		message.estimated_cost_usd = (static_cast<double>(message.tokens_input) * kCostPerMillionInputTokens + static_cast<double>(message.tokens_output) * kCostPerMillionOutputTokens) / kTokensPerMillion;
	}

	void AppendMessage(ChatSession& chat, const MessageRole role, const std::string& text, const ChatDomainService::MessageAnalytics* analytics = nullptr)
	{
		const bool should_auto_replace_title = ShouldAutoReplaceTitleFromFirstUserMessage(chat, role);
		const std::string timestamp = uam::time::TimestampNow();

		Message message;
		message.role = role;
		message.content = text;
		message.created_at = timestamp;
		if (analytics != nullptr)
		{
			ApplyMessageAnalytics(message, *analytics);
		}

		chat.messages.push_back(std::move(message));
		chat.updated_at = timestamp;

		if (should_auto_replace_title)
		{
			AutoReplaceTitleFromFirstUserMessage(chat, text);
		}
	}

	std::string_view RecentChatTimestamp(const ChatSession& chat)
	{
		// Recency = last message activity (updated_at), NOT selection time.
		// Ordering by last_opened_at floated the just-selected chat to the top,
		// reordering the sidebar on every click. See issue #49.
		return chat.updated_at.empty() ? std::string_view(chat.created_at) : std::string_view(chat.updated_at);
	}

	bool IsValidChatIndex(const std::vector<ChatSession>& chats, int index)
	{
		return index >= 0 && index < static_cast<int>(chats.size());
	}

	template <typename AppStateT> auto SelectedChatOrNull(AppStateT& app) -> decltype(&app.chats[app.selected_chat_index])
	{
		if (!IsValidChatIndex(app.chats, app.selected_chat_index))
		{
			return nullptr;
		}

		return &app.chats[app.selected_chat_index];
	}

	template <typename Range> auto FindById(Range& values, const std::string& id)
	{
		return std::ranges::find_if(values, [&](const auto& value) { return uam::strings::TrimmedEqualsNonEmpty(value.id, id); });
	}

	template <typename Range> int FindIndexById(Range& values, const std::string& id)
	{
		const auto found = FindById(values, id);
		return found == values.end() ? -1 : static_cast<int>(std::ranges::distance(values.begin(), found));
	}

	template <typename Range> auto* PointerOrNull(Range& values, decltype(values.begin()) found)
	{
		return found == values.end() ? nullptr : &*found;
	}

	std::string BranchTitleFromMessage(const std::string& message)
	{
		const std::string title = uam::strings::TrimAndElide(message, kBranchTitleMessageMaxChars);
		return title.empty() ? std::string(kFallbackBranchChatTitle) : "Branch: " + title;
	}

	std::string NativeDeduplicationKeyOrEmpty(const ChatSession& chat)
	{
		return uam::strings::IsBlank(chat.native_session_id) ? std::string{} : chat_identity::NativeIdentityKeyForLocalDeduplication(chat);
	}

	void RegisterChatIndexes(const ChatSession& chat,
	                         std::size_t index,
	                         std::unordered_map<std::string, std::size_t>& index_by_id,
	                         std::unordered_map<std::string, std::size_t>& index_by_native_identity)
	{
		index_by_id[chat.id] = index;

		const std::string native_key = NativeDeduplicationKeyOrEmpty(chat);
		if (!native_key.empty())
		{
			index_by_native_identity[native_key] = index;
		}
	}

	void UnregisterChatIndexes(const ChatSession& chat,
	                           std::unordered_map<std::string, std::size_t>& index_by_id,
	                           std::unordered_map<std::string, std::size_t>& index_by_native_identity)
	{
		index_by_id.erase(chat.id);

		const std::string native_key = NativeDeduplicationKeyOrEmpty(chat);
		if (!native_key.empty())
		{
			index_by_native_identity.erase(native_key);
		}
	}

	void ReplaceDedupedChat(ChatSession& existing,
	                        ChatSession&& candidate,
	                        std::size_t index,
	                        std::unordered_map<std::string, std::size_t>& index_by_id,
	                        std::unordered_map<std::string, std::size_t>& index_by_native_identity)
	{
		UnregisterChatIndexes(existing, index_by_id, index_by_native_identity);
		existing = std::move(candidate);
		RegisterChatIndexes(existing, index, index_by_id, index_by_native_identity);
	}

	bool IsLaterNativeSessionMatch(const ChatSession& candidate, const ChatSession& existing)
	{
		const std::string_view candidate_recent = RecentChatTimestamp(candidate);
		const std::string_view existing_recent = RecentChatTimestamp(existing);
		if (candidate_recent != existing_recent)
		{
			return candidate_recent > existing_recent;
		}

		if (candidate.updated_at != existing.updated_at)
		{
			return candidate.updated_at > existing.updated_at;
		}

		if (candidate.created_at != existing.created_at)
		{
			return candidate.created_at > existing.created_at;
		}

		return candidate.id > existing.id;
	}
} // namespace

std::string ChatDomainService::NewFolderId() const
{
	const std::string uuid = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	return uam::chat_ids::NewFolderId(uuid);
}

int ChatDomainService::FindFolderIndexById(const uam::AppState& app, const std::string& folder_id) const
{
	return FindIndexById(app.folders, uam::strings::Trim(folder_id));
}

const ChatFolder* ChatDomainService::FindFolderById(const uam::AppState& app, const std::string& folder_id) const
{
	return PointerOrNull(app.folders, FindById(app.folders, uam::strings::Trim(folder_id)));
}

ChatFolder* ChatDomainService::FindFolderById(uam::AppState& app, const std::string& folder_id) const
{
	return const_cast<ChatFolder*>(FindFolderById(static_cast<const uam::AppState&>(app), folder_id));
}

void ChatDomainService::EnsureNewChatFolderSelection(uam::AppState& app) const
{
	app.new_chat_folder_id = uam::strings::Trim(app.new_chat_folder_id);
	if (!app.new_chat_folder_id.empty() && FindFolderById(app, app.new_chat_folder_id) == nullptr)
	{
		app.new_chat_folder_id.clear();
	}
}

void ChatDomainService::NormalizeChatFolderAssignments(uam::AppState& app) const
{
	for (ChatSession& chat : app.chats)
	{
		if (!chat.folder_id.empty() || uam::strings::IsBlank(chat.workspace_directory))
		{
			continue;
		}

		const std::filesystem::path chat_workspace = uam::paths::NormalizeExistingPath(
		    PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(
		        uam::strings::Trim(chat.workspace_directory)));

		if (chat_workspace.empty())
		{
			continue;
		}

		for (const ChatFolder& folder : app.folders)
		{
			const std::filesystem::path folder_directory = uam::paths::NormalizeExistingPath(
			    PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(
			        uam::strings::Trim(folder.directory)));

			if (!folder_directory.empty() && chat_workspace == folder_directory)
			{
				chat.folder_id = folder.id;
				break;
			}
		}
	}

	const bool any_expanded_with_chats = std::ranges::any_of(app.folders, [&](const ChatFolder& folder) { return !folder.collapsed && CountChatsInFolder(app, folder.id) > 0; });

	if (!any_expanded_with_chats)
	{
		std::ranges::for_each(app.folders,
		                      [&](ChatFolder& folder)
		                      {
			                      if (CountChatsInFolder(app, folder.id) > 0)
			                      {
				                      folder.collapsed = false;
			                      }
		                      });
	}

	EnsureNewChatFolderSelection(app);
}

std::string ChatDomainService::FolderForNewChat(const uam::AppState& app) const
{
	const std::string target_folder_id = uam::strings::Trim(app.new_chat_folder_id);
	if (!target_folder_id.empty() && FindFolderById(app, target_folder_id) != nullptr)
	{
		return target_folder_id;
	}

	return "";
}

int ChatDomainService::CountChatsInFolder(const uam::AppState& app, const std::string& folder_id) const
{
	const std::string target_folder_id = uam::strings::Trim(folder_id);
	return static_cast<int>(std::ranges::count_if(app.chats, [&](const ChatSession& chat) { return uam::strings::TrimmedEquals(chat.folder_id, target_folder_id); }));
}

std::string ChatDomainService::FolderTitleOrFallback(const ChatFolder& folder) const
{
	return uam::strings::TrimOrFallback(folder.title, "Untitled Folder");
}

std::string ChatDomainService::ChatTitleOrFallback(const ChatSession& chat) const
{
	return uam::strings::TrimOrFallback(chat.title, "Untitled Chat");
}

int ChatDomainService::FindChatIndexById(const uam::AppState& app, const std::string& chat_id) const
{
	return FindIndexById(app.chats, uam::strings::Trim(chat_id));
}

const ChatSession* ChatDomainService::FindChatById(const uam::AppState& app, const std::string& chat_id) const
{
	return PointerOrNull(app.chats, FindById(app.chats, uam::strings::Trim(chat_id)));
}

ChatSession* ChatDomainService::FindChatById(uam::AppState& app, const std::string& chat_id) const
{
	return const_cast<ChatSession*>(FindChatById(static_cast<const uam::AppState&>(app), chat_id));
}

const ChatSession* ChatDomainService::FindChatByNativeSessionId(const uam::AppState& app, const std::string& native_session_id) const
{
	const std::string target_native_session_id = uam::strings::Trim(native_session_id);
	if (target_native_session_id.empty())
	{
		return nullptr;
	}

	const ChatSession* best_match = nullptr;
	int best_priority = 3;

	for (const auto& entry : app.resolved_native_sessions_by_chat_id)
	{
		if (uam::strings::Trim(entry.second) == target_native_session_id)
		{
			if (const ChatSession* resolved_chat = FindChatById(app, entry.first); resolved_chat != nullptr)
			{
				if (best_match == nullptr || best_priority > 0 || (best_priority == 0 && IsLaterNativeSessionMatch(*resolved_chat, *best_match)))
				{
					best_match = resolved_chat;
					best_priority = 0;
				}
			}
		}
	}

	for (const ChatSession& chat : app.chats)
	{
		if (uam::strings::Trim(chat.native_session_id) == target_native_session_id)
		{
			if (best_match == nullptr || best_priority > 1 ||
			    (best_priority == 0 && IsLaterNativeSessionMatch(chat, *best_match)) ||
			    (best_priority == 1 && IsLaterNativeSessionMatch(chat, *best_match)))
			{
				best_match = &chat;
				best_priority = 1;
			}
		}
	}

	return best_match;
}

ChatSession* ChatDomainService::FindChatByNativeSessionId(uam::AppState& app, const std::string& native_session_id) const
{
	return const_cast<ChatSession*>(FindChatByNativeSessionId(static_cast<const uam::AppState&>(app), native_session_id));
}

ChatSession* ChatDomainService::SelectedChat(uam::AppState& app) const
{
	return SelectedChatOrNull(app);
}

const ChatSession* ChatDomainService::SelectedChat(const uam::AppState& app) const
{
	return SelectedChatOrNull(app);
}

std::string ChatDomainService::SelectedChatId(const uam::AppState& app) const
{
	const ChatSession* selected = SelectedChat(app);
	return (selected != nullptr) ? uam::strings::Trim(selected->id) : "";
}

std::string ChatDomainService::SelectedChatProviderId(const uam::AppState& app) const
{
	const ChatSession* selected = SelectedChat(app);
	return (selected != nullptr) ? uam::strings::Trim(selected->provider_id) : "";
}

void ChatDomainService::SetSelectedChatIndexOrNearest(uam::AppState& app, int preferred_index) const
{
	if (app.chats.empty())
	{
		app.selected_chat_index = -1;
		RefreshRememberedSelection(app);
		return;
	}

	const int last_index = static_cast<int>(app.chats.size()) - 1;
	app.selected_chat_index = std::clamp(preferred_index, 0, last_index);
	RefreshRememberedSelection(app);
}

void ChatDomainService::SelectRememberedOrFirstChat(uam::AppState& app) const
{
	int preferred_index = 0;

	app.settings.last_selected_chat_id = uam::strings::Trim(app.settings.last_selected_chat_id);
	if (app.settings.remember_last_chat && !app.settings.last_selected_chat_id.empty())
	{
		preferred_index = FindChatIndexById(app, app.settings.last_selected_chat_id);
	}

	SetSelectedChatIndexOrNearest(app, preferred_index);
}

void ChatDomainService::SortChatsByRecent(std::vector<ChatSession>& chats) const
{
	std::ranges::sort(chats,
	                  [](const ChatSession& a, const ChatSession& b)
	                  {
		                  const std::string_view a_recent = RecentChatTimestamp(a);
		                  const std::string_view b_recent = RecentChatTimestamp(b);
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
		return !uam::strings::IsBlank(candidate.provider_id);
	}

	if (candidate.parent_chat_id != existing.parent_chat_id)
	{
		return !uam::strings::IsBlank(candidate.parent_chat_id);
	}

	if (candidate.branch_root_chat_id != existing.branch_root_chat_id)
	{
		return !uam::strings::IsBlank(candidate.branch_root_chat_id);
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
	index_by_id.reserve(chats.size());
	index_by_native_identity.reserve(chats.size());

	for (ChatSession& chat : chats)
	{
		chat.id = uam::strings::Trim(chat.id);

		if (chat.id.empty())
		{
			continue;
		}

		const std::string native_key = NativeDeduplicationKeyOrEmpty(chat);
		const bool has_native_identity = !native_key.empty();

		if (has_native_identity)
		{
			const auto native_it = index_by_native_identity.find(native_key);

			if (native_it != index_by_native_identity.end())
			{
				ChatSession& existing = deduped[native_it->second];

				if (ShouldReplaceChatForDuplicateId(chat, existing))
				{
					ReplaceDedupedChat(existing, std::move(chat), native_it->second, index_by_id, index_by_native_identity);
				}

				continue;
			}

			const auto id_it = index_by_id.find(chat.id);
			if (id_it != index_by_id.end())
			{
				ChatSession& existing = deduped[id_it->second];
				const std::string existing_native_key = NativeDeduplicationKeyOrEmpty(existing);

				if (!existing_native_key.empty() && existing_native_key == native_key)
				{
					if (ShouldReplaceChatForDuplicateId(chat, existing))
					{
						ReplaceDedupedChat(existing, std::move(chat), id_it->second, index_by_id, index_by_native_identity);
					}

					continue;
				}

				chat.id = chat.id + "--" + chat_identity::NativeIdentityKeyHash(native_key);
				while (index_by_id.contains(chat.id))
				{
					chat.id.push_back('_');
				}
			}

			const std::size_t next_index = deduped.size();
			RegisterChatIndexes(chat, next_index, index_by_id, index_by_native_identity);
			deduped.push_back(std::move(chat));
			continue;
		}

		const auto it = index_by_id.find(chat.id);

		if (it == index_by_id.end())
		{
			const std::size_t next_index = deduped.size();
			RegisterChatIndexes(chat, next_index, index_by_id, index_by_native_identity);
			deduped.push_back(std::move(chat));
			continue;
		}

		ChatSession& existing = deduped[it->second];

		if (ShouldReplaceChatForDuplicateId(chat, existing))
		{
			ReplaceDedupedChat(existing, std::move(chat), it->second, index_by_id, index_by_native_identity);
		}
	}

	SortChatsByRecent(deduped);
	return deduped;
}

void ChatDomainService::RefreshRememberedSelection(uam::AppState& app) const
{
	if (!app.settings.remember_last_chat)
	{
		app.settings.last_selected_chat_id.clear();
		return;
	}

	app.settings.last_selected_chat_id = SelectedChatId(app);
}

void ChatDomainService::SelectChatById(uam::AppState& app, const std::string& chat_id) const
{
	const std::string target_chat_id = uam::strings::Trim(chat_id);
	const std::string previous_id = SelectedChatId(app);
	app.selected_chat_index = FindChatIndexById(app, target_chat_id);

	const std::string selected_id = SelectedChatId(app);
	if (!selected_id.empty())
	{
		app.chats_with_unseen_updates.erase(selected_id);
	}

	if (previous_id != target_chat_id)
	{
		app.composer_text.clear();
	}

	RefreshRememberedSelection(app);
}

ChatSession ChatDomainService::CreateNewChat(const std::string& folder_id, const std::string& provider_id) const
{
	ChatSession chat;
	chat.id = uam::chat_ids::NewChatId();
	chat.provider_id = uam::strings::Trim(provider_id);
	chat.parent_chat_id.clear();
	chat.branch_root_chat_id = chat.id;
	chat.branch_from_message_index = -1;
	chat.folder_id = uam::strings::Trim(folder_id);
	chat.created_at = uam::time::TimestampNow();
	chat.updated_at = chat.created_at;
	chat.last_opened_at = chat.created_at;
	chat.title = "Chat " + chat.created_at;
	return chat;
}

bool ChatDomainService::CreateBranchFromMessage(uam::AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content) const
{
	const int source_index = FindChatIndexById(app, source_chat_id);

	if (source_index < 0)
	{
		app.status_line = "Branch source chat no longer exists.";
		return false;
	}

	std::string hydrate_warning;
	if (!ChatRepository::HydrateChatMessages(app.data_root, app.chats[source_index], &hydrate_warning))
	{
		app.status_line = uam::strings::NonEmptyOrFallback(hydrate_warning, "Failed to load branch source messages.");
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

	if (uam::ChatHasActiveAcpSession(app, source.id) || uam::ChatHasBusyCliTerminal(app, source.id))
	{
		app.status_line = "Wait for the active turn to finish before branching.";
		return false;
	}

	if (replacement_content.has_value() && uam::strings::IsBlank(*replacement_content))
	{
		app.status_line = "Edited message content is required.";
		return false;
	}

	ChatSession branch = CreateNewChat(source.folder_id, source.provider_id);
	branch.native_session_id.clear();
	branch.parent_chat_id = source.id;
	branch.branch_root_chat_id = uam::strings::NonEmptyOrFallback(source.branch_root_chat_id, source.id);
	branch.branch_from_message_index = message_index;
	branch.branch_message_edited = replacement_content.has_value();
	branch.linked_files = source.linked_files;
	branch.model_id = source.model_id;
	branch.reasoning_effort = source.reasoning_effort;
	branch.service_tier = source.service_tier;
	branch.approval_mode = source.approval_mode;
	branch.auto_approve_commands = source.auto_approve_commands;
	branch.command_safety_tier = source.command_safety_tier;
	branch.memory_level = source.memory_level;
	branch.memory_enabled = source.memory_enabled;
	branch.small_model_mode = source.small_model_mode;
	const bool branch_from_git_worktree = uam::paths::HasGitWorktreeSource(source);
	branch.workspace_directory = branch_from_git_worktree ? source.workspace_source_directory : uam::paths::ResolveWorkspaceRootPath(app, source).string();
	branch.messages.assign(source.messages.begin(), source.messages.begin() + message_index + 1);
	if (replacement_content.has_value())
	{
		branch.messages.back().content = *replacement_content;
	}
	branch.updated_at = uam::time::TimestampNow();
	branch.last_opened_at = branch.updated_at;
	branch.title = BranchTitleFromMessage(branch.messages.back().content);

	const ProviderProfile& branch_provider = ProviderResolutionService().ProviderForChatOrDefault(app, branch);
	if (!ProviderRuntime::SaveHistory(branch_provider, app.data_root, branch))
	{
		app.status_line = "Failed to save branch chat.";
		return false;
	}

	const std::string previous_selected_chat_id = SelectedChatId(app);
	app.chats.push_back(branch);
	ChatBranching::Normalize(app.chats);
	SortChatsByRecent(app.chats);
	SelectChatById(app, branch.id);
	if (!PersistenceCoordinator().SaveSettings(app))
	{
		std::erase_if(app.chats, [&branch](const ChatSession& chat) { return chat.id == branch.id; });
		SelectChatById(app, previous_selected_chat_id);
		(void)ChatRepository::DeleteChatStorageFiles(app.data_root, branch.id);
		app.status_line = "Failed to persist branch selection.";
		return false;
	}

	const ChatSession* selected_branch = SelectedChat(app);
	if (selected_branch != nullptr && ProviderResolutionService().ChatUsesCliOutput(app, *selected_branch))
	{
		uam::MarkSelectedCliTerminalForLaunch(app);
	}

	app.status_line = replacement_content.has_value() ? "Edited message opened in a new branch." : "Chat reverted in a new branch.";
	return true;
}

void ChatDomainService::AddMessage(ChatSession& chat, const MessageRole role, const std::string& text) const
{
	AppendMessage(chat, role, text);
}

void ChatDomainService::AddMessageWithAnalytics(ChatSession& chat, const MessageRole role, const std::string& text, const MessageAnalytics& analytics) const
{
	AppendMessage(chat, role, text, &analytics);
}
