#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/uuid.h"

// Inert authored-input provenance. Never delivery, replay, cancellation or ownership authority.
namespace uam::input_provenance
{
	inline bool ValidNativeInputId(std::string_view value)
	{
		return uam::uuid::IsCanonicalUuid(value);
	}

	inline bool ValidOriginRequestId(std::string_view value)
	{
		if (value.empty() || value.size() > 128) return false;
		for (const unsigned char byte : value)
			if (byte < 0x20 || byte > 0x7e) return false;
		return true;
	}
}

/// <summary>
/// Message role marker persisted with each chat message entry.
/// </summary>
enum class MessageRole
{
	User,
	Assistant,
	System
};

/// <summary>One addressable native child with provider-reported metadata.</summary>
struct SubAgentChild
{
	std::string id;
	std::string title;
	std::string status;
	std::optional<std::int64_t> started_at_epoch_ms;
	std::optional<std::int64_t> finished_at_epoch_ms;
	std::optional<std::int64_t> duration_ms;
	bool operator==(const SubAgentChild&) const = default;
};

/// <summary>
/// A single tool call execution record.
/// </summary>
struct ToolCall
{
	std::string id;
	std::string name;
	std::string args_json;
	std::string result_text;
	std::string status;
	bool is_sub_agent = false;
	std::string sub_agent_id;
	std::string sub_agent_title;
	std::string kind;
	std::string approval_status;
	std::string task_id;
	std::string child_activity;
	std::vector<SubAgentChild> sub_agent_children;
	// Native receiver metadata was normalized, including an empty or rejected list.
	bool sub_agent_children_authoritative = false;
	bool operator==(const ToolCall&) const = default;
};

struct MessagePlanEntry
{
	std::string content;
	std::string priority;
	std::string status;
	bool operator==(const MessagePlanEntry&) const = default;
};

struct MessageBlock
{
	std::string type;
	std::string text;
	std::string tool_call_id;
	std::string request_id_json;
		int64_t compaction_tokens_before = -1;
		int64_t compaction_tokens_after = -1;
	bool operator==(const MessageBlock&) const = default;
};

namespace uam::native_assets
{
	class InlinePayload;
}

// Source metadata never grants file, provider or preview authority.
enum class AttachmentSource
{
	LegacyPath,
	NativeInlinePending,
	OwnedNativeAsset,
	ProviderUri
};

struct MessageAttachment
{
	std::string id;
	std::string name;
	std::string kind;
	std::string mime_type;
	std::string path;
	std::uintmax_t size_bytes = 0;
	bool copied = false;
	std::string preview_key;
	std::string preview_origin_path;
	AttachmentSource source = AttachmentSource::LegacyPath;
	std::string source_uri;
	std::string origin_execution_host_id;
	std::string native_part_key;
	std::string native_tool_call_id;
	std::string native_source_field;
	std::string native_asset_key;
	std::string content_digest;
	// Runtime-only shared immutable original; repository preparation must consume it.
	std::shared_ptr<const uam::native_assets::InlinePayload> native_inline_payload;
	bool operator==(const MessageAttachment& other) const
	{
		// A validated digest represents original bytes. Allocation/pointer identity
		// and encoded bytes are deliberately absent from semantic comparisons.
		return std::tie(id, name, kind, mime_type, path, size_bytes, copied, preview_key, preview_origin_path, source, source_uri,
		           origin_execution_host_id, native_part_key, native_tool_call_id, native_source_field, native_asset_key, content_digest) ==
		    std::tie(other.id, other.name, other.kind, other.mime_type, other.path, other.size_bytes, other.copied, other.preview_key,
		        other.preview_origin_path, other.source, other.source_uri, other.origin_execution_host_id, other.native_part_key,
		        other.native_tool_call_id, other.native_source_field, other.native_asset_key, other.content_digest);
	}
};

namespace uam
{
	enum class ManagedRunTurnStatus
	{
		Queued, Running, Completed, Failed, Cancelled, Interrupted
	};

	enum class ManagedRunPrincipalKind
	{
		UserRoot, AgentRoot, AgentRun
	};

	enum class ManagedRunContinuity
	{
		Native
	};

	enum class ManagedRunRecoveryDecision
	{
		None, ContinueWithoutReplay
	};

	struct ManagedRunPrincipal
	{
		ManagedRunPrincipalKind kind = ManagedRunPrincipalKind::UserRoot;
		std::string root_chat_id;
		std::string root_birth_id;
		// Both empty for UserRoot/AgentRoot; both required for AgentRun.
		std::string parent_run_id;
		std::string parent_transcript_birth_id;
		bool operator==(const ManagedRunPrincipal&) const = default;
	};

	struct ManagedRunOwnerBinding
	{
		std::string root_birth_id;
		std::string transcript_birth_id;
		// Null for user-created delegation; only original agent_delegate may create it.
		std::optional<ManagedRunPrincipal> delegation_grant;
		bool operator==(const ManagedRunOwnerBinding&) const = default;
	};

