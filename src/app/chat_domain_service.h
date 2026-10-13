#pragma once

#include "common/state/app_state.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ChatDomainService
{
  public:
	struct BranchHistoryIdentity
	{
		std::string chat_id;
		std::string updated_at;
		std::string provider_id;
		std::string execution_host_id;
		std::string native_identity_key;
		std::string workspace_directory;
		std::string workspace_isolation_kind;
		std::string workspace_source_directory;
		std::string workspace_base_ref;
		std::string workspace_branch_name;
		std::string workspace_worktree_directory;
		std::size_t message_count = 0;
		std::string messages_digest;
	};

	struct ColdHistoryPreparation
	{
		std::filesystem::path data_root;
		BranchHistoryIdentity identity;
		ChatSession snapshot;
		std::string storage_revision;
		std::shared_ptr<uam::BranchPreparationOwnership> ownership;
	};

	/// <summary>Reserve before reading; prompt and branch actions cannot overtake each other.</summary>
	std::shared_ptr<ColdHistoryPreparation> BeginColdHistoryPreparation(uam::AppState& app, const std::string& chat_id) const;
	/// <summary>Hydrate only a worker snapshot; Stop never joins this read.</summary>
	bool LoadColdHistoryPreparation(ColdHistoryPreparation& preparation, std::string& error) const;
	/// <summary>Consume ownership and validate storage/history before adopting messages.</summary>
	bool CompleteColdHistoryPreparation(uam::AppState& app, ColdHistoryPreparation& preparation) const;

	struct MessageAnalytics
	{
		std::string provider;
		int64_t input_tokens = 0;
		int64_t output_chars = 0;
		int64_t time_to_first_token_ms = 0;
		int64_t processing_time_ms = 0;
		bool interrupted = false;
	};

	std::string AttentionRevision(const uam::AppState& app, const ChatSession& chat) const;
	void MarkChatNeedsAttention(uam::AppState& app, const std::string& chat_id) const;
	bool AcknowledgeChatAttention(uam::AppState& app, const std::string& chat_id, const std::string& revision) const;
	/// Snoozes attention until a future epoch timestamp; zero wakes without stopping the run.
	bool ValidateChatSnooze(const uam::AppState& app, const std::string& chat_id, std::int64_t until_ms, std::string* error_out = nullptr) const;
	bool SetChatSnoozedUntil(uam::AppState& app, const std::string& chat_id, std::int64_t until_ms, std::string* error_out = nullptr) const;
	bool SetChatSettled(uam::AppState& app, const std::string& chat_id, bool settled) const;
	std::string NewFolderId() const;
	int FindFolderIndexById(const uam::AppState& app, const std::string& folder_id) const;
	ChatFolder* FindFolderById(uam::AppState& app, const std::string& folder_id) const;
	const ChatFolder* FindFolderById(const uam::AppState& app, const std::string& folder_id) const;
	void EnsureNewChatFolderSelection(uam::AppState& app) const;
	void NormalizeChatFolderAssignments(uam::AppState& app) const;
	std::string FolderForNewChat(const uam::AppState& app) const;
	int CountChatsInFolder(const uam::AppState& app, const std::string& folder_id) const;
	std::string FolderTitleOrFallback(const ChatFolder& folder) const;
	std::string ChatTitleOrFallback(const ChatSession& chat) const;

	int FindChatIndexById(const uam::AppState& app, const std::string& chat_id) const;
	ChatSession* FindChatById(uam::AppState& app, const std::string& chat_id) const;
	const ChatSession* FindChatById(const uam::AppState& app, const std::string& chat_id) const;
	ChatSession* FindChatByNativeSessionId(uam::AppState& app, const std::string& native_session_id) const;
	const ChatSession* FindChatByNativeSessionId(const uam::AppState& app, const std::string& native_session_id) const;
	ChatSession* SelectedChat(uam::AppState& app) const;
	const ChatSession* SelectedChat(const uam::AppState& app) const;
	std::string SelectedChatId(const uam::AppState& app) const;
	std::string SelectedChatProviderId(const uam::AppState& app) const;
	void SetSelectedChatIndexOrNearest(uam::AppState& app, int preferred_index) const;
	void SelectRememberedOrFirstChat(uam::AppState& app) const;
	void SortChatsByRecent(std::vector<ChatSession>& chats) const;
	bool ShouldReplaceChatForDuplicateId(const ChatSession& candidate, const ChatSession& existing) const;
	std::vector<ChatSession> DeduplicateChatsById(std::vector<ChatSession> chats) const;
	void RefreshRememberedSelection(uam::AppState& app) const;
	void SelectChatById(uam::AppState& app, const std::string& chat_id) const;
	BranchHistoryIdentity CaptureBranchHistoryIdentity(const ChatSession& chat) const;
	bool MatchesBranchHistoryIdentity(const ChatSession& chat, const BranchHistoryIdentity& identity) const;
	std::optional<ChatSession> LoadBranchHistoryDetached(const std::filesystem::path& data_root,
	                                                     const BranchHistoryIdentity& identity) const;
	/// <summary>Reserve the source before a cold read; an outstanding request cannot be replaced.</summary>
	std::shared_ptr<uam::BranchPreparationOwnership> BeginBranchPreparation(uam::AppState& app, const std::string& source_id, const std::string& existing_branch_id) const;
	/// <summary>Consume only this request's reservation before any completion side effects.</summary>
	bool ConsumeBranchPreparation(uam::AppState& app, const std::string& source_id, const std::shared_ptr<uam::BranchPreparationOwnership>& ownership) const;
	void CancelBranchPreparations(uam::AppState& app, const std::string& chat_id) const;
	bool MatchesPreparedBranchHistory(const ChatSession& current, const BranchHistoryIdentity& identity, bool was_loaded, const ChatSession* loaded) const;
	ChatSession CreateNewChat(const std::string& folder_id, const std::string& provider_id, bool automatic_worktree = true) const;
	bool CreateBranchFromMessage(uam::AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content = std::nullopt, const std::optional<std::string>& operation_id = std::nullopt, bool* reused_existing_branch = nullptr, const std::optional<std::vector<std::string>>& retained_attachment_ids = std::nullopt, const ChatSession* prepared_source = nullptr, const ChatSession* prepared_existing_branch = nullptr) const;
	void AddMessage(ChatSession& chat, MessageRole role, const std::string& text) const;
	void AddMessageWithAnalytics(ChatSession& chat, MessageRole role, const std::string& text, const MessageAnalytics& analytics) const;
};
