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
	std::vector<ToolCall> tool_calls;
	std::string thoughts;
	std::string plan_summary;
	std::vector<MessagePlanEntry> plan_entries;
	std::vector<MessageBlock> blocks;
	std::vector<std::string> markdown_store_files;
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
};

/// <summary>
/// Chat session metadata and message history.
/// </summary>
struct ChatSession
{
	std::string id;
	std::string provider_id;
	std::string native_session_id;
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
	std::string approval_mode;
	bool auto_approve_commands = false;
	std::string command_safety_tier = "medium";
	std::string model_id;
	std::string reasoning_effort;
	std::string service_tier;
	std::string extra_flags;
	bool memory_enabled = true;
	int memory_last_processed_message_count = 0;
	std::string memory_last_processed_at;
	std::vector<Goal> goals;
	std::string active_goal_id;
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
	bool auto_approve_commands = false;
	bool memory_enabled = true;
	std::string reasoning_effort;
	std::string service_tier;
	std::string memory_level = "strict";
	bool small_model_mode = false;
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
	bool accepts_files = true;
	bool accepts_folders = true;
	bool enabled = true;
	bool open_workspace = false;
};

/// <summary>
/// Persisted application settings.
/// </summary>
struct AppSettings
{
	std::string active_provider_id = provider_build_config::FirstEnabledProviderId();
	bool provider_yolo_mode = false;
	std::string provider_extra_flags;
	std::string runtime_backend = "provider-cli";
	int cli_idle_timeout_seconds = 600;
	// Legacy keys retained for backward-compatible load paths.
	bool gemini_yolo_mode = false;
	std::string gemini_extra_flags;
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
	int goal_max_loop_iterations = 200; // 0 = unlimited
	bool update_checks_enabled = true;
	std::string update_last_checked_at;
	std::map<std::string, std::string> dismissed_update_versions;
	std::map<std::string, MemoryWorkerBinding> memory_worker_bindings;
	std::string default_new_chat_provider_id = provider_build_config::FirstEnabledProviderId();
	std::map<std::string, ProviderChatDefaults> provider_chat_defaults;
	std::string markdown_store_directory;
	std::string default_editor_preset_id = "vscode";
	int editor_default_groups_version = 0;
	std::vector<EditorFileAssociation> editor_file_associations;
	std::string memory_level_default = "strict";
	std::string voice_input_mode = "system";
	std::string voice_input_server_base_url = "https://api.openai.com";
	std::string voice_input_server_endpoint = "/v1/audio/transcriptions";
	std::string voice_input_server_model = "whisper-1";
	// A credential environment-variable name is persisted; the credential itself never is.
	std::string voice_input_api_key_env = "OPENAI_API_KEY";
};

/// <summary>
/// Main center-pane rendering mode.
/// </summary>
enum class CenterViewMode
{
	CliConsole
};

/// <summary>
/// In-flight provider command state used by async polling.
/// </summary>
struct ProcessExecutionResult
{
	bool ok = false;
	bool timed_out = false;
	bool canceled = false;
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
/// Converts center view mode enum into persisted text.
/// </summary>
std::string ViewModeToString(CenterViewMode mode);
/// <summary>
/// Parses persisted center view mode text into an enum value.
/// </summary>
CenterViewMode ViewModeFromString(std::string_view value);
/// <summary>
/// Converts goal status enum into persisted text.
/// </summary>
std::string GoalStatusToString(GoalStatus status);
/// <summary>
/// Parses persisted goal status text into an enum value.
/// </summary>
GoalStatus GoalStatusFromString(std::string_view value);
