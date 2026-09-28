#pragma once

#include "common/state/app_state.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


class ChatHistorySyncService
{
  public:
	struct RemoteOpenCodeSession
	{
		std::string id;
		std::string title;
		std::string directory;
		std::int64_t created_epoch_ms = 0;
		std::int64_t updated_epoch_ms = 0;
	};
	struct RemoteOpenCodeTranscript
	{
		bool success = false;
		std::string session_id;
		std::string directory;
		std::vector<Message> messages;
		std::string error;
	};
	struct RemoteCodexSession
	{
		std::string id;
		std::string title;
		std::string directory;
		std::int64_t created_epoch_seconds = 0;
		std::int64_t updated_epoch_seconds = 0;
	};
	struct RemoteCodexDiscovery
	{
		std::vector<RemoteCodexSession> sessions;
		std::string error;
	};
	struct RemoteCodexTranscript
	{
		bool success = false;
		std::string session_id;
		std::string directory;
		std::vector<Message> messages;
		std::string error;
	};

	struct ImportResult
	{
		int imported_count = 0;
		int total_count = 0;
		bool success = true;
		std::vector<std::string> errors;

		bool partial() const { return imported_count > 0 && !success; }
		void Fail(std::string error)
		{
			success = false;
			if (!error.empty()) errors.push_back(std::move(error));
		}
		void Merge(const ImportResult& other)
		{
			imported_count += other.imported_count;
			total_count += other.total_count;
			if (!other.success) success = false;
			errors.insert(errors.end(), other.errors.begin(), other.errors.end());
		}
	};
	ImportResult ImportAllNativeChatsToLocal(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id = "") const;
	ImportResult ImportAllNativeChatsByDiscovery(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id = "") const;
	ImportResult ImportProviderChatsForFolder(uam::AppState& app, const std::string& folder_id) const;
	ImportResult ImportRemoteOpenCodeChatsForFolder(
	    uam::AppState& app, const std::string& folder_id,
	    const std::vector<RemoteOpenCodeSession>& sessions) const;
	RemoteOpenCodeTranscript LoadRemoteOpenCodeTranscript(
	    const ExecutionHost& host, const ChatSession& chat, const ProviderProfile& profile, std::stop_token stop_token = {}) const;
	/// Runs a bounded native export; callers must keep this off the GUI thread.
	RemoteOpenCodeTranscript LoadLocalOpenCodeTranscript(const ChatSession& chat, const ProviderProfile& profile, std::stop_token stop_token = {}) const;
	static RemoteOpenCodeTranscript ParseRemoteOpenCodeTranscript(std::string_view output);
	RemoteCodexDiscovery DiscoverRemoteCodexSessions(
	    const ExecutionHost& host, const ChatFolder& folder) const;
	ImportResult ImportRemoteCodexChatsForFolder(
	    uam::AppState& app, const std::string& folder_id,
	    const std::vector<RemoteCodexSession>& sessions) const;
	/// Reads a local rollout without touching application state; run off the GUI thread.
	std::optional<ChatSession> LoadLocalCodexChildChat(const ChatSession& chat, std::string* error = nullptr) const;
	RemoteCodexTranscript LoadRemoteCodexTranscript(
	    const ExecutionHost& host, const ChatSession& chat, std::stop_token stop_token = {}) const;
	static bool AppendRemoteCodexSessions(
	    const nlohmann::json& result, std::vector<RemoteCodexSession>& sessions,
	    std::string* error = nullptr);
	static RemoteCodexTranscript ParseRemoteCodexTranscript(const nlohmann::json& result);
	ImportResult ImportCodexRolloutChatsForFolder(uam::AppState& app, const std::string& folder_id) const;
	bool AddNativeImportTombstones(const std::filesystem::path& data_root, const std::vector<ChatSession>& chats, std::vector<std::string>& added_keys) const;
	bool RemoveNativeImportTombstones(const std::filesystem::path& data_root, const std::vector<std::string>& keys) const;
	/// <summary>Save a validated native transcript before replacing the live chat; keep active or unsaved work.</summary>
	bool SaveNativeTranscript(uam::AppState& app, ChatSession& current, std::vector<Message> messages) const;
	bool SaveChatWithStatus(uam::AppState& app, const ChatSession& chat, const std::string& success, const std::string& failure) const;
	bool RenameChat(uam::AppState& app, ChatSession& chat, const std::string& requested_title) const;
	std::vector<ChatSession> LoadNativeSessionChats(const std::filesystem::path& chats_dir, const ProviderProfile& provider, std::stop_token stop_token = {}) const;
	std::optional<std::filesystem::path> ResolveNativeHistoryChatsDirForWorkspace(const std::filesystem::path& workspace_root) const;
	std::filesystem::path ResolveNativeHistoryChatsDirForChat(const uam::AppState& app, const ChatSession& chat) const;
	void LoadSidebarChats(uam::AppState& app) const;
	void MergeSidebarChatsPreservingCurrent(uam::AppState& app) const;
	void RefreshNativeSessionDirectory(uam::AppState& app) const;
	bool StartAsyncNativeChatLoad(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir) const;
	bool TryConsumeAsyncNativeChatLoad(uam::AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out) const;
	std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& chats) const;
	std::optional<std::filesystem::path> FindNativeSessionFilePath(const std::filesystem::path& chats_dir, const std::string& session_id) const;
	bool DeleteNativeSessionFileForChat(const uam::AppState& app, const ChatSession& chat, std::error_code* error_out = nullptr) const;
	bool DeleteNativeWorkspaceHistoryForFolder(const uam::AppState& app, const ChatFolder& folder, std::error_code* error_out = nullptr) const;
	std::string ResolveResumeSessionIdForChat(const uam::AppState& app, const ChatSession& chat) const;
	void ForgetResolvedNativeSessionForChat(uam::AppState& app, const std::string& chat_id) const;
	/// <summary>Roll back an uncommitted in-memory import without deleting previously saved history.</summary>
	void RollbackOpenNativeSessionChatImport(uam::AppState& app, const std::string& chat_id, const std::string& previous_selected_chat_id) const;
	void RestoreOpenNativeSessionResolvedMapping(uam::AppState& app, const std::string& chat_id, bool had_previous_resolved_native_session, const std::string& previous_resolved_native_session_id) const;
	ChatSession* FindInMemoryNativeSessionChatForOpen(
	    uam::AppState& app,
	    const ChatSession& source_chat,
	    const ProviderProfile& provider,
	    const std::string& native_session_id,
	    bool persist_resolved_mapping = true) const;
	ChatSession* FindOrImportNativeSessionChatForOpen(
	    uam::AppState& app,
	    const ChatSession& source_chat,
	    const ProviderProfile& provider,
	    const std::string& native_session_id,
	    bool persist_provider_normalization = true,
	    const ChatSession* native_snapshot = nullptr) const;
	bool PersistLocalDraftNativeSessionLink(const uam::AppState& app, ChatSession& local_chat, const std::string& native_session_id) const;
	void ApplyLocalOverrides(uam::AppState& app, std::vector<ChatSession>& native_chats, bool persist_local_draft_links = true) const;
	bool TruncateNativeSessionFromDisplayedMessage(const uam::AppState& app, const ChatSession& chat, int displayed_message_index, std::string* error_out) const;
	bool MoveChatToFolder(uam::AppState& app, ChatSession& chat, const std::string& new_folder_id) const;
	bool ExportChatToNative(const uam::AppState& app, const ChatSession& chat) const;

	/// Applies persisted metadata to candidates without replacing live chats or session mappings.
	std::vector<ChatSession> OverlayLocalHistory(const uam::AppState& app, std::vector<ChatSession>& native_chats, bool persist_local_draft_links) const;
};
