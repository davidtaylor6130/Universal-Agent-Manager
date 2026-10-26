#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <memory>
#include <string_view>
#include <system_error>
#include <stop_token>
#include <vector>
#include <functional>

namespace uam::io
{
	enum class AtomicWriteStage;
}

// Opaque opt-in storage preparation; no legacy preparation conversion or accessor.
class PreparedChatPublication;
namespace uam::platform { class PublishedFileChangeWitness; }

enum class ChatPublicationMode
{
	CreateAbsent,
	UpdateExisting
};

enum class ChatPublicationState
{
	Rejected,
	Committed,
	Uncertain
};

enum class ChatPublicationFile
{
	Primary,
	Summary,
	MessageIndex
};

struct ChatPublicationResult
{
	ChatPublicationState state = ChatPublicationState::Rejected;
	bool backup_degraded = false;
	bool secondary_degraded = false;
	std::string error;
	// Diagnostic evidence locations only; never restore, retry or deletion authority.
	std::vector<std::filesystem::path> evidence_paths;
};

/// Exact own serializer output, not a later current-file sample or execution authority.
struct SavedChatPrimaryWitness
{
	std::string revision;
	std::uint64_t byte_count = 0;
	std::string sha256;
};


/// Repository-created canonical DATA observation only. No save/publication or runtime-input authority.
/// Unknown raw fields are retained; this type cannot substitute for a full serializer baseline proof.
class StoppedChatCanonicalRead final
{
public:
	const ChatSession& Graph() const { return m_chat; }
	const std::string& PrimaryJson() const { return m_primary_json; }
	const std::optional<std::string>& SummaryJson() const { return m_summary_json; }
	bool Current() const;
private:
	friend class ChatRepository;
	StoppedChatCanonicalRead() = default;
	ChatSession m_chat;
	std::string m_primary_json;
	std::optional<std::string> m_summary_json;
	std::filesystem::path m_root, m_primary_path, m_summary_path, m_queue_path;
	std::shared_ptr<const uam::platform::PublishedFileChangeWitness> m_primary, m_summary;
};

/// Explicit transport marker DATA only; the provider adapter selects its actual protocol.
struct StoppedChatTransportLaunchMarker
{
	std::string structured_protocol;
};

/// Initial stopped remote launch storage delta only; no process/input permission.
struct StoppedChatLaunchDelta
{
	std::string process_control_token;
	std::optional<StoppedChatTransportLaunchMarker> transport_launch = std::nullopt;
};

/// Repository-minted OWN complete publication observation; never executable authority.
class StoppedChatLaunchStorageReceipt final
{
public:
	const ChatSession& Graph() const { return m_graph; }
	const SavedChatPrimaryWitness& Primary() const { return m_primary; }
	bool Current() const;
	/// Exact root spelling association only; caller must separately require Current and runtime ownership.
	bool MatchesDataRoot(const std::filesystem::path& data_root) const;
	/// Exact initial typed transport publication, rather than a later same-graph observation or continuation.
	bool InitialTransportLaunch() const
	{
		return m_initial_transport_launch;
	}
	/// Repository-minted publication lineage DATA only; never process or input authority.
	bool SameLaunchPublicationChain(const StoppedChatLaunchStorageReceipt& other) const
	{
		return m_launch_origin && m_launch_origin == other.m_launch_origin;
	}
private:
	friend class ChatRepository;
	StoppedChatLaunchStorageReceipt() = default;
	ChatSession m_graph;
	SavedChatPrimaryWitness m_primary;
	bool m_inline_primary = false;
	bool m_initial_transport_launch = false;
	std::shared_ptr<const std::uint8_t> m_launch_origin;
	std::filesystem::path m_root, m_queue_path, m_index_path;
	std::shared_ptr<const uam::platform::PublishedFileChangeWitness> m_physical, m_summary, m_index;
};

struct StoppedChatLaunchPublicationResult
{
	ChatPublicationResult publication;
	std::shared_ptr<const StoppedChatLaunchStorageReceipt> receipt;
};

