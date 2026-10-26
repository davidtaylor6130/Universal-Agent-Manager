#pragma once

#include "app/provider_model_catalog_service.h"
#include "app/execution_routing_service.h"
#include "app/repository_route_match_service.h"
#include "common/models/app_models.h"
#include "common/paths/workspace_lease.h"
#include "common/config/frontend_actions.h"
#include "common/platform/platform_state_fields.h"
#include "common/platform/observed_process_stop.h"
#include "common/provider/provider_profile.h"
#include "common/provider/attached_skill_resources.h"
#include "common/runtime/terminal/terminal_dimensions.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IProviderRuntime;
struct PreparedChatSave;
struct PreparedMemoryOutput;

namespace uam
{
	namespace claude_preinput { class Observation; }
	namespace acp_detail { struct ColdRetryBinding; }

	class StartupNativeImportService;
	class StorageRetentionService;
	class DataFolderSettingsService;
	class OpenCodeNativeActivityService;
	class ManagedSubagentCancellationReceipt;
	struct ManagedInitialHistoryCatalogue;
	struct OpenCodeNativeCompanion;
	struct OpenCodeTerminalActivityObserver;
	struct WorktreeLaunchPreparationTask;
	struct TerminalNativeMediaCoordinator;
	struct CliTerminalHistoryGeneration
	{
		std::atomic<bool> revoked{false};
		std::atomic<bool> stopped{false};
	};
	enum class OpenCodeCompactionGateState;

	namespace fs = std::filesystem;
	struct ChatBaselinePreparation;

	/// <summary>
	/// Runtime state for one embedded provider CLI terminal instance.
	/// PTY output is forwarded to xterm.js in the React frontend via
	/// uam::PushCliOutput().
	/// </summary>
	enum class CliTerminalTurnState
	{
		Unknown,
		Idle,
		Busy,
	};

	enum class CliTerminalLifecycleState
	{
		Disabled,
		Stopped,
		Unknown,
		Idle,
		Busy,
		ShuttingDown,
	};

	struct CliContextPreparation
	{
		std::atomic<bool> finished{false};
		std::stop_source cancellation;
		std::string baseline;
		std::string chat_id;
		std::vector<std::string> argv;
		std::vector<std::pair<std::string, std::string>> environment;
		std::filesystem::path provider_configuration_directory;
		std::string channel;
		bool history_preparation = false;
		std::string prepared_context;
		std::string storage_revision;
		std::vector<Message> baseline_messages;
		std::shared_ptr<PreparedChatSave> prepared_chat_save;
		std::string error;
		bool succeeded = false;
	};

	struct CliContextPreparationTask
	{
		std::shared_ptr<CliContextPreparation> state;
		std::unique_ptr<std::jthread> worker;
	};

	struct CodexActivityStringCursor
	{
		bool discard_checkpoint_tail = false;
		bool inside_string = false;
		bool escaped = false;
		bool compacting = false;
		std::string token;
		int token_expected_bytes = 0;
	};

	struct PendingManagedInitialMention;
	struct PendingManagedFollowup;
	struct ManagedFollowupCompletedOwnerWitness;
	struct ManagedInitialRuntimeOwner;
	struct AcpSessionState;
	struct PendingAcpPromptDelivery;
	struct LocalInputWriteObservation;
	struct ManagedParentContinuationLease;
	struct PendingAcpCompaction;
	struct PendingLocalAcpLaunch;
	struct PendingLocalSessionReceipt;
	struct PendingAgentInputPreparation;
	struct PendingQueuedHistoryPreparation;
	struct ClaudeQuotaHistoryTask;
	struct CliTerminalState : public platform::CliTerminalPlatformFields
	{
		std::shared_ptr<OpenCodeTerminalActivityObserver> opencode_activity_observer;
		std::shared_ptr<AcpSessionState> uam_control_session;
		std::unique_ptr<std::jthread> uam_control_relay;
		std::string terminal_id;
		// A final history receipt survives handle close/row erasure, never a new launch.
		std::shared_ptr<CliTerminalHistoryGeneration> native_history_generation = std::make_shared<CliTerminalHistoryGeneration>();
		std::string frontend_chat_id;
		bool running = false;
		bool repository_baseline_pending = false;
		std::shared_ptr<std::stop_source> repository_baseline_cancel;
		std::string attached_chat_id;
		std::string attached_session_id;
		std::vector<std::string> session_ids_before;
		bool native_session_discovery_ambiguous = false;
		bool native_identity_requires_owned_reply = false;
		std::string native_identity_command;
		std::string native_identity_output;
		std::string native_identity_deferred_input;
		int native_identity_query_phase = 0;
		double native_identity_query_time_s = 0.0;
		std::vector<std::string> linked_files_snapshot;
		int rows = kCliTerminalDefaultRows;
		int cols = kCliTerminalDefaultCols;
		bool should_launch = false;
		bool worktree_launch_pending = false;
		std::shared_ptr<std::stop_source> worktree_launch_cancellation;
		std::shared_ptr<CliContextPreparation> context_preparation;
		bool ui_attached = false;
		std::string ui_attachment_id;
		std::shared_ptr<std::stop_source> native_session_setup_cancel;
		double last_sync_time_s = 0.0;
		double last_output_time_s = 0.0;
		double last_activity_time_s = 0.0;
		double last_user_input_time_s = 0.0;
		double last_ai_output_time_s = 0.0;
		double last_polled_time_s = 0.0;
		double last_idle_confirmed_time_s = 0.0;
		double idle_interaction_started_time_s = 0.0;
		double last_busy_time_s = 0.0;
		double shutdown_requested_time_s = 0.0;
		double inactivity_interrupt_requested_time_s = 0.0;
		bool input_ready = false;
		double startup_time_s = 0.0;
		bool generation_in_progress = false;
		bool uses_prompt_activity_tracking = true;
		CliTerminalTurnState turn_state = CliTerminalTurnState::Idle;
		CliTerminalLifecycleState lifecycle_state = CliTerminalLifecycleState::Stopped;
		std::string recent_output_bytes;
		std::uint64_t output_cursor = 0;
		std::string current_turn_output_bytes;
		bool prompt_settle_required = false;
		double prompt_settle_candidate_time_s = 0.0;
		// Transient cursor for the verified native Codex rollout, never persisted.
		std::string native_activity_provider_id;
		std::string codex_activity_session_id;
		std::filesystem::path codex_activity_cwd;
		std::filesystem::path codex_activity_rollout;
		std::uintmax_t codex_activity_offset = 0;
		std::string codex_activity_partial_line;
		CodexActivityStringCursor codex_activity_string_cursor;
		std::string codex_activity_turn_id;
		bool codex_activity_discard_line = false;
		bool codex_activity_awaiting_turn = false;
		bool codex_activity_read_failed = false;
		bool codex_activity_completed = false;
		std::string last_native_history_snapshot_digest;
		std::string pending_steer_prompt;
		double pending_steer_started_time_s = 0.0;
		bool pending_steer_restart_attempted = false;
		std::string last_error;
	};

