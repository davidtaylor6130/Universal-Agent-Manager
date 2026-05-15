#pragma once

#include "common/models/app_models.h"
#include "common/config/frontend_actions.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/provider_profile.h"
#include "common/runtime/terminal/terminal_dimensions.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
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
		bool input_ready = false;
		double startup_time_s = 0.0;
		bool generation_in_progress = false;
		CliTerminalTurnState turn_state = CliTerminalTurnState::Idle;
		CliTerminalLifecycleState lifecycle_state = CliTerminalLifecycleState::Stopped;
		std::string recent_output_bytes;
		std::string last_native_history_snapshot_digest;
		std::string last_error;
	};

	struct AcpToolCallState
	{
		std::string id;
		std::string title;
		std::string kind;
		std::string status;
		std::string content;
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
		bool load_session_supported = false;
		bool processing = false;
		bool waiting_for_permission = false;
		bool waiting_for_user_input = false;
		bool cancel_requested = false;
		int next_request_id = 1;
		int initialize_request_id = 0;
		int session_setup_request_id = 0;
		int startup_model_request_id = 0;
		int prompt_request_id = 0;
		int cancel_request_id = 0;
		int current_assistant_message_index = -1;
		int turn_user_message_index = -1;
		int turn_assistant_message_index = -1;
		int turn_serial = 0;
		std::string queued_prompt;
		bool ignore_session_updates_until_ready = false;
		bool codex_resume_fallback_attempted = false;
		bool gemini_resume_fallback_attempted = false;
		std::string stdout_buffer;
		std::string stderr_buffer;
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
		std::vector<AcpModeState> available_modes;
		std::string current_mode_id;
		std::vector<AcpModelState> available_models;
		std::string current_model_id;
		std::string pending_startup_model_id;
		std::vector<AcpTurnEventState> turn_events;
		AcpPendingPermissionState pending_permission;
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
		std::string raw_output;
		std::string message;
		std::string install_output;
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
		std::vector<ProviderProfile> provider_profiles;
		uam::FrontendActionMap frontend_actions;

		std::vector<ChatSession> chats;
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
		CenterViewMode center_view_mode = CenterViewMode::CliConsole;
		std::vector<std::unique_ptr<CliTerminalState>> cli_terminals;
		std::vector<std::unique_ptr<AcpSessionState>> acp_sessions;

		std::vector<PendingRuntimeCall> pending_calls;
		std::unordered_map<std::string, std::string> resolved_native_sessions_by_chat_id;
		AsyncCommandTask runtime_cli_version_check_task;
		AsyncCommandTask runtime_cli_pin_task;
		std::string runtime_cli_version_provider_id;
		std::string runtime_cli_pin_provider_id;
		std::vector<AsyncMemoryExtractionTask> memory_extraction_tasks;
		std::deque<QueuedMemoryExtractionTask> memory_extraction_queue;
		std::unordered_map<std::string, double> memory_idle_started_at_by_chat_id;
		std::unordered_map<std::string, double> memory_retry_not_before_by_chat_id;
		std::unordered_map<std::string, int> memory_failure_count_by_chat_id;
		std::unordered_map<std::string, CliProviderVersionState> runtime_cli_versions_by_provider_id;
		std::string memory_last_status;
		MemoryActivityState memory_activity;
		platform::AsyncNativeChatLoadTask native_chat_load_task;
		std::unordered_map<std::string, platform::AsyncNativeChatLoadTask> native_chat_load_tasks;
	};

} // namespace uam
