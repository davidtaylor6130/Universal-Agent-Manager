#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <optional>
#include <memory>
#include <string_view>
#include <system_error>
#include <stop_token>
#include <vector>
#include <functional>

struct PreparedChatSave
{
	std::filesystem::path primary_path;
	std::filesystem::path primary_temp;
	std::filesystem::path summary_path;
	std::filesystem::path summary_temp;
	std::filesystem::path index_path;
	std::filesystem::path index_temp;
	bool preserve_backup = true;
	std::string expected_storage_revision;
	std::string primary_revision;
	std::filesystem::path transcript_source_path;
	std::string transcript_source_revision;
	std::string expected_summary_revision;
	std::filesystem::path queue_path;
	std::string expected_queue_revision;
	std::string chat_id;
	std::string queue_storage_revision;
	std::string published_queue_full_revision;
	~PreparedChatSave();
};

struct ChatMessagePage
{
	ChatSession chat;
	std::size_t start_index = 0;
	std::size_t total_count = 0;
	std::string messages_digest;
	std::string storage_revision;
	std::optional<std::vector<std::size_t>> user_message_indices;
};

struct ChatStorageDeleteResult
{
	bool unsafe_chat_id = false;
	std::error_code legacy_directory_error;
	std::error_code metadata_file_error;
	std::error_code metadata_backup_file_error;
	std::error_code summary_file_error;
	std::error_code summary_backup_file_error;
	std::error_code queue_file_error;
	std::error_code message_index_file_error;
	std::error_code message_index_backup_file_error;
	std::error_code computer_use_directory_error;

	bool Failed() const
	{
		return unsafe_chat_id || static_cast<bool>(legacy_directory_error) || static_cast<bool>(metadata_file_error) ||
		       static_cast<bool>(metadata_backup_file_error) || static_cast<bool>(summary_file_error) ||
		       static_cast<bool>(summary_backup_file_error) || static_cast<bool>(queue_file_error) || static_cast<bool>(message_index_file_error) || static_cast<bool>(message_index_backup_file_error) || static_cast<bool>(computer_use_directory_error);
	}
};