	struct ManagedRunExecutionBinding
	{
		// Canonical exact JSON projections, not hashes or renderer-supplied values.
		std::string connection_configuration_json;
		std::string provider_configuration_json;
		std::string execution_host_id;
		std::string workspace_directory;
		std::string workspace_worktree_directory;
		std::string workspace_source_directory;
		std::string routing_mode;
		std::string routing_source_folder_id;
		std::string routing_source_directory;
		std::string effective_model_id;
		// Empty until adapter-owned evidence exists; exact-requested or advertised-resolution.
		std::string model_identity_source;
		std::string reasoning_effort;
		std::string service_tier;
		bool service_tier_explicit = false;
		std::string approval_mode;
		std::string command_safety_tier;
		std::string native_session_id;
		bool operator==(const ManagedRunExecutionBinding&) const = default;
	};

	struct ManagedRunTurnReceipt
	{
		// Initial entry: turn_id == run.id, number == 1, request/predecessor/text empty.
		std::string turn_id;
		std::uint32_t turn_number = 0;
		std::string client_request_id;
		std::string after_turn_id;
		ManagedRunPrincipal principal;
		std::string text;
		ManagedRunContinuity continuity = ManagedRunContinuity::Native;
		ManagedRunRecoveryDecision recovery_decision = ManagedRunRecoveryDecision::None;
		std::string retry_of_turn_id;
		ManagedRunTurnStatus status = ManagedRunTurnStatus::Queued;
		std::string delivery_id;
		bool dispatch_attempted = false;
		int expected_turn_serial = 0;
		int first_user_message_index = -1;
		std::string user_message_created_at;
		int64_t started_at_epoch_ms = 0;
		int64_t finished_at_epoch_ms = 0;
		bool result_required = false;
		bool result_delivered = false;
		int result_delivery_attempts = 0;
		bool audit_pending = false;
		std::string diagnostic_code;
		bool operator==(const ManagedRunTurnReceipt&) const = default;
	};

	struct ManagedRunContinuation
	{
		ManagedRunOwnerBinding owner;
		// May be absent during an initial turn; absence never enables follow-up.
		std::optional<ManagedRunExecutionBinding> execution;
		std::string current_turn_id;
		std::uint32_t current_turn_number = 0;
		std::uint64_t state_revision = 0;
		// Initial entry plus at most 32 follow-ups, in turn-number order.
		std::vector<ManagedRunTurnReceipt> turns;
		bool operator==(const ManagedRunContinuation&) const = default;
	};

	struct ManagedRunPromptIdentity
	{
		std::string run_id;
		std::string turn_id;
		std::uint32_t turn_number = 0;
		std::string expected_native_session_id;
		ManagedRunContinuity continuity = ManagedRunContinuity::Native;
		bool operator==(const ManagedRunPromptIdentity&) const = default;
	};

	/// Runtime-only immutable metadata for a passive transcript; never dispatched as queued input.
	struct PassiveQueuedPromptArchive
	{
		std::string owner_chat_id;
		std::string canonical_prompts_json;
	};

	/// <summary>
	/// A bounded follow-up prompt persisted until the ACP runtime accepts it for delivery.
	/// </summary>
	struct AcpQueuedUserPromptState
	{
		std::string text;
		std::string uam_agent_id = "build";
		std::string uam_agent_definition_hash;
		std::string uam_agent_definition_snapshot;
		std::string uam_agent_instructions;
		std::vector<std::string> uam_agent_skills;
		std::vector<std::string> uam_agent_delegates;
		std::string uam_agent_workspace_access = "write";
		std::string uam_agent_execution_capability = "uam-prompt-injected";
		std::vector<std::string> markdown_store_files;
		std::vector<std::string> markdown_store_prompt_blocks;
		std::vector<MessageAttachment> attachments;
		bool append_user_message = true;
		/// <summary>Optional exact durable raw row reused by prepared agent input instead of appending it twice.</summary>
		int existing_user_message_index = -1;
		std::string existing_user_message_created_at;
		/// <summary>Expand branch context at dispatch while persisted text still identifies the raw row.</summary>
		bool include_prior_context = false;
		/// <summary>Direct input awaiting admission, distinct from a follow-up queued behind active work.</summary>
		bool direct_admission = false;
		/// <summary>Held because its pre-launch repository baseline failed; explicit receipt Retry re-runs that baseline.</summary>
		bool repository_baseline_hold = false;
		bool prepared_for_delivery = false;
		std::string claude_quota_continuation_id;
		int prepared_user_message_count = 0;
		bool goal_mode = false;
		std::string goal_id;
		bool computer_use_mode = false;
		bool priority_steer = false;
		std::string id;
		int revision = 1;
		std::optional<ManagedRunPromptIdentity> managed_turn;
		/// Optional per-input provenance, independent of queue id/revision; batches may omit it.
		std::optional<std::string> native_input_id;
		std::optional<std::string> origin_request_id;
		bool operator==(const AcpQueuedUserPromptState&) const = default;
	};