	struct AcpToolCallState
	{
		std::string id;
		std::string title;
		std::string kind;
		std::string status;
		std::string content;
		std::string args_json;
		std::string permission_review_decision;
		std::string permission_review_reason;
		bool is_sub_agent = false;
		std::string sub_agent_id;
		std::string sub_agent_title;
		std::string approval_status;
		std::string task_id;
		std::string child_activity;
		std::vector<SubAgentChild> sub_agent_children;
		bool sub_agent_children_authoritative = false;
	};

	struct AcpPlanEntryState
	{
		std::string content;
		std::string priority;
		std::string status;
	};

	struct AcpModeState
	{
		std::string id;
		std::string name;
		std::string description;
		std::string provider_id;
	};

	struct AcpConfigOptionChoiceState
	{
		std::string value;
		std::string name;
		std::string description;
	};

	struct AcpConfigOptionState
	{
		std::string id;
		std::string name;
		std::string description;
		std::string category;
		std::string current_value;
		std::vector<AcpConfigOptionChoiceState> choices;
	};

	struct AcpCommandState
	{
		std::string name;
		std::string description;
		std::string input_hint;
	};

	struct AcpModelState
	{
		std::string id;
		std::string name;
		std::string description;
		std::string default_reasoning_effort;
		std::vector<std::string> supported_reasoning_efforts;
		std::vector<std::string> additional_speed_tiers;
		std::optional<std::int64_t> context_window_tokens;
		std::optional<std::int64_t> max_output_tokens;
	};

	struct AcpTurnEventState
	{
		std::string type;
		std::string text;
		std::string tool_call_id;
		std::string request_id_json;
		int64_t compaction_tokens_before = -1;
		int64_t compaction_tokens_after = -1;
	};

	struct AcpReplayUpdateState
	{
		int message_index = -1;
		int block_index = -1;
		std::string session_update;
		std::string text;
		std::string tool_call_id;
		std::string title;
	};

	struct AcpTokenUsageBreakdownState
	{
		std::int64_t input_tokens = 0;
		std::int64_t cached_input_tokens = 0;
		std::int64_t cache_write_input_tokens = 0;
		std::int64_t output_tokens = 0;
		std::int64_t reasoning_output_tokens = 0;
		std::int64_t total_tokens = 0;
	};

	struct AcpTokenUsageState
	{
		bool available = false;
		bool last_available = true;
		std::string session_id;
		std::int64_t updated_at_ms = 0;
		AcpTokenUsageBreakdownState total;
		AcpTokenUsageBreakdownState last;
		std::int64_t model_context_window = 0;
		// Runtime-only goal baseline from correlated cumulative receipts, separate from display totals.
		std::optional<std::int64_t> goal_cumulative_total;
		std::string goal_cumulative_session_id;
	};

	struct AcpContextUsageState
	{
		bool available = false;
		std::int64_t updated_at_ms = 0;
		std::int64_t used_tokens = 0;
		std::int64_t window_tokens = 0;
	};

	struct AcpRateLimitWindowState
	{
		bool available = false;
		int used_percent = 0;
		std::int64_t resets_at = 0;
		std::int64_t window_duration_minutes = 0;
		// Internal observation time for independently reported windows; zero retains legacy snapshot timing.
		std::int64_t updated_at_ms = 0;
	};

	struct AcpCreditsState
	{
		bool available = false;
		bool has_credits = false;
		bool unlimited = false;
		std::string balance;
	};

	struct AcpSpendControlLimitState
	{
		bool available = false;
		std::string limit;
		std::string used;
		int remaining_percent = 0;
		std::int64_t resets_at = 0;
	};

	struct AcpRateLimitsState
	{
		bool available = false;
		std::int64_t updated_at_ms = 0;
		std::string limit_id;
		std::string limit_name;
		AcpRateLimitWindowState primary;
		AcpRateLimitWindowState secondary;
		AcpCreditsState credits;
		AcpSpendControlLimitState individual_limit;
		bool spend_control_reached_available = false;
		bool spend_control_reached = false;
		std::string plan_type;
		std::string rate_limit_reached_type;
	};

	struct AcpProviderUsageState
	{
		AcpTokenUsageState token_usage;
		AcpContextUsageState context_usage;
		AcpRateLimitsState rate_limits;
	};

	struct AcpDiagnosticEntryState
	{
		std::string time;
		std::string event;
		std::string reason;
		std::string method;
		std::string request_id;
		bool has_code = false;
		int code = 0;
		std::string message;
		std::string detail;
		std::string lifecycle_state;
	};

	struct AcpPermissionOptionState
	{
		std::string id;
		std::string name;
		std::string kind;
	};

	struct AcpPendingPermissionState
	{
		std::string request_id_json;
		std::string provider_request_method;
		std::string provider_request_kind;
		std::string codex_approval_payload_json;
		std::string provider_input_json;
		std::string provider_request_params_json;
		std::string tool_call_id;
		std::string title;
		std::string kind;
		std::string status;
		std::string content;
		std::string safety_risk;
		std::string safety_tier;
		bool safety_requires_approval = false;
		bool version_controlled_workspace = false;
		std::vector<AcpPermissionOptionState> options;
		// Typed native command only; display content and reason text cannot prove execution authority.
		std::string execution_command;
	};

	struct AcpUserInputOptionState
	{
		std::string label;
		std::string description;
	};

	struct AcpUserInputQuestionState
	{
		std::string id;
		std::string header;
		std::string question;
		bool is_other = false;
		bool is_multiple = false;
		bool is_secret = false;
		std::vector<AcpUserInputOptionState> options;
	};

	struct AcpPendingUserInputState
	{
		std::string request_id_json;
		std::string provider_input_json;
		std::string item_id;
		std::string status;
		std::string attention_kind;
		std::vector<AcpUserInputQuestionState> questions;
	};

	struct PendingModelDiscoveryRetry
	{
		std::string chat_id;
		std::string provider_id;
		std::string workspace_directory;
		std::string execution_host_id = "local";
	};

	struct UamAgentWait;

	struct UamControlCapability
	{
		struct PendingApproval
		{
			std::string request_id;
			std::string method;
			std::string reason;
			std::string objective;
			std::string idempotency_key;
			std::string question;
			std::vector<std::string> question_options;
			std::string computer_use_task;
			std::string computer_use_origin;
			std::shared_ptr<const std::uint8_t> computer_use_chat_incarnation;
			std::shared_ptr<const std::uint8_t> computer_use_root_incarnation;
			int64_t expires_at_epoch_ms = 0;
		};