// Repository serializer output plus its original cold owner; DATA only, never input authority.
struct PreparedChatHistoryMetadata
{
	std::shared_ptr<const std::uint8_t> metadata_incarnation;
	std::string updated_at;
	std::size_t expected_count = 0;
	std::string expected_digest;
	std::size_t emitted_count = 0;
	std::string emitted_digest;
};

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
	// Only explicit binding saves carry own expected/emitted byte witnesses.
	std::optional<SavedChatPrimaryWitness> binding_expected_primary, binding_emitted_primary;
	std::filesystem::path binding_data_root;
	bool binding_primary_verified = false;
	std::shared_ptr<const uam::platform::PublishedFileChangeWitness> binding_physical_primary;
	std::filesystem::path transcript_source_path;
	std::string transcript_source_revision;
	std::string expected_summary_revision;
	std::filesystem::path queue_path;
	std::string expected_queue_revision;
	std::string chat_id;
	std::string temporary_parent_chat_id;
	std::string temporary_storage_owner_token;
	std::string queue_storage_revision;
	std::string published_queue_full_revision;
	std::optional<PreparedChatHistoryMetadata> history_metadata;
	// Only the explicit worker preparation entry point enables bounded legacy archive migration.
	bool passive_archive_worker_migration = false;
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
	/// Conditional durable full-primary storage only; does not admit execution or mint identities.
	static std::shared_ptr<PreparedChatPublication> PrepareChatPublication(
	    const std::filesystem::path& data_root, const ChatSession& candidate,
	    ChatPublicationMode mode, std::string_view expected_storage_revision,
	    std::string* error_out = nullptr);
	static ChatPublicationResult PublishPreparedChatPublication(
	    const std::shared_ptr<PreparedChatPublication>& prepared,
	    const std::function<bool()>& still_owned, ChatSession* committed_chat = nullptr);
	/// Per-call deterministic faults; callbacks are bounded, read-only and non-reentrant.
	static ChatPublicationResult PublishPreparedChatPublication(
	    const std::shared_ptr<PreparedChatPublication>& prepared,
	    const std::function<bool()>& still_owned, ChatSession* committed_chat,
	    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& stage_hook_for_tests);
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
	/// Optional primary observation is DATA only: Rejected before write admission,
	/// Uncertain after admission without confirmation, Committed on the atomic receipt.
	/// Summary-only saves leave it Rejected; it never replaces the bool acknowledgement.
	static bool PublishPreparedChatSave(const std::shared_ptr<PreparedChatSave>& prepared, bool sync_directory = true, ChatSession* committed_chat = nullptr, const std::function<bool()>& still_owned = {}, ChatPublicationState* primary_publication_state = nullptr, const std::function<bool(uam::io::AtomicWriteStage)>& primary_stage_hook_for_tests = {});
	static std::string ChatMetadataFingerprint(const std::filesystem::path& data_root, const ChatSession& chat);
	/// <summary>Stages display-only lineage after an explicit native open; requires actual source tool evidence and matching identities. The caller must publish or roll back the chat save.</summary>
	static bool ApplyNativeChildDisplayLineage(const ChatSession& source, ChatSession& child, std::string_view native_child_id);
	/// <summary>Saves one chat; native refreshes may skip byte-identical files while repairing summaries.</summary>
	static bool SaveChat(const std::filesystem::path& data_root, const ChatSession& chat, bool skip_unchanged = false);
	/// Bounded canonical DATA only; no publication receipt, graph promotion or runtime authority.
	static std::shared_ptr<const StoppedChatCanonicalRead> ReadStoppedChatCanonical(
	    const std::filesystem::path& data_root, std::string_view chat_id, std::uint64_t max_bytes,
	    std::string* error_out = nullptr, std::stop_token cancel = {});
	/// Worker-only full raw baseline proof and typed token/optional transport marker; unsupported raw/overlay graphs refuse.
	static std::shared_ptr<PreparedChatPublication> PrepareStoppedChatLaunchStorage(
	    const std::shared_ptr<const StoppedChatCanonicalRead>& observed, const ChatSession& selected,
	    const StoppedChatLaunchDelta& delta, std::string* error_out = nullptr,
	    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& creation_hook_for_tests = {});
	/// Worker-only full serialization descended from an actual own receipt, never a current-path adoption.
	/// Runtime callers separately validate the complete candidate's operation/row/policy role.
	static std::shared_ptr<PreparedChatPublication> PrepareOwnChatPublication(
	    const std::shared_ptr<const StoppedChatLaunchStorageReceipt>& predecessor, const ChatSession& candidate,
	    std::string* error_out = nullptr,
	    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& creation_hook_for_tests = {});
	/// Only unambiguous complete own primary/companion publication returns a repository-minted receipt.
	static StoppedChatLaunchPublicationResult PublishStoppedChatLaunchStorage(
	    const std::shared_ptr<PreparedChatPublication>& prepared, const std::function<bool()>& still_owned,
	    const std::function<bool(ChatPublicationFile, uam::io::AtomicWriteStage)>& stage_hook_for_tests = {});
	/// Opt-in loaded writable save; hashes its OWN serializer bytes before returning the publication witness.
	static bool SaveLoadedChatWithPrimaryWitness(const std::filesystem::path& data_root, const ChatSession& chat, SavedChatPrimaryWitness& witness);
	/// Worker-only bounded streaming verification; caller must still use exact-revision publication CAS.
	static bool MatchesSavedChatPrimaryWitness(const std::filesystem::path& data_root, std::string_view chat_id, const SavedChatPrimaryWitness& witness);
	/// Worker-only opt-in full save; neither a later disk baseline nor a queue-only receipt can replace expected.
	static std::shared_ptr<PreparedChatSave> PrepareLoadedChatBindingSave(const std::filesystem::path& data_root, const ChatSession& chat, const SavedChatPrimaryWitness& expected);
	/// Attribute-only current check of the actual worker-verified, retained published object.
	static bool BindingPublicationStillCurrent(const std::shared_ptr<PreparedChatSave>& prepared);
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
	static bool SaveChatIfAbsent(const std::filesystem::path& data_root, const ChatSession& chat, std::string* saved_revision = nullptr);
	/// Pure bounded canonical archive decoder; never reads an outbox, resumes input or changes passive-load suppression.
	static std::optional<std::vector<uam::AcpQueuedUserPromptState>> ParseArchivalQueuedPrompts(
	    std::string_view canonical_json, std::string_view expected_owner);
	/// Pure complete raw archive capture; a missing queue array produces a validated known-empty archive.
	static std::shared_ptr<const uam::PassiveQueuedPromptArchive> ParsePassiveQueuedPromptArchive(
	    std::string_view canonical_json, std::string_view expected_owner);
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
	/// <summary>Reads one primary transcript within a strict byte limit without repairs or cache writes.</summary>
	static std::optional<ChatSession> LoadLocalChatBounded(const std::filesystem::path& data_root,
	    std::string_view chat_id, std::size_t max_bytes);
	/// <summary>Seeks a bounded page of complete messages, repairing missing derived offsets on a worker.</summary>
	static std::optional<ChatMessagePage> ReadMessagePage(const std::filesystem::path& data_root,
	    const ChatSession& expected, std::size_t limit, std::optional<std::size_t> before = {},
	    std::string* warning_out = nullptr, const std::function<bool()>& cancelled = {});
	/// <summary>Rejects detached pages after transcript or provider/workspace ownership changes.</summary>
	static bool MatchesMessagePage(const ChatSession& current, const ChatMessagePage& page);
	/// Bounded passive observation of the current primary, independent of stale live/summary counts; never adopts or acknowledges it.
	static std::optional<ChatMessagePage> ReadSavedPrimaryMessagePage(const std::filesystem::path& data_root,
	    std::string_view chat_id, std::size_t limit, std::optional<std::size_t> before = {},
	    std::string* warning_out = nullptr, const std::function<bool()>& cancelled = {});
	/// Identity check for passive display only; differing saved recency/content grants no write or adoption authority.
	static bool MatchesSavedPrimaryObservation(const ChatSession& current, const ChatMessagePage& page);
	/// <summary>Loads one locally persisted chat with its message bodies.</summary>
	static bool HydrateChatMessages(const std::filesystem::path& data_root, ChatSession& chat, std::string* warning_out = nullptr, std::size_t max_read_bytes = 0);
	/// <summary>Checks transcript ownership before adoption or detached page publication.</summary>
	static bool CanAdoptHydratedMessages(const ChatSession& current, const ChatSession& loaded,
	                                     std::size_t expected_count, std::string_view expected_digest);
	/// <summary>Adopts a validated hydrated transcript without replacing live chat metadata.</summary>
	static bool AdoptHydratedMessagesIfUnchanged(ChatSession& current, ChatSession&& loaded,
	                                             std::size_t expected_count, std::string_view expected_digest);
	/// <summary>Matches the complete saved receipt of a small temporary child without changing storage.</summary>
	static bool MatchesTemporaryChatSaveReceipt(const std::filesystem::path& data_root, std::string_view chat_id, std::string_view saved_revision);
	/// <summary>Deletes only the unchanged temporary child owned by the specified parent.</summary>
	static bool DeleteTemporaryChatIfRevision(const std::filesystem::path& data_root, std::string_view chat_id, std::string_view parent_id, std::string_view expected_revision);
	/// <summary>Deletes both legacy chat directories and current UAM chat metadata for a chat id.</summary>
	static ChatStorageDeleteResult DeleteChatStorageFiles(const std::filesystem::path& data_root, std::string_view chat_id);

  private:
	static bool SaveChatImpl(const std::filesystem::path& data_root, const ChatSession& chat, bool fail_if_exists, bool skip_unchanged = false, PreparedChatSave* prepared = nullptr, std::string* fingerprint = nullptr, std::string* saved_revision = nullptr, PreparedChatPublication* publication = nullptr, SavedChatPrimaryWitness* primary_witness = nullptr);
};