/// <summary>
/// Persists local chat metadata and message files.
/// </summary>
class ChatRepository
{
  public:
	/// <summary>Serializes and syncs temporary files on a worker without changing saved chat files.</summary>
	static std::shared_ptr<PreparedChatSave> PrepareChatSave(const std::filesystem::path& data_root, const ChatSession& chat, std::string_view expected_storage_revision = {});
	using ImportMessageProducer = std::function<bool(const std::function<bool(const Message&)>&, ChatSession&)>;
	/// <summary>Stages complete imported messages one at a time without a whole-transcript JSON allocation.</summary>
	static std::shared_ptr<PreparedChatSave> PrepareImportedChatSave(const std::filesystem::path& data_root,
	    ChatSession& metadata, const ImportMessageProducer& produce);
	/// <summary>Captures primary-file metadata without reading transcript bodies; missing files have a stable revision.</summary>
	static std::optional<std::string> ChatStorageRevision(const std::filesystem::path& data_root, std::string_view chat_id);
	static std::optional<std::string> ChatSummaryStorageRevision(const std::filesystem::path& data_root, std::string_view chat_id);
	/// <summary>Publishes prepared files after the caller validates current chat and deletion state.</summary>
	static bool PublishPreparedChatSave(const std::shared_ptr<PreparedChatSave>& prepared, bool sync_directory = true, ChatSession* committed_chat = nullptr, const std::function<bool()>& still_owned = {});
	static std::string ChatMetadataFingerprint(const std::filesystem::path& data_root, const ChatSession& chat);
	/// <summary>Saves one chat; native refreshes may skip byte-identical files while repairing summaries.</summary>
	static bool SaveChat(const std::filesystem::path& data_root, const ChatSession& chat, bool skip_unchanged = false);
	/// <summary>Durably saves future queued inputs without rewriting an already persisted transcript.</summary>
	static bool SaveQueuedPromptOutbox(const std::filesystem::path& data_root, ChatSession& chat, bool* full_save_required = nullptr, const std::function<bool()>& still_owned = {});
	/// <summary>Queue identity and payload comparisons exclude changing assistant output.</summary>
	static std::string QueueStorageOwner(const ChatSession& chat);
	static std::string QueueAdmissionFingerprint(const ChatSession& chat);
	/// <summary>Persists selection time in the validated summary without rewriting an unchanged transcript.</summary>
	/// <summary>Prepares selection metadata off-thread, preserving the primary transcript when its summary is current.</summary>
	static std::shared_ptr<PreparedChatSave> PrepareLastOpenedAt(const std::filesystem::path& data_root, const ChatSession& metadata);
	static bool SaveLastOpenedAt(const std::filesystem::path& data_root, const ChatSession& chat);
	/// <summary>Saves one chat only when neither its primary nor backup storage already exists.</summary>
	static bool SaveChatIfAbsent(const std::filesystem::path& data_root, const ChatSession& chat);
	/// <summary>Loads locally persisted chat sessions from disk.</summary>
	static std::vector<ChatSession> LoadLocalChats(const std::filesystem::path& data_root, std::string* warning_out = nullptr);
	/// <summary>Loads locally persisted chat sessions without hydrating message bodies.</summary>
	static std::vector<ChatSession> LoadLocalChatSummaries(const std::filesystem::path& data_root, std::string* warning_out = nullptr);
	/// <summary>Reads current primary chat summaries without migration, repair, or backup publication.</summary>
	static std::vector<ChatSession> LoadLocalChatSummariesReadOnly(const std::filesystem::path& data_root, std::stop_token stop_token = {});
	/// <summary>Reads one bounded metadata record without repair or cache publication; unknown or oversized ownership returns no record.</summary>
	static std::optional<ChatSession> LoadLocalChatSummaryReadOnly(const std::filesystem::path& data_root, std::string_view chat_id);
	/// <summary>Loads one locally persisted chat without scanning sibling files.</summary>
	static std::optional<ChatSession> LoadLocalChat(const std::filesystem::path& data_root, std::string_view chat_id, bool include_messages = true, std::string* warning_out = nullptr, bool publish_summary_cache = true);
	/// <summary>Seeks a bounded page of complete messages, repairing missing derived offsets on a worker.</summary>
	static std::optional<ChatMessagePage> ReadMessagePage(const std::filesystem::path& data_root,
	    const ChatSession& expected, std::size_t limit, std::optional<std::size_t> before = {},
	    std::string* warning_out = nullptr, const std::function<bool()>& cancelled = {});
	/// <summary>Rejects detached pages after transcript or provider/workspace ownership changes.</summary>
	static bool MatchesMessagePage(const ChatSession& current, const ChatMessagePage& page);
	/// <summary>Loads one locally persisted chat with its message bodies.</summary>
	static bool HydrateChatMessages(const std::filesystem::path& data_root, ChatSession& chat, std::string* warning_out = nullptr);
	/// <summary>Checks transcript ownership before adoption or detached page publication.</summary>
	static bool CanAdoptHydratedMessages(const ChatSession& current, const ChatSession& loaded,
	                                     std::size_t expected_count, std::string_view expected_digest);
	/// <summary>Adopts a validated hydrated transcript without replacing live chat metadata.</summary>
	static bool AdoptHydratedMessagesIfUnchanged(ChatSession& current, ChatSession&& loaded,
	                                             std::size_t expected_count, std::string_view expected_digest);
	/// <summary>Deletes both legacy chat directories and current UAM chat metadata for a chat id.</summary>
	static ChatStorageDeleteResult DeleteChatStorageFiles(const std::filesystem::path& data_root, std::string_view chat_id);

  private:
	static bool SaveChatImpl(const std::filesystem::path& data_root, const ChatSession& chat, bool fail_if_exists, bool skip_unchanged = false, PreparedChatSave* prepared = nullptr, std::string* fingerprint = nullptr);
};