	/// Inert policy associated with one definitely unsent authored row; never dispatch authority.
	struct AcpUnsentPromptRecovery
	{
		int version = 1;
		std::string chat_id;
		std::string provider_id;
		int user_message_index = -1;
		std::string user_message_created_at;
		std::string row_witness;
		std::string admission_id;
		int admission_revision = 1;
		std::string uam_agent_id;
		std::string uam_agent_definition_hash;
		std::string uam_agent_definition_snapshot;
		std::string uam_agent_instructions;
		std::vector<std::string> uam_agent_skills;
		std::vector<std::string> uam_agent_delegates;
		std::string uam_agent_workspace_access;
		std::string uam_agent_execution_capability;
		bool operator==(const AcpUnsentPromptRecovery&) const = default;
	};

	/// <summary>
	/// A remote JSON-RPC interaction response retained until the output delivery
	/// containing its request is durably acknowledged.
	/// </summary>
	struct AcpRemoteInteractionResponseState
	{
		std::string request_id_json;
		std::string response_json;
	};

	/// <summary>
	/// Outgoing request correlation retained until the provider response is persisted.
	/// </summary>
	struct AcpRemotePendingRequestState
	{
		int request_id = 0;
		std::string method;
		std::string delivery_id;
		std::string payload;
		int user_message_index = -1;
		int turn_serial = 0;
		std::string provider_turn_id;
		bool response_consumed = false;
		bool operator==(const AcpRemotePendingRequestState&) const = default;
	};
}

/// <summary>
/// One persisted chat message payload.
/// </summary>
struct Message
{
	MessageRole role = MessageRole::User;
	std::string content;
	std::string created_at;
	std::string provider;
	int tokens_input = 0;
	int tokens_output = 0;
	double estimated_cost_usd = 0.0;
	int time_to_first_token_ms = 0;
	int processing_time_ms = 0;
	bool interrupted = false;
	std::string stop_reason;
	bool priority_steer = false;
	std::string checkpoint_sha;
	std::string checkpoint_parent_sha;
	std::vector<ToolCall> tool_calls;
	std::string thoughts;
	std::string plan_summary;
	std::vector<MessagePlanEntry> plan_entries;
	std::vector<MessageBlock> blocks;
	std::vector<std::string> markdown_store_files;
	std::vector<std::string> markdown_store_prompt_blocks;
	std::vector<MessageAttachment> attachments;
	/// <summary>Runtime model used for this response; absent for legacy or unknown history.</summary>
	std::string model_id;
	bool continues_turn = false;
	bool acp_prompt_not_sent = false;
	bool acp_prompt_discarded = false;
	/// Native event receipts are saved with their projected message before remote output is acknowledged.
	std::vector<std::string> provider_event_ids;
	/// Output-only authored-row correlation; never execution or control authority.
	std::optional<std::string> managed_origin_run_id;
	/// Inert provenance only; copies retain history, never current action authority.
	std::optional<std::string> native_input_id;
	/// Exact original local stdio entry completed at the OS sink; not provider processing or input authority.
	bool input_write_completed = false;
	std::optional<std::string> origin_request_id;
	bool operator==(const Message&) const = default;
};

/// <summary>
/// Goal status for a thread-level goal.
/// </summary>
enum class GoalStatus
{
	Active,
	Complete,
	Blocked,
	Paused
};

/// <summary>Persists the first objective row; Attempted denotes transport ambiguity rather than provider acceptance.</summary>
struct NativeGoalInitialDelivery
{
	enum class State { Pending, Attempted, Accepted };
	std::int64_t user_message_index = -1;
	std::string user_message_created_at;
	State state = State::Pending;
	std::string turn_id;
	bool operator==(const NativeGoalInitialDelivery&) const = default;
};

/// <summary>Receipt evidence for explicit native resume; creation time is not a unique provider ID or CAS token.</summary>
struct NativeGoalBinding
{
	int version = 1;
	std::string provider_id;
	std::string execution_host_id;
	std::string thread_id;
	std::string uam_chat_id;
	std::string uam_goal_id;
	std::string objective;
	std::int64_t created_at = 0;
	std::int64_t last_updated_at = 0;
	std::int64_t uam_tokens_at_creation = 0;
	std::int64_t uam_budget_at_creation = 0;
	std::optional<std::int64_t> native_token_budget;
	std::int64_t last_native_tokens_used = 0;
	std::optional<NativeGoalInitialDelivery> initial_delivery;
	bool operator==(const NativeGoalBinding&) const = default;
};