		std::string id;
		std::filesystem::path directory;
		std::filesystem::path provider_configuration_directory;
		bool central_skills_enabled = false;
		std::string chat_id;
		std::string session_chat_id;
		bool mcp_initialized = false;
		std::string mcp_connection_id;
		int64_t mcp_initialized_at_epoch_ms = 0;
		int64_t mcp_heartbeat_at_epoch_ms = 0;
		bool session_control_override_at_launch = false;
		bool root_control_override_at_launch = false;
		std::string provider_id;
		std::string agent_id;
		std::string agent_run_id;
		std::vector<std::string> agent_skills;
		std::vector<std::string> agent_delegates;
		int64_t expires_at_epoch_ms = 0;
		std::deque<int64_t> request_times_epoch_ms;
		std::unordered_set<std::string> seen_request_ids;
		std::unordered_set<std::string> owned_agent_run_ids;
		// Current-connection cancellation tombstones; overflow refuses new waits until renewal.
		std::unordered_set<std::string> cancelled_wait_ids;
		bool wait_cancellation_overflow = false;
		std::optional<PendingApproval> pending_approval;
		std::shared_ptr<UamAgentWait> pending_agent_wait;
		// Captured by real setup; wait admission cannot adopt a changed host/workspace selection.
		std::optional<ExecutionHost> wait_host_at_launch;
		std::shared_ptr<const std::uint8_t> wait_root_incarnation_at_launch, wait_chat_incarnation_at_launch;
		std::string wait_workspace_at_launch, wait_worktree_at_launch;
		std::string wait_root_workspace_at_launch, wait_root_worktree_at_launch;
	};

	struct AcpPendingSteerState
	{
		int user_message_index = -1;
		int turn_serial = 0;
		std::string provider_turn_id;
	};

	/// <summary>Intent survives asynchronous remote shutdown through final settlement.</summary>
	enum class AcpStopPurpose
	{
		Interrupt,
		ProviderUpdate,
		WorkspaceChange,
		Timeout
	};

	struct MemoryRecallIdentity
	{
		std::filesystem::path data_root;
		std::string workspace;
		std::string chat_id;
		std::string provider_id;
		std::string session_id;
		std::string prompt;
		std::string query;
		std::size_t position = 0;
		int turn_serial = 0;
		int budget = 0;
		std::string memory_level;
		bool small_model_mode = false;
		bool operator==(const MemoryRecallIdentity&) const = default;
	};
	struct MemoryRecallPreparation
	{
		MemoryRecallIdentity identity;
		std::string context;
		std::string error;
		std::stop_source cancel;
		std::atomic<bool> finished{false};
	};
	struct MemoryRecallPreparationLease
	{
		std::shared_ptr<MemoryRecallPreparation> state;
		explicit MemoryRecallPreparationLease(std::shared_ptr<MemoryRecallPreparation> value) : state(std::move(value)) {}
		~MemoryRecallPreparationLease() { if (state != nullptr) state->cancel.request_stop(); }
	};
	struct MemoryRecallTask
	{
		std::shared_ptr<MemoryRecallPreparation> state;
		std::unique_ptr<std::jthread> worker;
	};

	struct RemoteContextPreparation
	{
		std::filesystem::path data_root;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::string ownership;
		std::vector<attached_skills::Span> skill_spans;
		std::string wire_prompt;
		bool skills_prepared = false;
		std::string memory_level = "strict";
		ExecutionHost host;
		std::string workspace;
		std::string prompt;
		int memory_budget = 0;
		std::string context;
		std::string error;
		std::stop_source cancel;
		std::atomic<bool> finished{false};
	};
	struct RemoteContextTask
	{
		std::shared_ptr<RemoteContextPreparation> state;
		std::unique_ptr<std::jthread> worker;
	};

	/// Captured only by explicit Retry on an already bound remote process.
	/// A replacement process requires a fresh invocation, never scope promotion.
	struct SavedImageRetryRemoteScope
	{
		ExecutionHost host;
		std::string native_session_id, runtime_session_id, process_id, proxy_token, configuration;
		int proxy_protocol = 0;
	};

	/// Invocation-only permission from an explicit saved-row Retry; never serialized or reconstructed.
	struct SavedImageRetryPermission
	{
		std::filesystem::path root;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::string chat_id, provider_id, execution_host_id, chat_created_at;
		int row_index = -1;
		std::string row_created_at, content;
		std::vector<MessageAttachment> attachments;
		std::string queued_id;
		int queued_revision = 0;
		std::optional<SavedImageRetryRemoteScope> remote_scope;
	};

	struct ClaudeInputIdentity
	{
		std::string chat_id;
		std::string provider_id;
		std::string chat_provider_id;
		std::string session_id;
		std::string process_host_id;
		std::string execution_host_id;
		std::string workspace;
		std::string prompt;
		std::string model_id;
		std::string goal_model_id;
		std::string approval_mode;
		std::string computer_use_backend;
		bool computer_use_enabled = false;
		int turn_serial = 0;
		int first_message_index = -1;
		int last_message_index = -1;
		std::vector<MessageAttachment> images;
		std::filesystem::path original_root;
		std::shared_ptr<const std::uint8_t> original_incarnation;
		std::shared_ptr<const SavedImageRetryPermission> saved_image_retry;
		nlohmann::json native_metadata;
		std::string remote_retry_ownership;
		bool operator==(const ClaudeInputIdentity&) const = default;
	};
	struct ClaudeInputPreparation
	{
		ClaudeInputIdentity identity;
		ExecutionHost host;
		nlohmann::json message;
		// Owner-thread seal is distinct from the worker's validated image coverage.
		nlohmann::json sealed_message;
		std::string sealed_method;
		int sealed_request_id = 0;
		std::string error;
		std::stop_source cancel;
		std::atomic<bool> finished{false};
	};
	/// Session lifetime requests cancellation; app ownership keeps the worker off GUI destruction paths.
	struct ClaudeInputPreparationLease
	{
		std::shared_ptr<ClaudeInputPreparation> state;
		explicit ClaudeInputPreparationLease(std::shared_ptr<ClaudeInputPreparation> preparation) : state(std::move(preparation))
		{
		}
		ClaudeInputPreparationLease(const ClaudeInputPreparationLease&) = delete;
		ClaudeInputPreparationLease& operator=(const ClaudeInputPreparationLease&) = delete;
		~ClaudeInputPreparationLease()
		{
			if (state != nullptr)
				state->cancel.request_stop();
		}
	};
	struct ClaudeInputPreparationTask
	{
		std::shared_ptr<ClaudeInputPreparation> state;
		std::unique_ptr<std::jthread> worker;
	};

