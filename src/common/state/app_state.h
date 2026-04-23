#pragma once

#include "common/models/app_models.h"
#include "common/config/frontend_actions.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/provider_profile.h"

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
	/// Scrollback line stub retained for legacy terminal state references.
	/// Retained so that code referencing TerminalScrollbackLine still compiles.
	/// </summary>
	struct TerminalScrollbackLine
	{
		// xterm.js handles rendering in the React frontend.
	};

	/// <summary>
	/// Maximum number of scrollback lines (kept for backward compatibility).
	/// </summary>
	inline constexpr std::size_t kTerminalScrollbackMaxLines = 5000;

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
		int rows = 24;
		int cols = 80;
		bool scroll_to_bottom = false;
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
		std::string protocol_kind = "gemini-acp";
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
		int next_request_id = 1;
		int initialize_request_id = 0;
		int session_setup_request_id = 0;
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
			std::vector<AcpTurnEventState> turn_events;
			AcpPendingPermissionState pending_permission;
			AcpPendingUserInputState pending_user_input;
		};

	/// <summary>
	/// Background command execution container for Gemini compatibility checks.
	/// </summary>
	struct AsyncCommandTask
	{
		bool running = false;
		std::string command_preview;
		std::shared_ptr<AsyncProcessTaskState> state;
		std::unique_ptr<std::jthread> worker;
	};

	struct AsyncMemoryExtractionTask
	{
		bool running = false;
		std::string chat_id;
		int message_count = 0;
		std::filesystem::path workspace_root;
		std::shared_ptr<AsyncProcessTaskState> state;
		std::unique_ptr<std::jthread> worker;
	};

	/// <summary>
	/// Shared application state for the CEF/React Gemini CLI release slice.
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
			int latest_imported_count = 0;
			int latest_import_total_count = 0;

		std::string composer_text;
		std::string attach_file_input;
		std::string new_chat_folder_id;
		bool open_new_chat_popup = false;
		std::string pending_new_chat_provider_id;
		bool open_duplicate_new_chat_popup = false;
		std::string pending_duplicate_new_chat_existing_id;
		std::string pending_duplicate_new_chat_provider_id;
		std::string pending_duplicate_new_chat_folder_id;
		std::string new_folder_title_input;
		std::string new_folder_directory_input;
		std::string pending_move_chat_to_new_folder_id;
		std::string move_chat_pending_id;
		std::string move_chat_original_folder_id;
		std::string move_chat_original_workspace;
		std::string move_chat_target_folder_id;
		std::string move_chat_target_workspace;
		bool move_chat_show_missing_session_warning = false;
		std::string editing_chat_id;
		int editing_message_index = -1;
		std::string editing_message_text;
		std::string rename_chat_target_id;
		std::string rename_chat_input;
		bool open_rename_chat_popup = false;
		std::string inline_title_editing_chat_id;
		std::string pending_branch_chat_id;
		int pending_branch_message_index = -1;
		bool open_sidebar_chat_options_popup = false;
		std::string sidebar_chat_options_popup_chat_id;
		bool open_edit_message_popup = false;
		bool open_about_popup = false;
		bool open_app_settings_popup = false;
		bool open_delete_chat_popup = false;
		std::string pending_delete_chat_id;
		bool open_delete_folder_popup = false;
		std::string pending_delete_folder_id;
		bool open_folder_settings_popup = false;
		std::string pending_folder_settings_id;
		std::string folder_settings_title_input;
		std::string folder_settings_directory_input;
		std::unordered_set<std::string> collapsed_branch_chat_ids;
		std::unordered_set<std::string> chats_with_unseen_updates;
		std::string sidebar_search_query;
		std::string last_sidebar_search_query;
		bool sidebar_resize_drag_active = false;
		std::unordered_set<std::string> filtered_chat_ids;
		std::string status_line;
		CenterViewMode center_view_mode = CenterViewMode::CliConsole;
		std::vector<std::unique_ptr<CliTerminalState>> cli_terminals;
		std::vector<std::unique_ptr<AcpSessionState>> acp_sessions;

		std::vector<PendingRuntimeCall> pending_calls;
		std::unordered_map<std::string, std::string> resolved_native_sessions_by_chat_id;
		AsyncCommandTask runtime_cli_version_check_task;
		AsyncCommandTask runtime_cli_pin_task;
		std::vector<AsyncMemoryExtractionTask> memory_extraction_tasks;
		std::unordered_map<std::string, double> memory_idle_started_at_by_chat_id;
		std::string memory_last_status;
		bool runtime_cli_version_checked = false;
		bool runtime_cli_version_supported = false;
		std::string runtime_cli_installed_version;
		std::string runtime_cli_version_raw_output;
		std::string runtime_cli_version_message;
		std::string runtime_cli_pin_output;
		bool scroll_to_bottom = false;
		platform::AsyncNativeChatLoadTask native_chat_load_task;
		std::unordered_map<std::string, platform::AsyncNativeChatLoadTask> native_chat_load_tasks;
	};

} // namespace uam