/// <summary>
/// A thread-level goal that persists the agent across turns until it
/// self-detects completion or gets stuck (>= 3 consecutive blocker turns).
/// Mirrors Codex CLI's /goal mode.
/// </summary>
struct Goal
{
	std::string id;
	std::string objective;                    // user-provided goal text
	GoalStatus status = GoalStatus::Active;
	int64_t token_budget = 0;                // 0 = unlimited
	int64_t tokens_used = 0;
	int blocked_turn_count = 0;              // consecutive turns at same blocker
	std::string last_blocker;                // description of current blocker
	std::string last_blocker_kind;           // machine-readable blocker category
	std::string last_diagnostic;             // durable goal-loop diagnostic reason
	std::vector<std::string> completed_items;
	std::vector<std::string> remaining_items;
	std::string current_step;
	std::string last_verification;
	std::string last_next_prompt;
	int same_next_prompt_count = 0;
	std::string last_assistant_text;             // trimmed text of the last worker turn
	int same_assistant_text_count = 0;           // consecutive identical worker outputs
	int loop_count = 0;
	std::string created_at;
	std::string updated_at;
	std::string execution_owner = "uam";
	std::string provider_command;
	std::optional<NativeGoalBinding> native_binding;
	std::string worker_model_id;
	std::string reviewer_model_id;
	std::string creator = "user";
	std::string creator_provider_id;
	std::string creator_agent_id;
	std::string creator_run_id;
	std::string creator_request_key_hash;
};

struct UamControlAuditRecord
{
	std::string request_id;
	std::string method;
	std::string result;
	std::string reason;
	std::string provider_id;
	std::string agent_id;
	std::string run_id;
	std::string created_at;
};

struct AgentRun
{
	/// Inert v2 persistence; startup excludes these records until execution integration exists.
	std::optional<uam::ManagedRunContinuation> managed_continuation;
	std::string id;
	std::string root_chat_id;
	std::string parent_run_id;
	std::string resumed_from_run_id;
	std::string transcript_chat_id;
	std::string goal_id;
	std::string agent_id;
	std::string definition_hash;
	std::string definition_snapshot;
	std::string definition_instructions;
	std::vector<std::string> skills_snapshot;
	std::vector<std::string> delegates_snapshot;
	std::string provider_id;
	std::string model_id;
	std::string execution_capability;
	std::string task;
	std::string effective_workspace_access;
	std::string status;
	int depth = 1;
	int expected_turn_serial = 0;
	int64_t started_at_epoch_ms = 0;
	/// Zero on older ledgers; otherwise captured at the terminal write, never parsed from local time.
	int64_t finished_at_epoch_ms = 0;
	int64_t deadline_at_epoch_ms = 0;
	std::string created_at;
	std::string started_at;
	std::string finished_at;
	std::string updated_at;
	std::string result_delivery_id;
	bool deliver_result_to_root_chat = false;
	bool root_result_delivered = false;
	int root_result_delivery_attempts = 0;
	std::string result_excerpt;
	std::string diagnostic_code;
	std::string diagnostic;
};

/// <summary>
/// Chat session metadata and message history.
/// </summary>
struct ProviderHandoffCliContext
{
	std::string execution_host_id;
	std::string directory;
	std::string connection_identity;
	bool operator==(const ProviderHandoffCliContext&) const = default;
};

/// <summary>One durable native-session continuation; claimed delivery is never replayed after restart.</summary>
struct ClaudeQuotaContinuation
{
	std::string id;
	std::string state;
	std::string chat_id;
	std::string native_session_id;
	std::string context;
	std::string accepted_message_digest;
	std::string agent_id;
	std::string agent_definition_hash;
	std::string agent_workspace_access;
	std::string agent_execution_capability;
	int64_t observed_at_epoch_s = 0;
	int64_t resets_at_epoch_s = 0;
	int user_message_index = -1;
	std::size_t message_count = 0;
	bool operator==(const ClaudeQuotaContinuation&) const = default;
};

/// <summary>Portable custom icon metadata. PNG values name assets under the data root.</summary>
struct CustomIcon
{
	std::string type;
	std::string value;
	bool operator==(const CustomIcon&) const = default;
};

/// Process-local evidence for an already-owned terminal transcript's normal retirement.
/// It permits result reads only; persistence/catalogue loaders never mint this witness.
struct ManagedTerminalRetirementWitness
{
	struct RunIdentity
	{
		std::string id, root_id, parent_id, transcript_id, provider_id, model_id, definition_hash, created_at;
		std::string agent_id, execution_capability, resumed_from_id, result_delivery_id, workspace_access, definition_snapshot;
		static RunIdentity Capture(const AgentRun& run)
		{
			return {run.id, run.root_chat_id, run.parent_run_id, run.transcript_chat_id, run.provider_id,
			    run.model_id, run.definition_hash, run.created_at, run.agent_id, run.execution_capability,
			    run.resumed_from_run_id, run.result_delivery_id, run.effective_workspace_access, run.definition_snapshot};
		}
		bool operator==(const RunIdentity&) const = default;
	};
	std::shared_ptr<const std::uint8_t> original_birth;
	std::filesystem::path data_root;
	RunIdentity run;
	std::string provider_id, host_id, workspace, worktree, native_session_id, created_at;
};