	struct AcpSessionState : public platform::StdioProcessPlatformFields
	{
		std::shared_ptr<ManagedInitialRuntimeOwner> managed_initial_owner;
		std::shared_ptr<const char> permission_review_incarnation = std::make_shared<const char>(0);
		bool worktree_launch_pending = false;
		std::shared_ptr<std::stop_source> worktree_launch_cancellation;
		AcpStopPurpose stop_purpose = AcpStopPurpose::Interrupt;
		platform::ProcessStopOutcome stop_outcome = platform::ProcessStopOutcome::Unknown;
		bool local_stop_pending = false;
		std::string goal_command_revision;
		std::string interaction_at;
		std::string interaction_wait_request_id;
		std::string chat_id;
		std::string provider_id;
		std::string process_execution_host_id;
		/// Volatile shutdown projection fence captured at runtime creation, never native/input authority.
		std::shared_ptr<const std::uint8_t> shutdown_chat_incarnation;
		std::filesystem::path shutdown_data_root;
		/// Catalog ownership captured when this provider process is launched; never persisted.
		std::string model_catalog_scope_key;
		std::string protocol_kind = uam::provider_profile_constants::kProtocolGeminiAcp;
		bool prompt_image_supported = false;
		std::shared_ptr<OpenCodeNativeCompanion> opencode_native_companion;
		bool opencode_v2_native = false; // owned local process only; never persisted
		// Optional guard captured from the actual process adapter, never mutable chat/provider selection.
		const IProviderRuntime* native_pre_dispatch_runtime = nullptr;
		// Ephemeral native query DATA; never serialized or an input permission.
		std::shared_ptr<claude_preinput::Observation> claude_preinput_observation;
		// Invocation-only opt-in provenance; passive loading and current Retry never create it.
		std::shared_ptr<acp_detail::ColdRetryBinding> cold_retry_binding;
		bool claude_manual_compaction_supported = false;
		bool claude_manual_compaction_boundary_seen = false;
		nlohmann::json copilot_native_state = nlohmann::json::object();
		std::string session_id;
		std::string codex_thread_id;
		std::string codex_turn_id;
		/// Runtime-only exact original prompt RPC witness; notifications never create it.
		std::string codex_origin_rpc_turn_id;
		int codex_origin_rpc_turn_serial = 0;
		/// Native child identities observed from this process's current parent, never restored from saved tools.
		std::unordered_set<std::string> codex_known_child_thread_ids;
		std::unordered_set<std::string> codex_live_background_thread_ids;
        /// Antigravity reports per-step usage separately from cumulative conversation results.
        int64_t antigravity_input_tokens = 0;
        int64_t antigravity_output_tokens = 0;
        bool antigravity_turn_usage_complete = true;
		bool antigravity_turn_has_usage = false;
		int64_t antigravity_completed_turns = 0;
		std::unordered_set<int64_t> antigravity_completed_steps;
		struct AntigravityGoalTurn
		{
			std::string owner_chat_id;
			std::string goal_id;
			std::string revision;
			std::string execution_host_id;
			std::string native_conversation_id;
			std::filesystem::path data_root;
			std::shared_ptr<const std::uint8_t> owner_incarnation;
			int turn_serial = -1;
			int64_t accounted_tokens = 0;
			std::optional<int64_t> result_tokens;
			bool settled = false;
		};
		AntigravityGoalTurn antigravity_goal_turn;
		std::string lifecycle_state = "stopped";
		/// Volatile blocked-transport latch; last_error retains the original failure.
		std::string transport_ownership_error;
		bool running = false;
		bool initialized = false;
		bool session_ready = false;
		bool model_discovery_only = false;
		bool ephemeral_model_discovery = false;
		bool load_session_supported = false;
		bool resume_session_supported = false;
		bool mcp_http_supported = false;
		bool mcp_sse_supported = false;
		bool processing = false;
		std::shared_ptr<PendingAcpCompaction> pending_compaction;
		bool manual_compaction_pending = false;
		bool context_compaction_in_progress = false;
		bool recovering_remote_turn = false;
		bool recovering_remote_process = false;
		bool waiting_for_permission = false;
		bool waiting_for_user_input = false;
		bool cancel_requested = false;
		double cancel_requested_time_s = 0.0;
		bool inactivity_timeout_pending = false;
		bool turn_checkpoint_eligible = false;
		bool turn_checkpoint_preflight_pending = false;
		bool turn_checkpoint_commit_pending = false;
		int next_request_id = 1;
		int initialize_request_id = 0;
		int session_setup_request_id = 0;
		int startup_model_request_id = 0;
		int reasoning_change_request_id = 0;
		int config_option_change_request_id = 0;
		int mode_change_request_id = 0;
		int model_change_request_id = 0;
		bool awaiting_model_config_options = false;
		int prompt_request_id = 0;
		std::string claude_quota_delivery_id;
		bool claude_quota_usage_error = false;
		int claude_quota_evidence_turn_serial = 0;
		int64_t claude_quota_reset_epoch_s = 0;
		int64_t claude_quota_observed_epoch_s = 0;
		int cancel_request_id = 0;
		int current_assistant_message_index = -1;
		int turn_first_user_message_index = -1;
		int turn_user_message_index = -1;
		int turn_assistant_message_index = -1;
		int turn_serial = 0;
		std::vector<AcpUnsentPromptRecovery> delivered_prompt_recoveries;
		std::string active_uam_agent_id = "build";
		std::string active_uam_agent_definition_hash;
		std::string active_uam_agent_definition_snapshot;
		std::vector<std::string> active_uam_agent_skills;
		std::vector<std::string> active_uam_agent_delegates;
		std::string active_uam_agent_workspace_access = "write";
		std::string active_uam_agent_instructions;
		std::string active_uam_agent_execution_capability = "uam-prompt-injected";
		std::filesystem::path active_uam_agent_adapter_directory;
		std::string managed_agent_run_id;
		std::unique_ptr<std::jthread> uam_control_relay;
		std::filesystem::path provider_configuration_directory;
		std::string launch_model_options_id;
		bool launch_model_options_captured = false; // Runtime-only: empty captured Default differs from an unknown reattached launch.
		std::string provider_protocol_instructions;
		std::string uam_control_capability_id;
		bool managed_launch_attempted = false;
		bool managed_cancellation_pending = false;
		bool remote_stop_pending = false;
		bool remote_stop_unconfirmed = false;
		bool restart_marker_save_pending = false;
		bool restart_after_remote_stop_cleanup = false;
		int last_settled_turn_serial = 0;
		std::string last_turn_outcome;
		std::string last_turn_error;
		double turn_started_time_s = 0.0;
		bool turn_computer_use_mode = false;
		std::string queued_prompt;
		std::vector<attached_skills::Span> queued_skill_spans;
		std::shared_ptr<RemoteContextPreparation> attached_skill_preparation;
		bool defer_prompt_save = false;
		std::shared_ptr<PendingAcpPromptDelivery> pending_prompt_delivery;
		std::shared_ptr<ManagedParentContinuationLease> managed_parent_continuation;
		std::shared_ptr<PendingLocalAcpLaunch> pending_local_launch;
		std::shared_ptr<PendingLocalSessionReceipt> pending_local_session_receipt;
		std::shared_ptr<PendingAgentInputPreparation> pending_agent_input;
		std::shared_ptr<PendingQueuedHistoryPreparation> pending_queued_history;
		std::string pending_referenced_queue_launch_fingerprint;
		std::deque<AcpQueuedUserPromptState> queued_user_prompts;
		bool queue_held = false;
		// Definite queued write rollback is not dispatchable until its complete history is saved.
		// Survives runtime reset; a committed full save clears it, never a queue-only journal.
		bool queued_rollback_save_pending = false;
		std::shared_ptr<RemoteContextPreparation> remote_context_preparation;
		std::optional<std::size_t> memory_recall_position;
		std::string memory_recall_query;
		std::unique_ptr<MemoryRecallPreparationLease> memory_recall_preparation;
		std::unique_ptr<ClaudeInputPreparationLease> claude_input_preparation;
		std::shared_ptr<const SavedImageRetryPermission> saved_image_retry;
		int saved_image_retry_turn_serial = -1;
		// Counts automatic relaunches after the process died before the queued
		// prompt was delivered. Deliberately survives ResetAcpRuntimeState so a
		// crash-looping provider cannot restart forever; cleared when a new user
		// prompt is sent or a turn completes.
		int crash_restart_attempts = 0;
		bool reconnect_pending = false;
		int reconnect_attempts = 0;
		double reconnect_not_before_time_s = 0.0;
		// Counts watchdog re-queues of a stalled goal loop; the goal is marked
		// blocked after 3 so a failing provider cannot be retried forever.
		// Cleared when a turn completes or a new user prompt is sent.
		int goal_auto_resume_attempts = 0;
		// Set when the user cancels a turn so the goal-loop watchdog does not
		// override an explicit stop; cleared on the next user prompt.
		enum class GoalQueueAdmission { None, Pending, Held, PendingContinuation, HeldContinuation };
		GoalQueueAdmission goal_queue_admission = GoalQueueAdmission::None;
		int goal_queue_admission_turn_serial = 0;
		std::string goal_queue_admission_revision;
		bool goal_resume_suppressed = false;
		std::string goal_turn_kind;
		std::string goal_turn_model_id;
		bool goal_internal_session = false;
		bool goal_review_turn = false;
		bool goal_review_scheduled = false;
		std::string goal_review_goal_id;
		std::string goal_review_user_prompt;
		std::string goal_review_assistant_text;
		int goal_review_repair_attempts = 0;
		// Runtime receipt from validated provider usage, never reconstructed from review text.
		struct GoalReviewUsage
		{
			int turn_serial = -1;
			std::optional<std::int64_t> tokens;
			std::optional<std::int64_t> cumulative_baseline;
			std::string session_id;
			bool fresh_session = false;
			bool consumed = false;
		} goal_review_usage;
		bool ignore_session_updates_until_ready = false;
		bool codex_resume_fallback_attempted = false;
		bool acp_resume_fallback_attempted = false;
		std::string stdout_buffer;
		std::string stderr_buffer;
		std::string remote_output_delivery_token;
		int remote_runner_protocol_version = 0;
		std::string pending_remote_input_receipt_id;
		std::string pending_remote_output_ack_line;
		std::uintmax_t pending_remote_stdout_cursor = 0;
		std::uintmax_t pending_remote_stderr_cursor = 0;
		std::uintmax_t remote_consumed_stderr_cursor = 0;
		std::string pending_remote_source_exit_ack_line;
		bool remote_source_exit_reported = false;
		int remote_source_exit_code = -1;
		bool stdout_poll_pending = false;
		bool stderr_poll_pending = false;
		std::string recent_stderr;
		std::string last_error;
		bool has_last_exit_code = false;
		int last_exit_code = 0;
		bool process_exit_observed = false;
		int observed_process_exit_code = -1;
		std::string last_process_id;
		std::vector<std::string> assistant_replay_prefixes;
		std::vector<AcpReplayUpdateState> load_history_replay_updates;
		int load_history_replay_end_message_index = -1;
		std::vector<AcpDiagnosticEntryState> diagnostics;
		std::string pending_assistant_thoughts;
		std::string agent_name;
		std::string agent_title;
		std::string agent_version;
		std::unordered_map<int, std::string> pending_request_methods;
		std::unordered_map<std::string, AcpPendingSteerState> pending_steer_requests;
		std::unordered_map<int, std::function<void()>> pending_local_steer_abandon;
		std::unordered_map<std::string, int> tool_call_message_indices;
		struct OpenCodeChildActivity
		{
			std::string native_session_id;
			std::string status;
			std::string parent_session_id;
			std::string process_id;
		};
		/// Current-process protocol receipts only; saved tool history cannot create live activity.
		std::unordered_map<std::string, OpenCodeChildActivity> opencode_child_activity_by_tool;
		std::vector<AcpToolCallState> tool_calls;
		std::unordered_set<std::string> claude_seen_child_messages;
		/// <summary>Native parent envelope identities prevent repeated delivery from appending twice.</summary>
		std::unordered_set<std::string> claude_seen_parent_messages;
		/// <summary>Observed tasks owned by this native process, retained across parent turns but never persisted.</summary>
		std::unordered_set<std::string> claude_live_background_task_ids;
		std::vector<AcpPlanEntryState> plan_entries;
		std::string plan_summary;
		std::unordered_map<std::string, std::string> codex_agent_message_text_by_item_id;
		std::string codex_last_agent_message_item_id;
		std::unordered_set<std::string> codex_streamed_reasoning_keys;
		/// <summary>Effective reviewer confirmed by this native thread, never inferred from requested settings.</summary>
		std::string codex_approvals_reviewer;
		/// <summary>Native goal support is confirmed by this process, never inferred from slash commands.</summary>
		enum class CodexGoalCapability { Unknown, StateApi, Unsupported };
		CodexGoalCapability codex_goal_capability = CodexGoalCapability::Unknown;
		/// <summary>Volatile receipt ownership binds native execution to the persisted UAM intent.</summary>
		struct CodexGoalOwner
		{
			enum class Action { Probe, ReplaceClear, PreparePaused, InitialTurn, Activate, StopPause, StopClear };
			std::string chat_id;
			std::string thread_id;
			std::string goal_id;
			std::string revision;
			std::string objective;
			std::int64_t token_budget = 0;
			std::int64_t tokens_used_at_start = 0;
			bool user_origin = false;
			bool start_authorized = false;
			bool replace_native = false;
			bool resume_existing = false;
			std::int64_t binding_created_at = 0;
			Action action = Action::Probe;
			std::string execution_host_id = "local";
			std::string process_id;
			/// <summary>Volatile parent identity; persisted IDs cannot authorize a recreated chat.</summary>
			std::shared_ptr<const std::uint8_t> parent_incarnation;
			enum class InitialHistoryState { Unprepared, Pending, Committed };
			InitialHistoryState initial_history_state = InitialHistoryState::Unprepared;
			int initial_history_user_message_index = -1;
			std::string initial_history_user_message_created_at;
			std::shared_ptr<const std::uint8_t> initial_history_incarnation;
			std::string delivery_configuration;
		};
		CodexGoalOwner codex_goal_owner;
		std::unordered_map<std::string, CodexGoalOwner> codex_goal_pending_requests;
		std::optional<CodexGoalOwner> codex_goal_queued_user_action;
		std::optional<CodexGoalOwner> codex_goal_initial_turn_pending;
		bool codex_goal_native_active = false;
		bool codex_goal_auto_continuation_observed = false;
		std::string codex_goal_initial_turn_id;
		int codex_goal_initial_turn_request_id = 0;
		std::int64_t codex_goal_created_at = 0;
		std::int64_t codex_goal_updated_at = 0;
		std::string codex_last_reasoning_section;
		std::vector<AcpCommandState> available_commands;
		std::vector<AcpModeState> available_modes;
		std::string current_mode_id;
		std::string mode_change_previous_id;
		std::optional<std::string> mode_change_previous_chat_id;
		std::optional<std::string> mode_change_previous_command_safety_tier;
		std::string mode_change_requested_id;
		// Keep paginated discovery separate from the last complete model catalog.
		struct CodexModelDiscoveryState
		{
			std::vector<AcpModelState> models;
			std::unordered_set<std::string> model_ids;
			std::unordered_set<std::string> cursors;
			std::string current_model_id;
		} codex_model_discovery;
		std::vector<AcpModelState> available_models;
		std::vector<AcpConfigOptionState> available_config_options;
		std::string current_model_id;
		AcpProviderUsageState provider_usage;
		std::string pending_startup_model_id;
		std::string reasoning_change_previous_id;
		std::optional<std::string> reasoning_change_previous_chat_id;
		std::string reasoning_change_requested_id;
		std::string config_option_change_id;
		std::string config_option_change_previous_value;
		std::string config_option_change_requested_value;
		std::string model_change_previous_id;
		std::optional<std::string> model_change_previous_chat_id;
		std::string model_change_requested_id;
		std::vector<AcpTurnEventState> turn_events;
		std::uint64_t turn_protocol_bytes = 0;
		bool turn_output_warning_emitted = false;
		std::array<std::uint64_t, 60> turn_output_bytes_per_second{};
		std::int64_t turn_output_latest_second = -1;
		AcpPendingPermissionState pending_permission;
		std::deque<AcpPendingPermissionState> queued_permissions;
		AcpPendingUserInputState pending_user_input;
		double wait_started_time_s = 0.0;
		double last_runtime_activity_time_s = 0.0;
		double idle_interaction_started_time_s = 0.0;
		bool wait_is_stale = false;
		std::string wait_stale_reason;
	};

