#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/provider/runtime/provider_build_config.h"

/// <summary>
/// Message role marker persisted with each chat message entry.
/// </summary>
enum class MessageRole
{
	User,
	Assistant,
	System
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
};

struct MessagePlanEntry
{
	std::string content;
	std::string priority;
	std::string status;
};

struct MessageBlock
{
	std::string type;
	std::string text;
	std::string tool_call_id;
	std::string request_id_json;
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
};

namespace uam
{
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
		bool goal_mode = false;
		std::string goal_id;
		bool computer_use_mode = false;
		bool priority_steer = false;
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
struct ChatSession
{
	std::string id;
	std::string execution_host_id = "local";
	std::string provider_id;
	std::string native_session_id;
	// Persisted only while a remote structured turn is active. A GUI restart uses
	// this to reattach to the existing runner process without replaying the prompt.
	bool remote_turn_reconnect_pending = false;
	// Conservatively remains true from remote proxy launch until helper process
	// removal is confirmed. This is independent of whether a turn is active.
	bool remote_process_exists = false;
	// Persisted until a previously requested idle remote stop is confirmed.
	bool remote_stop_cleanup_pending = false;
	// Persisted from a stop-then-restart request until the replacement prompt is
	// durably delivered or the abandoned restart is cleaned up after relaunch.
	bool remote_restart_pending = false;
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
	std::vector<uam::AcpQueuedUserPromptState> acp_queued_prompts;
	std::size_t acp_dispatched_queued_prompt_count = 0;
	std::string parent_chat_id;
	std::string branch_root_chat_id;
	int branch_from_message_index = -1;
	bool branch_message_edited = false;
	std::string folder_id;
	std::string title;
	std::string created_at;
	std::string updated_at;
	std::string last_opened_at;
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
	std::string workspace_branch_name;
	std::string workspace_worktree_directory;
	bool imported_read_only = false;
	std::string approval_mode;
	std::string uam_agent_id = "build";
	std::string agent_run_id;
	// Fresh, bounded transcript owned by a goal on another visible chat.
	// Empty on ordinary chats and on all legacy data.
	std::string goal_owner_chat_id;
	std::string goal_iteration_goal_id;
	std::string goal_iteration_turn_kind;
	int goal_iteration_repair_attempts = 0;
	bool uam_control_enabled = false;
	std::string command_safety_tier = "off";
	// Enablement and the selected OS target are intentionally runtime-only.
	// Only the backend preference is persisted with the chat.
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
};

struct ResourceReference
{
	std::string id;
	std::string type;
	std::string target;
	std::string label;
};

struct ResourceCollection
{
	std::string id;
	std::string name;
	bool collapsed = false;
	std::vector<ResourceReference> references;
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
};

struct McpServerConfiguration
{
	std::string id;
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
};

/// <summary>
/// Persisted application settings.
/// </summary>
struct AppSettings
{
	std::string active_provider_id = provider_build_config::FirstEnabledProviderId();
	std::string provider_extra_flags;
	int cli_idle_timeout_seconds = 600;
	int active_turn_inactivity_timeout_seconds = 1800;
	std::string ui_theme = "focus";
	bool show_provider_icons_in_sidebar = true;
	bool show_worktree_path_in_sidebar = true;
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
	std::string update_last_checked_at;
	std::map<std::string, std::string> dismissed_update_versions;
	std::map<std::string, MemoryWorkerBinding> memory_worker_bindings;
	std::string permission_reviewer_provider_id;
	std::string permission_reviewer_model_id;
	std::string default_new_chat_provider_id = provider_build_config::FirstEnabledProviderId();
	std::map<std::string, ProviderChatDefaults> provider_chat_defaults;
	std::string markdown_store_directory;
	std::string default_editor_preset_id = "vscode";
	int editor_default_groups_version = 0;
	std::vector<EditorFileAssociation> editor_file_associations;
	std::vector<McpServerConfiguration> mcp_servers;
	std::vector<ExecutionHost> execution_hosts;
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
	std::chrono::steady_clock::time_point launch_time;
	std::string provider_id;
	int64_t estimated_input_tokens = 0;
};

struct PendingRuntimeCall
{
	std::string chat_id;
	std::string resume_session_id;
	std::string provider_id_snapshot;
	std::string native_history_chats_dir_snapshot;
	std::vector<std::string> session_ids_before;
	std::string command_preview;
	std::shared_ptr<AsyncProcessTaskState> state;
	std::unique_ptr<std::jthread> worker;
};

inline void ResetPendingRuntimeCall(PendingRuntimeCall& call)
{
	if (call.worker != nullptr)
	{
		call.worker->request_stop();
		call.worker.reset();
	}

	call.state.reset();
}

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