/// Durable DATA only. Loading this record never reconstructs continuation authority.
struct ManagedParentResultRecord
{
	int version = 1;
	std::string id, stage = "collecting", reason;
	std::string root_id, provider_id, host_id, workspace, native_thread, policy_digest;
	std::string origin_turn, origin_user_digest, payload;
	std::vector<std::string> run_ids, delivered_ids, batch_ids;
	std::string native_request_id, accepted_turn_id;
	bool operator==(const ManagedParentResultRecord&) const = default;
};

/// Append-only canonical parent DATA for one actual internal input; never a runtime execution lease.
struct ManagedParentInputReceipt
{
	int version = 1;
	std::string id, origin_id, root_id, provider_id, host_id, workspace, native_thread;
	std::string origin_turn, payload, native_request_id, accepted_turn_id;
	std::vector<std::string> run_ids;
	bool operator==(const ManagedParentInputReceipt&) const = default;
};

struct ChatSession
{
	/// <summary>Runtime identity retained by copies; a recreated chat receives a fresh tag.</summary>
	std::shared_ptr<const std::uint8_t> metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	/// Runtime-only immutable terminal-retirement proof, distinct from inspection metadata birth.
	std::shared_ptr<const ManagedTerminalRetirementWitness> managed_terminal_retirement_witness;
	/// Ephemeral UI refresh signal only; never persisted or used as execution authority.
	std::uint64_t managed_subagent_revision = 0;
	/// Durable birth witness; empty on legacy/unwitnessed data and never minted by a codec.
	std::string managed_followup_incarnation;
	/// Runtime-only fail-closed marker; preserve raw managed input until its consumer exists.
	bool managed_followup_input_unavailable = false;
	std::string id;
	std::string execution_host_id = "local";
	std::string routing_mode = "explicit";
	std::string routing_source_folder_id;
	std::string routing_source_directory;
	std::string routing_assigned_connection_identity;
	std::string routing_assigned_provider_configuration_identity;
	bool routing_reservation_pending = false;
	std::vector<std::string> routing_inherited_default_fields;
	std::string provider_id;
	std::string native_session_id;
	/// Inert V2 origin/refusal hint, bound to that exact native ID; never resume or input authority.
	std::optional<std::string> opencode_v2_session_id;
	std::optional<ManagedParentResultRecord> managed_parent_result;
	std::vector<ManagedParentInputReceipt> managed_parent_input_receipts;
	/// Codec presence only; an absent legacy key is distinct from a present empty/invalid collection.
	bool managed_parent_input_receipts_present = false;
	ClaudeQuotaContinuation claude_quota_continuation;
	bool native_session_reset_pending = false;
	// New Claude chats may assign an owned CLI ID on first local or remote launch.
	// Keep the persisted legacy field name; older unbound chats may already have history.
	bool remote_claude_session_unstarted = false;
	// Persisted only while a remote structured turn is active. A GUI restart uses
	// this to reattach to the existing runner process without replaying the prompt.
	bool remote_turn_reconnect_pending = false;
	// Conservatively remains true from remote proxy launch until helper process
	// removal is confirmed. This is independent of whether a turn is active.
	bool remote_process_exists = false;
	bool remote_prompt_image_supported = false;
	std::string remote_structured_protocol;
	int remote_opencode_native_port = 0;
	std::string remote_opencode_native_token;
	bool remote_manual_compaction_pending = false;
	std::string remote_manual_compaction_boundary_json;
	// Persisted until a previously requested idle remote stop is confirmed.
	bool remote_stop_cleanup_pending = false;
	// Persisted from a stop-then-restart request until the replacement prompt is
	// durably delivered or the abandoned restart is cleaned up after relaunch.
	bool remote_restart_pending = false;
	bool remote_recovery_enabled = false;
	std::string remote_recovery_state;
	// Capability required to reattach to or control the helper-owned process.
	std::string remote_process_control_token;
	std::uintmax_t remote_delivered_stdout_cursor = 0;
	std::uintmax_t remote_delivered_stderr_cursor = 0;
	bool remote_source_exit_pending = false;
	int remote_source_exit_code = -1;
	// Stable for the lifetime of a remote provider session so a replacement GUI
	// bridge can recreate the local side of the provider's existing MCP channel.
	std::string remote_uam_control_channel_id;
	// Responses are retained until the output batch containing their requests is
	// durably acknowledged. One batch can contain multiple interaction requests.
	std::vector<uam::AcpRemoteInteractionResponseState> remote_interaction_responses;
	// Protocol-3 input delivery state. The transport owns the wire envelope and
	// acknowledgement; runtime persists the exact payload and stable delivery id.
	std::string remote_prompt_delivery_session_id;
	std::string remote_prompt_delivery_id;
	std::string remote_prompt_delivery_payload;
	std::vector<uam::AcpRemotePendingRequestState> remote_pending_requests;
	int remote_next_request_id = 1;
	std::string remote_active_turn_id;
	int remote_turn_serial = 0;
	int remote_turn_user_message_index = -1;
	std::vector<uam::AcpQueuedUserPromptState> acp_queued_prompts;
	/// Absent is legacy unknown; a version-zero record means malformed-present.
	std::vector<uam::AcpUnsentPromptRecovery> acp_unsent_prompt_recoveries;
	// Null is unknown, distinct from a validated empty archive; shared copies stay immutable.
	std::shared_ptr<const uam::PassiveQueuedPromptArchive> passive_queued_prompt_archive;
	std::string acp_queue_storage_revision;
	// Successful persistence refreshes this stamp; copied snapshots keep their earlier stamp.
	mutable std::string acp_queue_full_save_revision;
	bool acp_queue_storage_error = false;
	bool acp_queue_held = false;
	std::size_t acp_dispatched_queued_prompt_count = 0;
	std::string temporary_parent_chat_id;
	/// <summary>Storage incarnation for temporary children, independent of provider identity.</summary>
	std::string temporary_storage_owner_token;
	bool side_cleanup_requested = false;
	double side_cleanup_retry_time_s = 0.0;
	std::shared_ptr<std::atomic<bool>> side_cleanup_stop_finished;
	std::string parent_chat_id;
	std::string branch_root_chat_id;
	/// Display-only native-child navigation; never branch, resume, permission or execution authority.
	std::string subagent_parent_chat_id;
	/// Exact creation timestamp of the originating UAM parent, preventing ID-reuse attachment.
	std::string subagent_parent_created_at;
	int branch_from_message_index = -1;
	bool branch_message_edited = false;
	std::string branch_operation_id;
	std::string branch_retry_error;
	std::string folder_id;
	std::string title;
	CustomIcon custom_icon;
	std::string created_at;
	std::string updated_at;
	std::string last_opened_at;
	/// Nonempty until an explicit interaction acknowledges this exact update.
	std::string attention_revision;
	/// Set when the user marks the chat done in Active chats; newer activity reopens it.
	std::string settled_at;
	/// Snooze affects sidebar attention only; the provider and queued inputs remain owned.
	std::int64_t snoozed_until_ms = 0;
	std::string last_stop_reason;
	/// Changes only for explicit goal commands, invalidating older completions.
	std::string goal_command_revision;
	std::string goal_pending_continuation_id;
	/// Persisted recency of user input and runtime state events, excluding stream traffic.
	/// Empty for imported history until its first UAM interaction.
	std::string interaction_at;
	bool pinned = false;
	std::vector<std::string> linked_files;
	std::vector<Message> messages;
	bool messages_loaded = true;
	std::size_t persisted_message_count = 0;
	std::string persisted_messages_digest;
	std::string workspace_directory;
	std::string workspace_isolation_kind;
	std::string workspace_source_directory;
	std::string workspace_base_ref;
	/// <summary>Absent inherits Settings; empty explicitly starts at HEAD without switching source.</summary>
	std::optional<std::string> git_base_branch_override;
	std::string workspace_branch_name;
	std::string workspace_worktree_directory;
	bool svn_update_pending = false;
	bool imported_read_only = false;
	std::string approval_mode;
	std::string uam_agent_id = "build";
	// Provider and definition identity last dispatched as prompt context.
	std::string last_prompt_agent_definition_hash;
	/// <summary>Transcript snapshot carried to a new provider, bound after prompt delivery.</summary>
	std::string provider_handoff_context;
	std::string provider_handoff_session_id;
	/// <summary>Owned native context locations retained until chat deletion succeeds.</summary>
	std::vector<ProviderHandoffCliContext> provider_handoff_cli_contexts;
	std::string agent_run_id;
	// Fresh, bounded transcript owned by a goal on another visible chat.
	// Empty on ordinary chats and on all legacy data.
	std::string goal_owner_chat_id;
	std::string goal_iteration_goal_id;
	std::string goal_iteration_turn_kind;
	int goal_iteration_repair_attempts = 0;
	bool uam_control_enabled = false;
	bool uam_control_explicitly_disabled = false;
	std::string command_safety_tier = "off";
	// Enablement and the selected OS target are intentionally runtime-only.
	// Only the backend preference is persisted with the chat.
	/// <summary>Launch-only policy snapshot; global consent is persisted in AppSettings.</summary>
	bool computer_use_globally_enabled = true;
	bool computer_use_enabled = false;
	std::string computer_use_backend = "auto";
	std::string computer_use_target_kind = "window";
	std::string computer_use_target_id;
	std::string computer_use_target_process_id;
	std::string computer_use_target_title;
	std::string computer_use_target_input_mode;
	std::string model_id;
	std::string reviewer_model_id;
	std::string reasoning_effort;
	std::string service_tier;
	bool service_tier_explicit = false;
	std::string extra_flags;
	bool memory_enabled = true;
	int memory_last_processed_message_count = 0;
	std::string memory_last_processed_at;
	std::vector<Goal> goals;
	std::string active_goal_id;
	std::vector<UamControlAuditRecord> uam_control_audit;
	std::string memory_level = "strict";
	bool small_model_mode = false;
};