	/// Mutation fence for a captured stop result; never process ownership or replay permission.
	struct AcpStopProjectionOwner
	{
		std::filesystem::path data_root;
		std::shared_ptr<const std::uint8_t> chat_incarnation;
		std::shared_ptr<const char> runtime_incarnation;
		const AcpSessionState* runtime = nullptr; // Comparison only, never dereferenced.
	};

	struct PendingAcpRemoteStop : public platform::StdioProcessPlatformFields
	{
		std::optional<AcpStopProjectionOwner> projection_owner;
		std::string chat_id;
		std::string stderr_tail;
		double deadline_time_s = 0.0;
		bool recoverable_turn = false;
		bool restart_after_stop = false;
	};

	struct AsyncAcpProcessStopTask
	{
		std::optional<AcpStopProjectionOwner> projection_owner;
		// Initial-v2 completion must not fall back to a replacement chat ID/serial.
		std::shared_ptr<ManagedInitialRuntimeOwner> managed_initial_owner;
		std::string chat_id;
		std::string provider_id;
		std::string execution_host_id = "local";
		AcpStopPurpose purpose = AcpStopPurpose::Interrupt;
		int turn_serial = 0;
		std::shared_ptr<platform::ObservedProcessStopResult> result;
		std::shared_ptr<platform::StdioProcessPlatformFields> owned_process;
		bool result_consumed = false;
		std::string native_writer_key;
		bool observe_exit = false;
		bool preserve_queued_prompts = false;
		// Optional owned platform dependency; retained through asynchronous stop and exit polling.
		std::shared_ptr<IPlatformProcessService> stop_service;
		std::shared_ptr<std::atomic<bool>> finished;
		std::unique_ptr<std::jthread> worker;
	};

