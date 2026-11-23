#pragma once

#include "common/state/app_state.h"
#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam
{
	struct ManagedAgentTranscriptReadIdentity;
	struct ManagedWorktreeFileWitness;
	/// Immutable server-held authority. Never accept its contents from the renderer.
	class ManagedSubagentControlScope
	{
	  public:
		bool operator==(const ManagedSubagentControlScope&) const = default;

	  private:
		friend class ManagedSubagentControlService;
		struct RunOwnership
		{
			std::string identity;
			std::string transcript_identity;
			std::shared_ptr<const std::uint8_t> transcript_incarnation;
			std::string root_birth;
			std::string transcript_birth;
			bool operator==(const RunOwnership&) const = default;
		};
		std::shared_ptr<const std::uint8_t> app_incarnation;
		std::filesystem::path data_root;
		std::string root_chat_id;
		std::string root_created_at;
		std::string root_birth;
		std::shared_ptr<const ManagedInitialHistoryCatalogue> history_catalogue;
		std::shared_ptr<const std::uint8_t> root_incarnation;
		std::unordered_map<std::string, RunOwnership> runs;
	};

	class ManagedSubagentCancellationReceipt
	{
		friend class ManagedSubagentControlService;
		ManagedSubagentControlScope scope;
		std::string selected_run_id;
	};

	struct ManagedSubagentRun
	{
		std::string run_id;
		std::string parent_run_id;
		std::string agent_id;
		std::string transcript_chat_id;
		std::string status;
		std::string provider_id;
		std::string model_id;
		std::optional<std::string> host;
		std::string created_at;
		std::string started_at;
		std::string completed_at;
		int64_t started_at_epoch_ms = 0;
		int64_t finished_at_epoch_ms = 0;
		std::string execution_capability;
		bool can_cancel = false;
		/// Bounded output preview for this owned row; never the task or diagnostics.
		std::string summary;
		bool last_reported = false;
		/// Presentation-only label of the owned task (first meaningful line, bounded); never a result, definition or diagnostic.
		std::string task_title;
	};

	struct ManagedSubagentPage
	{
		ManagedSubagentControlScope scope;
		std::vector<ManagedSubagentRun> runs;
		std::size_t total_count = 0;
		std::size_t offset = 0;
		std::optional<std::size_t> next_offset;
	};

	/// Value-owned metadata only: no borrowed AppState/chat/session pointers or private prompts.
	class ManagedSubagentSnapshot
	{
		friend class ManagedSubagentControlService;
		ManagedSubagentPage page;
		std::vector<AgentRun> records;
		std::string signature;
		std::string selected_run_id;
		std::size_t offset = 0;
		std::size_t limit = 50;
		std::unordered_set<std::string> history_ids;
		bool wants_history_transcript = false;
	};

	struct ManagedSubagentDetachedRead
	{
		bool ready = false;
		ManagedSubagentPage page;
		std::string error;
		std::size_t archive_bytes_read = 0;
		std::optional<ChatSession> history_transcript;
	};

	/// Server-only cleanup authority; never serialized to or supplied by the renderer.
	class ManagedWorktreeFamily
	{
		friend class ManagedSubagentControlService;
		ManagedSubagentSnapshot ownership;
		std::filesystem::path data_root;
		std::vector<std::pair<std::string, std::string>> identities;
		mutable std::unordered_map<std::string, std::string> revisions;
		mutable std::unordered_map<std::string, std::shared_ptr<const ManagedWorktreeFileWitness>> files;
		mutable std::optional<std::string> parent_summary_revision;
		mutable std::weak_ptr<AppState::GitPortRequest> operation;
		mutable bool operation_bound = false;
		std::vector<ChatSession> unrelated_references;
	  public:
		std::vector<std::string> ChatIds() const;
		const std::vector<ChatSession>& UnrelatedReferences() const { return unrelated_references; }
	};

	class ManagedSubagentControlService
	{
	  public:
		static std::shared_ptr<const ManagedWorktreeFamily> CaptureWorktreeFamily(const AppState&, const std::string&, std::string*);
		/// Worker-only validation, repeated immediately before cleanup publication/removal.
		static bool BindWorktreeFamilyParentRevision(const ManagedWorktreeFamily&, const std::string&, const std::string&, std::string*);
		static void BindWorktreeFamilyOperation(const ManagedWorktreeFamily&, const std::shared_ptr<AppState::GitPortRequest>&);
		static bool WorktreeFamilyOperationCurrent(const ManagedWorktreeFamily&, std::string*);
		static bool ValidateWorktreeFamily(const ManagedWorktreeFamily&, std::string*);
		static bool PublishWorktreeCleanup(const ManagedWorktreeFamily&, const ChatSession&, std::string*);
		static bool WorktreeFamilyContains(const ManagedWorktreeFamily&, const ChatSession&);
		/// Workspace-cleanup lineage of a managed child whose run record has expired: the link saved at the child's birth
		/// names this exact root by ID and creation time. Callers must first establish that no run record names the child;
		/// children saved without the link keep run-ledger lineage only. Grants no display, follow-up or execution authority.
		static bool SavedRootLinkMatches(const ChatSession& root, const ChatSession& child);
		/// Once per data root in existing startup/sidebar loading, including empty/error results.
		static void LoadRestartInspection(AppState& app);
		/// Pure display hints; does not inspect disk or create controls/owners.
		static void AddRestartInspectionCounts(const AppState&, std::unordered_map<std::string, std::size_t>&);
		/// Pure exact-row capture for the existing detached transcript API.
		static bool CaptureRestartTranscript(const AppState&, std::string_view root_id, std::string_view transcript_id, ManagedAgentTranscriptReadIdentity&);
		static constexpr std::size_t MaxLedgerRuns = 10000;
		static constexpr std::size_t MaxPageSize = 100;
		static constexpr std::size_t MaxChatMetadata = 20000;
		static constexpr std::size_t MaxArchivedMetadataBytes = 64U * 1024U;
		static constexpr std::size_t MaxArchiveTotalBytes = 1024U * 1024U;

		/// Capture/revalidate on the serialized app owner; these methods perform no filesystem I/O.
		static bool Capture(const AppState& app, const std::string& visible_root_chat_id, ManagedSubagentSnapshot* snapshot_out, std::string* error_out = nullptr, const std::string& run_id = {}, std::size_t offset = 0, std::size_t maximum_count = 50);
		/// Worker-only durable validation and metadata projection; snapshot owns every captured value.
		static ManagedSubagentDetachedRead ReadDetached(const ManagedSubagentSnapshot& snapshot, bool read_archives = true, const std::function<void(const std::filesystem::path&)>& after_primary_size_for_tests = {});
		static bool MatchesSnapshot(const AppState& app, const ManagedSubagentSnapshot& snapshot);
		/// Pure selected UserRoot scope; admission requires the original live OR retired transcript token.
		static bool ValidateFollowupScope(const AppState&, const std::string& root_id, const std::string& run_id, const ManagedSubagentControlScope&, std::string* error_out = nullptr);
		/// Receipt delivery only: hydration may replace the retired token; never grants input permission.
		static bool ValidateFollowupCompletionScope(const AppState&, const std::string& root_id, const std::string& run_id, const ManagedSubagentControlScope&, std::string* error_out = nullptr);
		static bool CancelPrepared(AppState& app, const std::string& visible_root_chat_id, const std::string& run_id, const ManagedSubagentControlScope& original_scope, const ManagedSubagentSnapshot& snapshot, const ManagedSubagentDetachedRead& read, std::string* error_out = nullptr);

		/// Synchronous convenience for non-bridge callers/tests; GUI handlers use the split API above.
		static bool List(const AppState& app, const std::string& visible_root_chat_id, ManagedSubagentPage* page_out, std::string* error_out = nullptr, const std::string& run_id = {}, std::size_t offset = 0, std::size_t maximum_count = 50);
		static bool Cancel(AppState& app, const std::string& visible_root_chat_id, const std::string& run_id, const ManagedSubagentControlScope& scope, std::string* error_out = nullptr);
		/// No receipt means existing native/internal cancellation semantics, with no GUI admission checks.
		static bool ValidateCancellationOwnership(const AppState& app, const std::string& run_id, std::string* error_out = nullptr);
		static nlohmann::json ToJson(const ManagedSubagentPage& page);
	  private:
		static bool ValidateFollowupScopeImpl(const AppState&, const std::string&, const std::string&, const ManagedSubagentControlScope&, bool completion, std::string*);
	};
} // namespace uam