struct MemoryWorkerBinding
{
	std::string worker_provider_id;
	std::string worker_model_id;
};

struct EditorFileAssociation
{
	std::string id;
	std::string name;
	std::vector<std::string> extensions;
	std::string editor_preset_id;
};

struct ProviderChatDefaults
{
	std::string model_id;
	std::string approval_mode = "default";
	std::string command_safety_tier = "off";
	bool memory_enabled = true;
	std::string reasoning_effort;
	std::string service_tier;
	std::string memory_level = "strict";
	bool small_model_mode = false;
	std::string reviewer_model_id;
	std::string feature_preference = "uam";
	std::vector<std::string> hidden_model_ids;
	std::vector<std::string> hidden_provider_ids;
};

/// <summary>
/// User-defined chat folder metadata.
/// </summary>
struct ChatFolder
{
	std::string id;
	std::string title;
	std::string directory;
	bool collapsed = false;
	std::string execution_host_id;
	CustomIcon custom_icon;
	// Transient worker probe; never persisted with workspace metadata.
	std::string availability_directory;
	std::string availability_host_id;
	bool directory_missing = false;
};

struct ResourceReference
{
	std::string id;
	std::string type;
	std::string target;
	std::string label;
	CustomIcon custom_icon;
};

struct ResourceCollection
{
	std::string id;
	std::string name;
	bool collapsed = false;
	std::vector<ResourceReference> references;
	CustomIcon custom_icon;
};

