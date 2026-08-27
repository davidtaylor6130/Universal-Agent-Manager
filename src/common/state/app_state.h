#pragma once

#include "app/provider_model_catalog_service.h"
#include "common/models/app_models.h"
#include "common/config/frontend_actions.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/provider_profile.h"
#include "common/runtime/terminal/terminal_dimensions.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam
{

	namespace fs = std::filesystem;

	/// <summary>
	/// Runtime state for one embedded provider CLI terminal instance.
	/// PTY output is forwarded to xterm.js in the React frontend via
	/// uam::PushCliOutput().
	/// </summary>
	enum class CliTerminalTurnState
	{
		Idle,
		Busy,
	};

	enum class CliTerminalLifecycleState
	{
		Disabled,
		Stopped,
		Idle,
		Busy,
		ShuttingDown,
	};

	struct CliTerminalState : public platform::CliTerminalPlatformFields
	{
		std::string terminal_id;
		std::string frontend_chat_id;
		bool running = false;
		std::string attached_chat_id;
		std::string attached_session_id;
		std::vector<std::string> session_ids_before;
		std::vector<std::string> linked_files_snapshot;
		int rows = kCliTerminalDefaultRows;
		int cols = kCliTerminalDefaultCols;
		bool should_launch = false;
		bool ui_attached = false;
		double last_sync_time_s = 0.0;
		double last_output_time_s = 0.0;
		double last_activity_time_s = 0.0;
		double last_user_input_time_s = 0.0;
		double last_ai_output_time_s = 0.0;
		double last_polled_time_s = 0.0;
		double last_idle_confirmed_time_s = 0.0;
		double last_busy_time_s = 0.0;
		double shutdown_requested_time_s = 0.0;
		double inactivity_interrupt_requested_time_s = 0.0;
		bool input_ready = false;
		double startup_time_s = 0.0;
		bool generation_in_progress = false;
		CliTerminalTurnState turn_state = CliTerminalTurnState::Idle;
		CliTerminalLifecycleState lifecycle_state = CliTerminalLifecycleState::Stopped;
		std::string recent_output_bytes;
		std::string current_turn_output_bytes;
		bool prompt_settle_required = false;
		double prompt_settle_candidate_time_s = 0.0;
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
		std::string permission_review_decision;
		std::string permission_review_reason;
		bool is_sub_agent = false;
		std::string sub_agent_id;
		std::string sub_agent_title;
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
	};

	struct AcpTurnEventState
	{
		std::string type;
		std::string text;
		std::string tool_call_id;
		std::string request_id_json;
	};

	struct AcpReplayUpdateState
	{
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
		std::int64_t updated_at_ms = 0;
		AcpTokenUsageBreakdownState total;
		AcpTokenUsageBreakdownState last;
		std::int64_t model_context_window = 0;
	};

	struct AcpRateLimitWindowState
	{
		bool available = false;
		int used_percent = 0;
		std::int64_t resets_at = 0;
		std::int64_t window_duration_minutes = 0;
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
		bool is_secret = false;
		std::vector<AcpUserInputOptionState> options;
	};

	struct AcpPendingUserInputState
	{
		std::string request_id_json;
		std::string item_id;
		std::string status;
		std::string attention_kind;
		std::vector<AcpUserInputQuestionState> questions;
	};

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
		bool goal_mode = false;
		std::string goal_id;
		bool priority_steer = false;
	};

	struct PendingModelDiscoveryRetry
	{
		std::string chat_id;
		std::string provider_id;
		std::string workspace_directory;
	};

	struct UamControlCapability
	{
		std::string id;
		std::filesystem::path directory;
		std::string chat_id;
		std::string session_chat_id;
		std::string provider_id;
		std::string agent_id;
		std::string agent_run_id;
		std::vector<std::string> agent_skills;
		std::vector<std::string> agent_delegates;
		int64_t expires_at_epoch_ms = 0;
		std::deque<int64_t> request_times_epoch_ms;
		std::unordered_set<std::string> seen_request_ids;
		std::unordered_set<std::string> owned_agent_run_ids;
	};

	struct AcpSessionState : public platform::StdioProcessPlatformFields
	{
		std::string chat_id;
		std::string provider_id;
		std::string protocol_kind = uam::provider_profile_constants::kProtocolGeminiAcp;
		std::string session_id;
		std::string codex_thread_id;
		std::string codex_turn_id;
		std::string lifecycle_state = "stopped";
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
		int cancel_request_id = 0;
		int current_assistant_message_index = -1;
		int turn_user_message_index = -1;
		int turn_assistant_message_index = -1;
		int turn_serial = 0;
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
		std::string uam_control_capability_id;
		bool managed_launch_attempted = false;
		int last_settled_turn_serial = 0;
		std::string last_turn_outcome;
		std::string last_turn_error;
		double turn_started_time_s = 0.0;
		std::string queued_prompt;
		std::deque<AcpQueuedUserPromptState> queued_user_prompts;
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
		bool ignore_session_updates_until_ready = false;
		bool codex_resume_fallback_attempted = false;
		bool acp_resume_fallback_attempted = false;
		std::string stdout_buffer;
		std::string stderr_buffer;
		bool stdout_poll_pending = false;
		bool stderr_poll_pending = false;
		std::string recent_stderr;
		std::string last_error;
		bool has_last_exit_code = false;
		int last_exit_code = 0;
		std::string last_process_id;
		std::vector<std::string> assistant_replay_prefixes;
		std::vector<AcpReplayUpdateState> load_history_replay_updates;
		std::vector<AcpDiagnosticEntryState> diagnostics;
		std::string pending_assistant_thoughts;
		std::string agent_name;
		std::string agent_title;
		std::string agent_version;
		std::unordered_map<int, std::string> pending_request_methods;
		std::vector<AcpToolCallState> tool_calls;
		std::vector<AcpPlanEntryState> plan_entries;
		std::string plan_summary;
		std::unordered_map<std::string, std::string> codex_agent_message_text_by_item_id;
		std::string codex_last_agent_message_item_id;
		std::unordered_set<std::string> codex_streamed_reasoning_keys;
		std::string codex_last_reasoning_section;
		std::vector<AcpCommandState> available_commands;
		std::vector<AcpModeState> available_modes;
		std::string current_mode_id;
		std::string mode_change_previous_id;
		std::optional<std::string> mode_change_previous_chat_id;
		std::optional<std::string> mode_change_previous_command_safety_tier;
		std::string mode_change_requested_id;
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
		bool wait_is_stale = false;
		std::string wait_stale_reason;
	};

	/// <summary>
	/// Background command execution container for provider CLI version checks.
	/// </summary>
	struct AsyncCommandTask
	{
		bool running = false;
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
		task.command_preview.clear();
		task.state.reset();
	}

	struct AsyncMemoryExtractionTask
	{
		bool running = false;
		std::string chat_id;
		std::string command_preview;
		int message_count = 0;
		int scan_start_message_index = -1;
		std::filesystem::path workspace_root;
		std::filesystem::path native_history_chats_dir;
		std::vector<std::string> native_history_files_before;
		std::shared_ptr<AsyncProcessTaskState> state;
		std::unique_ptr<std::jthread> worker;
	};

	struct AsyncPermissionReviewTask
	{
		std::string chat_id;
		std::string request_id_json;
		std::string command_preview;
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
		bool checked = false;
		bool supported = false;
		std::string installed_version;
		std::string selected_version;
		std::string install_method = "npm";
		std::string install_command;
		std::string last_install_status = "none";
		std::string raw_output;
		std::string message;
		std::string install_output;
	};

	struct PendingGoalIterationState
	{
		std::string owner_chat_id;
		std::string goal_id;
		std::string prompt;
		std::string turn_kind;
		int repair_attempts = 0;
	};

	/// <summary>
	/// Shared application state for the CEF/React provider CLI release slice.
	/// </summary>
	struct AppState
	{
		fs::path data_root;
		fs::path native_history_chats_dir;
		AppSettings settings;
		std::vector<ChatFolder> folders;
		std::vector<ResourceCollection> resource_collections;
		std::vector<ShellAction> shell_actions;
		std::string shell_action_notification;
		std::vector<ProviderProfile> provider_profiles;
		uam::FrontendActionMap frontend_actions;

		std::vector<ChatSession> chats;
		std::vector<AgentRun> agent_runs;
		std::vector<UamControlCapability> uam_control_capabilities;
		std::string uam_control_manager_id;
		std::deque<std::string> queued_agent_run_ids;
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
		std::vector<std::unique_ptr<CliTerminalState>> cli_terminals;
		std::vector<std::unique_ptr<AcpSessionState>> acp_sessions;
		// Runtime-only discovery contexts; never serialized or persisted as user chats.
		std::vector<ChatSession> model_discovery_chats;
		std::vector<PendingModelDiscoveryRetry> pending_model_discovery_retries;

		std::vector<PendingRuntimeCall> pending_calls;
		std::unordered_map<std::string, std::string> resolved_native_sessions_by_chat_id;
		AsyncCommandTask runtime_cli_version_check_task;
		AsyncCommandTask runtime_cli_pin_task;
		std::string runtime_cli_version_provider_id;
		std::string runtime_cli_pin_provider_id;
		std::deque<std::string> runtime_cli_version_check_queue;
		std::vector<AsyncMemoryExtractionTask> memory_extraction_tasks;
		std::vector<AsyncPermissionReviewTask> permission_review_tasks;
		std::vector<AsyncTurnCheckpointTask> turn_checkpoint_tasks;
		std::deque<PendingGoalIterationState> pending_goal_iterations;
		std::deque<QueuedMemoryExtractionTask> memory_extraction_queue;
		std::unordered_map<std::string, double> memory_idle_started_at_by_chat_id;
		std::unordered_map<std::string, double> memory_retry_not_before_by_chat_id;
		std::unordered_map<std::string, int> memory_failure_count_by_chat_id;
		std::unordered_map<std::string, CliProviderVersionState> runtime_cli_versions_by_provider_id;
		std::string memory_last_status;
		MemoryActivityState memory_activity;
		platform::AsyncNativeChatLoadTask native_chat_load_task;
		std::unordered_map<std::string, platform::AsyncNativeChatLoadTask> native_chat_load_tasks;

		std::unordered_map<std::string, double> pending_chat_save_at_by_chat_id;
		std::unordered_set<std::string> worktree_operation_chat_ids;

		std::unique_ptr<ProviderModelCatalogService> provider_model_catalog;
	};

} // namespace uam