	/// <summary>
	/// Background command execution container for provider CLI version checks.
	/// </summary>
	struct AsyncCommandTask
	{
		bool running = false;
		bool allow_interactive_elevation = false;
		ExecutionHost execution_host;
		std::string command_preview;
		std::shared_ptr<AsyncProcessTaskState> state;
		std::unique_ptr<std::jthread> worker;
	};

	inline void ResetAsyncCommandTask(AsyncCommandTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}

		task.running = false;
		task.allow_interactive_elevation = false;
		task.command_preview.clear();
		task.state.reset();
	}

	struct AsyncMemoryExtractionState
	{
		std::atomic<bool> remote_publication_ready{false};
		std::mutex remote_publication_mutex;
		std::condition_variable_any remote_publication_condition;
		bool remote_publication_permitted = false;
		int hydrated_message_count = -1;
		std::string hydrated_messages_digest;
		bool low_signal_skip = false;
		bool hydration_failed = false;
		bool output_prepared = false;
		std::shared_ptr<PreparedMemoryOutput> prepared_output;
		std::string output_error;
		bool remote_applied = false;
		int remote_entry_count = 0;
	};

	struct AsyncMemoryExtractionTask
	{
		bool running = false;
		std::filesystem::path data_root;
		std::string chat_id;
		std::string command_preview;
		int message_count = 0;
		bool prepared_from_cold = false;
		int source_persisted_message_count = 0;
		std::string source_persisted_messages_digest;
		int scan_start_message_index = -1;
		std::filesystem::path workspace_root;
		std::optional<ExecutionHost> remote_host;
		std::string remote_workspace;
		std::string owner_provider_id;
		std::string memory_level;
		std::shared_ptr<AsyncProcessTaskState> state;
		std::shared_ptr<AsyncMemoryExtractionState> extraction_state;
		std::unique_ptr<std::jthread> worker;
	};

	struct AsyncPermissionReviewTask
	{
		std::shared_ptr<const char> permission_review_incarnation;
		std::string chat_id;
		std::string request_id_json;
		std::string command_preview;
		std::string requested_model_id;
		std::shared_ptr<AsyncProcessTaskState> state;
		std::unique_ptr<std::jthread> worker;
	};

	enum class AsyncTurnCheckpointTaskKind
	{
		Preflight,
		Commit,
	};

	struct AsyncTurnCheckpointState
	{
		std::atomic<bool> finished{false};
		bool eligible = false;
		bool ok = false;
		bool changed = false;
		std::string checkpoint_sha;
		std::string parent_sha;
		std::string message;
	};

	struct AsyncTurnCheckpointTask
	{
		AsyncTurnCheckpointTaskKind kind = AsyncTurnCheckpointTaskKind::Preflight;
		std::string chat_id;
		int turn_serial = 0;
		int assistant_message_index = -1;
		std::string expected_message_created_at;
		std::string completed_goal_turn_kind;
		bool completed_review_turn = false;
		bool cancelled = false;
		std::string goal_id;
		std::shared_ptr<AsyncTurnCheckpointState> state;
		std::unique_ptr<std::jthread> worker;
	};

	inline void StopAsyncPermissionReviewTask(AsyncPermissionReviewTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}
	}

	inline void StopAsyncMemoryExtractionWorker(AsyncMemoryExtractionTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}

		task.running = false;
	}

	struct QueuedMemoryExtractionTask
	{
		std::string chat_id;
		int scan_start_message_index = -1;
		bool manual = false;
	};

	struct MemoryActivityState
	{
		int entry_count = 0;
		std::string last_created_at;
		int last_created_count = 0;
		int running_count = 0;
		std::string last_status;
		std::string last_worker_chat_id;
		std::string last_worker_provider_id;
		std::string last_worker_updated_at;
		std::string last_worker_status;
		std::string last_worker_output;
		std::string last_worker_error;
		bool last_worker_timed_out = false;
		bool last_worker_canceled = false;
		bool last_worker_has_exit_code = false;
		int last_worker_exit_code = 0;
	};

	struct CliProviderVersionState
	{
		std::string provider_id;
		ExecutionHost execution_host;
		bool checked = false;
		bool supported = false;
		std::string installed_version;
		std::string selected_version;
		std::string install_method = "npm";
		std::string install_command;
		std::string last_install_status = "none";
		std::string raw_output;
		std::string message;
		std::string check_error;
		std::string install_output;
		bool verify_failed_install = false;
		// Numeric requested update floor; Homebrew still selects its current latest package.
		std::string requested_minimum_version;
	};

	struct PendingCliUpdate
	{
		std::string provider_id;
		std::string version;
		ExecutionHost execution_host;
		double deadline = 0;
	};

	struct PendingGoalIterationState
	{
		std::string owner_chat_id;
		std::string goal_id;
		std::string prompt;
		std::string turn_kind;
		int repair_attempts = 0;
	};

	struct ComputerUseHistoryEntry
	{
		std::string time;
		std::string action;
		std::string status;
		std::string detail;
	};

	struct ComputerUseRuntimeState
	{
		std::string state = "stopped";
		std::string target_kind;
		std::string target_id;
		std::string target_process_id;
		std::string target_title;
		std::string target_input_mode;
		std::vector<ComputerUseHistoryEntry> history;
		std::string fingerprint;
		std::string source_signature;
	};

	/// <summary>Identity of one cold chat action, owned until its UI completion.</summary>
	struct BranchPreparationOwnership
	{
		std::string existing_branch_id;
		std::stop_source cancellation;
	};

	/// <summary>Shared application state for the CEF/React provider CLI release slice.</summary>
	struct PendingChatSaveTask;
	struct PendingAcpCompletedPromptRecoveries;
	struct MemoryActivityRefreshTask;
	struct HiddenAgentChatOwnership
	{
		fs::path data_root;
		std::vector<ChatSession> chats;
	};

	struct DeletionProgress
	{
		std::size_t completed = 0;
		std::size_t total = 0;
		std::string current_label;
	};

	struct AppState
	{
		WorkspaceLeaseGate worktree_retention_gate;
		std::shared_ptr<StorageRetentionService> storage_retention_service;
		std::shared_ptr<DataFolderSettingsService> data_folder_settings;
		std::shared_ptr<ExecutionRoutingService> execution_routing_service;
		std::vector<RepositoryRouteWorkspace> repository_route_workspaces;
		std::filesystem::path repository_route_data_root;
		double repository_route_observed_at_s = -1.0;
		std::shared_ptr<RepositoryRouteTask> repository_route_refresh;
		std::vector<std::shared_ptr<RepositoryRouteTask>> repository_route_tasks;
		std::unordered_map<std::string, std::vector<RepositoryRouteWorkspace>> repository_route_assignments;
		std::shared_ptr<StartupNativeImportService> startup_native_import_service;
		// Weak reservations expire if the async callback is discarded during bridge shutdown.
		std::vector<std::shared_ptr<ClaudeQuotaHistoryTask>> claude_quota_history_tasks;
		std::string claude_quota_dispatch_id;
		std::unordered_map<std::string, std::weak_ptr<BranchPreparationOwnership>> branch_preparations;
		std::unordered_map<std::string, std::string> execution_routing_waits;
		std::unordered_map<std::string, double> execution_routing_retry_at_by_chat_id;
		std::unordered_set<std::string> execution_routing_reserved_chat_ids;
		std::unordered_set<std::string> execution_routing_restored_unresolved_chat_ids;
		double execution_routing_next_poll_s = 0.0;
		fs::path data_root;
		fs::path native_history_chats_dir;
		AppSettings settings;
		// AppState-lifetime publication ordering only; not durable CAS or cross-process authority.
		std::string settings_publication_scope;
		std::uint64_t settings_publication_revision = 0;
		bool memory_settings_uncertain = false;
		std::vector<ChatFolder> folders;
		std::vector<ResourceCollection> resource_collections;
		std::vector<ShellAction> shell_actions;
		std::string shell_action_notification;
		std::vector<ProviderProfile> provider_profiles;
		uam::FrontendActionMap frontend_actions;

		std::vector<ChatSession> chats;
		HiddenAgentChatOwnership hidden_agent_chat_ownership;
		/// Saved chats of providers this build excludes (see excluded_provider_data.h): never shown or changed, consulted only so
		/// import and deletion respect the native sessions and workspaces they own. Always empty in a build with every provider.
		std::vector<ChatSession> excluded_provider_chats;
		std::vector<AgentRun> agent_runs;
		// Read-only restart metadata, never an execution or capacity inventory.
		std::shared_ptr<const ManagedInitialHistoryCatalogue> managed_initial_history;
		// Real creation tasks protect both IDs until worker retirement and callback settlement.
		std::unordered_map<std::string, std::shared_ptr<PendingManagedInitialMention>> managed_initial_mentions;
		// Real opt-in follow-up preparation, never restored or inferred from saved catalogue data.
		std::unordered_map<std::string, std::shared_ptr<PendingManagedFollowup>> managed_followups;
		// Private volatile origin proof retained only from genuinely completed/reaped owners, never startup-loaded.
		std::unordered_map<std::string, std::shared_ptr<const ManagedFollowupCompletedOwnerWitness>> managed_followup_completed_owners;
		std::unordered_map<std::string, std::shared_ptr<ManagedInitialRuntimeOwner>> managed_initial_runtime_owners;
		bool managed_initial_shutdown = false;
		std::vector<UamControlCapability> uam_control_capabilities;
		std::deque<std::shared_ptr<UamAgentWait>> uam_wait_retirements;
		std::string uam_control_manager_id;
		std::deque<std::string> queued_agent_run_ids;
		std::size_t managed_parent_continuation_cursor = 0;
		/// <summary>Retains cancellation intent across failed ledger writes until polling can retry.</summary>
		std::unordered_set<std::string> pending_agent_run_cancellation_ids;
		/// GUI-only ephemeral admission; native/internal cancellation intent remains independent.
		std::shared_ptr<const std::uint8_t> managed_subagent_control_incarnation = std::make_shared<const std::uint8_t>(0);
		std::unordered_map<std::string, std::shared_ptr<const ManagedSubagentCancellationReceipt>> managed_subagent_cancellation_receipts;
		std::unordered_map<std::string, int64_t> agent_run_deadline_steady_ms;
		std::unordered_map<std::string, std::deque<int64_t>> agent_provider_crash_times_epoch_ms;
		std::unordered_map<std::string, int64_t> agent_provider_circuit_open_until_epoch_ms;
		int selected_chat_index = -1;
		std::uint64_t state_revision = 0;

		std::string composer_text;
		std::string new_chat_folder_id;
		std::string move_chat_pending_id;
		std::string move_chat_original_folder_id;
		std::string move_chat_original_workspace;
		std::string move_chat_target_folder_id;
		std::string move_chat_target_workspace;
		bool move_chat_show_missing_session_warning = false;
		std::unordered_set<std::string> collapsed_branch_chat_ids;
		std::unordered_set<std::string> chats_with_unseen_updates;
		std::unordered_set<std::string> filtered_chat_ids;
		std::string status_line;
		// Ownership of the native-sync failure currently shown in status_line.
		std::string native_chat_refresh_error_target_id;
		std::string native_chat_refresh_error_status;
		std::vector<std::unique_ptr<CliTerminalState>> cli_terminals;
		std::vector<std::unique_ptr<AcpSessionState>> acp_sessions;
		// Bounded pending local sink observations; retiring one never retires input/cleanup authority.
		std::vector<std::shared_ptr<LocalInputWriteObservation>> local_input_write_observations;
		/// Application-owned hooks keep native compaction queries outside runtime and transport state.
		std::function<void(AcpSessionState&, const ChatSession&)> opencode_compaction_before_turn_reset;
		std::function<OpenCodeCompactionGateState(AcpSessionState&, ChatSession&, std::string&)> opencode_compaction_before_dispatch;
		std::vector<std::unique_ptr<PendingAcpRemoteStop>> pending_acp_remote_stops;
		std::vector<AsyncAcpProcessStopTask> acp_process_stop_tasks;
		std::vector<CliContextPreparationTask> cli_context_preparation_tasks;
		std::unique_ptr<std::jthread> provider_context_cleanup_worker;
		std::shared_ptr<std::atomic<bool>> provider_context_cleanup_finished;
		double provider_context_cleanup_not_before_s = 0.0;
		std::vector<RemoteContextTask> remote_context_tasks;
		std::vector<MemoryRecallTask> memory_recall_tasks;
		std::vector<ClaudeInputPreparationTask> claude_input_tasks;
		std::vector<std::shared_ptr<PendingLocalAcpLaunch>> local_acp_launch_tasks;
		std::vector<std::shared_ptr<PendingAgentInputPreparation>> agent_input_preparation_tasks;
		// Runtime-only discovery contexts; never serialized or persisted as user chats.
		std::vector<ChatSession> model_discovery_chats;
		std::vector<PendingModelDiscoveryRetry> pending_model_discovery_retries;

		std::unordered_map<std::string, std::string> resolved_native_sessions_by_chat_id;
		AsyncCommandTask runtime_cli_version_check_task;
		std::unordered_map<std::string, AsyncCommandTask> runtime_cli_install_tasks;
		std::unordered_map<std::string, PendingCliUpdate> pending_cli_updates;
		std::string runtime_cli_version_provider_id;
		std::deque<std::pair<std::string, std::string>> runtime_cli_version_check_queue;
		bool remote_host_health_changed = false;
		std::vector<AsyncMemoryExtractionTask> memory_extraction_tasks;
		std::vector<AsyncPermissionReviewTask> permission_review_tasks;
		std::vector<AsyncTurnCheckpointTask> turn_checkpoint_tasks;
		std::deque<PendingGoalIterationState> pending_goal_iterations;
		std::deque<QueuedMemoryExtractionTask> memory_extraction_queue;
		std::unordered_map<std::string, double> memory_idle_started_at_by_chat_id;
		std::unordered_map<std::string, double> memory_retry_not_before_by_chat_id;
		std::unordered_map<std::string, int> memory_failure_count_by_chat_id;
		double memory_global_retry_not_before = 0.0;
		int memory_global_failure_count = 0;
		std::unordered_map<std::string, CliProviderVersionState> runtime_cli_versions_by_provider_id;
		std::unordered_map<std::string, ComputerUseRuntimeState> computer_use_by_chat_id;
		std::unordered_set<std::string> computer_use_revocation_pending_chat_ids;
		std::string memory_last_status;
		MemoryActivityState memory_activity;
		platform::AsyncNativeChatLoadTask native_chat_load_task;
		std::size_t open_code_history_scan_offset = 0;
		std::unordered_map<std::string, platform::AsyncNativeChatLoadTask> native_chat_load_tasks;
		std::uint64_t terminal_native_history_load_generation = 0;
		std::shared_ptr<TerminalNativeMediaCoordinator> terminal_native_media;

		std::unordered_map<std::string, double> pending_chat_save_at_by_chat_id;
		std::unordered_map<std::string, std::shared_ptr<PendingChatSaveTask>> pending_chat_save_tasks;
		std::unordered_map<std::string, std::shared_ptr<PendingAcpCompletedPromptRecoveries>> pending_acp_completed_prompt_recoveries;
		// A failed durable barrier can retain a scoped retry after removing its active task.
		std::unordered_map<std::string, std::weak_ptr<PendingChatSaveTask>> pending_chat_save_retry_owners;
		std::shared_ptr<MemoryActivityRefreshTask> memory_activity_refresh_task;
		std::unordered_map<std::string, double> remote_recovery_hydration_retry_not_before_by_chat_id;
		std::optional<DeletionProgress> deletion_progress;
		std::unordered_set<std::string> worktree_operation_chat_ids;
		std::unordered_map<std::string, std::shared_ptr<ChatBaselinePreparation>> repository_baseline_preparations;
		std::vector<std::shared_ptr<ChatBaselinePreparation>> retired_repository_baseline_preparations;
		struct GitPortRequest
		{
			enum class WorkerState { Queued, Running, Finished };
			std::atomic<WorkerState> worker_state{WorkerState::Queued};
			std::shared_ptr<WorkspaceLeaseGate::Lease> lease;
			std::weak_ptr<void> handler_lifetime;
			bool lifetime_bound = false;
			bool gui_settled = false;
			int browser_id = -1;
			int64_t query_id = -1;
			std::string frame_id;
			std::filesystem::path root;
			std::shared_ptr<const uint8_t> incarnation;
			std::string workspace, source, worktree, branch, base;
			std::shared_ptr<std::stop_source> cancellation = std::make_shared<std::stop_source>();
		};
		std::unordered_map<std::string, std::shared_ptr<GitPortRequest>> git_port_requests;
		std::unordered_map<std::string, std::shared_ptr<WorktreeLaunchPreparationTask>> worktree_launch_preparations;
		std::unordered_map<std::string, std::weak_ptr<void>> remote_vcs_operation_leases_by_host_id;

		std::unique_ptr<ProviderModelCatalogService> provider_model_catalog;
		std::shared_ptr<OpenCodeNativeActivityService> opencode_native_activity;
	};

} // namespace uam