struct ShellAction
{
	std::string id;
	std::string label;
	std::string skill_path;
	std::string provider_id;
	std::string model_id;
	std::vector<std::string> group_path;
	bool accepts_files = true;
	bool accepts_folders = true;
	bool enabled = true;
	bool open_workspace = false;
};

struct McpSecretReference
{
	std::string name;
	std::string environment_variable;
	std::string value; // Literal value stored in settings; used when environment_variable is empty.
};

struct McpServerConfiguration
{
	std::string id;
	std::string execution_host_id;
	std::string name;
	std::string workspace_directory;
	std::string transport = "stdio";
	std::string command;
	std::vector<std::string> args;
	std::string url;
	std::vector<McpSecretReference> environment;
	std::vector<McpSecretReference> headers;
	bool enabled = true;
};

struct ExecutionRoutingProviderConfirmation
{
	std::string provider_id;
	std::string connection_identity;
	std::string provider_configuration_identity;
};

struct ExecutionRoutingWorkspaceMapping
{
	std::string source_folder_id;
	std::string source_directory;
	std::string destination_folder_id;
};

struct ExecutionRoutingHostPolicy
{
	std::string host_id;
	bool enabled = false;
	std::size_t max_active_turns = 0;
	std::vector<ExecutionRoutingProviderConfirmation> provider_confirmations;
	std::vector<ExecutionRoutingWorkspaceMapping> workspace_mappings;
};

struct ExecutionRoutingSettings
{
	bool enabled = false;
	std::vector<ExecutionRoutingHostPolicy> hosts;
};

struct ExecutionHost
{
	std::string id = "local";
	std::string label = "This computer";
	std::string transport = "local";
	std::string ssh_alias;
	std::string runner_status = "ready";
	std::string runner_version;
	std::string platform;
	std::string architecture;
	std::string last_seen_at;
	std::string runner_directory;
	int runner_protocol_version = 0;
	std::string instruction_file;
	bool startup_enabled = false;
	std::string startup_status = "disabled";
	CustomIcon custom_icon;
	bool cli_updates_as_admin = false;
	/// Absent uses the legacy remote file; an empty value explicitly disables host instructions.
	std::optional<std::string> instruction_text;
	std::optional<std::string> runner_startup_applied;
	std::string runner_startup_error;
	bool operator==(const ExecutionHost&) const = default;
};

struct ComputerUseApplicationRule
{
	std::string identity_kind;
	std::string identity;
	bool operator==(const ComputerUseApplicationRule&) const = default;
};

/// <summary>A provider and host's override of a shared resource default.</summary>
struct ProviderMcpOverride
{
	std::string provider_id;
	std::string execution_host_id;
	std::string server_id;
	bool enabled = true;
};

/// <summary>Resources UAM installs privately for configured provider launches.</summary>
struct CentralProviderConfiguration
{
	bool enabled = false;
	std::string instructions;
	std::vector<std::string> instruction_files;
	std::vector<std::string> skill_directories;
	std::string default_agent_id = "build";
	bool uam_control_enabled = true;
	std::vector<ProviderMcpOverride> mcp_overrides;
	std::vector<std::string> default_skills;
	std::vector<std::string> default_agents;
};

/// <summary>A provider's independent runtime availability on one configured execution host.</summary>
struct ProviderAvailabilityOverride
{
	std::string provider_id;
	std::string execution_host_id;
	bool enabled = true;
};

enum class GitPortMethod { Patch, Merge, Squash };

struct HostScopedSettings
{
	std::optional<std::string> default_new_chat_provider_id;
	std::map<std::string, ProviderChatDefaults> provider_chat_defaults;
	std::optional<std::vector<McpServerConfiguration>> mcp_servers;
};

struct AppSettings
{
	std::string voice_input_mode = "system";
	std::string voice_input_server_base_url = "https://api.openai.com";
	std::string voice_input_server_endpoint = "/v1/audio/transcriptions";
	std::string voice_input_server_model = "whisper-1";
	std::string voice_input_api_key_env = "OPENAI_API_KEY";

	bool computer_use_globally_enabled = true;
	bool computer_use_allowlist_enabled = false;
	std::vector<ComputerUseApplicationRule> computer_use_allowed_applications;
	std::string active_provider_id = provider_build_config::FirstEnabledProviderId();
	std::string provider_extra_flags;
	int cli_idle_timeout_seconds = 600;
	int active_turn_inactivity_timeout_seconds = 1800;
	std::string ui_theme = "focus";
	bool show_provider_icons_in_sidebar = true;
	bool show_worktree_path_in_sidebar = true;
	bool git_enabled = true;
	bool git_auto_worktree = true;
	bool git_snapshot_svn = true;
	bool git_snapshot_plain_folders = true;
	bool git_pull_before_chat = false;
	GitPortMethod git_port_method = GitPortMethod::Patch;
	std::string git_default_base_branch;
	bool worktree_cleanup_enabled = true;
	int worktree_retention_days = 14;
	int backup_log_retention_days = 14;
	int http_cache_clear_interval_days = 0;
	bool svn_enabled = true;
	bool svn_update_before_chat = false;
	bool confirm_delete_chat = true;
	bool confirm_delete_folder = true;
	bool remember_last_chat = true;
	std::string last_selected_chat_id;
	float ui_scale_multiplier = 1.0f;
	float sidebar_width = 280.0f;
	int window_width = 1440;
	int window_height = 860;
	bool window_maximized = false;
	bool memory_enabled_default = true;
	int memory_idle_delay_seconds = 60;
	int memory_recall_budget_bytes = 2048;
	int goal_max_loop_iterations = 200;
	int acp_setup_inactivity_timeout_seconds = 600;
	int acp_turn_output_limit_mib = 1024;
	bool update_checks_enabled = true;
	bool automatic_provider_updates = false;
	std::string update_last_checked_at;
	std::map<std::string, std::string> dismissed_update_versions;
	std::map<std::string, MemoryWorkerBinding> memory_worker_bindings;
	std::string permission_reviewer_provider_id;
	std::string permission_reviewer_model_id;
	std::string default_new_chat_provider_id = provider_build_config::FirstEnabledProviderId();
	std::map<std::string, ProviderChatDefaults> provider_chat_defaults;
	std::map<std::string, HostScopedSettings> host_scoped_settings;
	std::vector<ProviderAvailabilityOverride> provider_availability;
	std::string markdown_store_directory;
	std::string default_editor_preset_id = "vscode";
	std::string file_explorer_application;
	int editor_default_groups_version = 0;
	std::vector<EditorFileAssociation> editor_file_associations;
	CentralProviderConfiguration central_provider_configuration;
	std::vector<McpServerConfiguration> mcp_servers;
	std::vector<ExecutionHost> execution_hosts;
	ExecutionRoutingSettings execution_routing;
	std::vector<std::string> favorite_uam_agent_ids;
	std::string uam_agent_cycle_shortcut = "shift+tab";
	std::string memory_level_default = "strict";
};

/// <summary>
/// In-flight provider command state used by async polling.
/// </summary>
struct ProcessExecutionResult
{
	bool ok = false;
	bool timed_out = false;
	bool canceled = false;
	bool output_truncated = false;
	int exit_code = -1;
	std::string output;
	std::string error;
};

struct AsyncProcessTaskState
{
	std::atomic<bool> completed{false};
	ProcessExecutionResult result;
	std::optional<bool> remote_helper_connected;
	std::chrono::steady_clock::time_point launch_time;
	std::string provider_id;
	int64_t estimated_input_tokens = 0;
};

/// <summary>
/// Converts a message role enum into persisted text.
/// </summary>
std::string RoleToString(MessageRole role);
/// <summary>
/// Parses persisted message role text into an enum value.
/// </summary>
MessageRole RoleFromString(std::string_view value);
/// <summary>
/// Converts goal status enum into persisted text.
/// </summary>
std::string GoalStatusToString(GoalStatus status);
/// <summary>
/// Parses persisted goal status text into an enum value.
/// </summary>
GoalStatus GoalStatusFromString(std::string_view value);
