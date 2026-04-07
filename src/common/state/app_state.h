#pragma once

#include <vterm.h>

#include "common/models/app_models.h"
#include "common/config/frontend_actions.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/provider_profile.h"
#include "common/rag/rag_index_service.h"
#include "common/runtime/local_engine_runtime_service.h"
#include "common/vcs/vcs_workspace_service.h"

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
	/// Immutable snapshot of one scrollback line used by the embedded terminal renderer.
	/// </summary>
	struct TerminalScrollbackLine
	{
		std::vector<VTermScreenCell> cells;
	};

	/// <summary>
	/// Maximum number of scrollback lines preserved per embedded terminal.
	/// </summary>
	inline constexpr std::size_t kTerminalScrollbackMaxLines = 5000;

	/// <summary>
	/// Runtime state for one embedded provider CLI terminal instance.
	/// </summary>
	struct CliTerminalState : public platform::CliTerminalPlatformFields
	{
		bool running = false;
		std::string attached_chat_id;
		std::string attached_session_id;
		std::vector<std::string> session_ids_before;
		std::vector<std::string> linked_files_snapshot;
		VTerm* vt = nullptr;
		VTermScreen* screen = nullptr;
		VTermState* state = nullptr;
		int rows = 24;
		int cols = 80;
		std::deque<TerminalScrollbackLine> scrollback_lines;
		int scrollback_view_offset = 0;
		bool scroll_to_bottom = false;
		bool needs_full_refresh = true;
		bool should_launch = false;
		double last_sync_time_s = 0.0;
		double last_output_time_s = 0.0;
		double last_activity_time_s = 0.0;
		double last_polled_time_s = 0.0;
		bool input_ready = false;
		double startup_time_s = 0.0;
		std::deque<std::string> pending_structured_prompts;
		bool generation_in_progress = false;
		std::string last_error;
		int sel_start_row = -1;
		int sel_start_col = -1;
		int sel_end_row = -1;
		int sel_end_col = -1;
		bool has_selection = false;
		double cursor_blink_last_time = 0.0;
		bool cursor_visible = true;
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

	/// <summary>
	/// Runtime state for the UAM-managed OpenCode bridge process.
	/// </summary>
	struct OpenCodeBridgeState : public platform::OpenCodeBridgePlatformFields
	{
		bool running = false;
		bool healthy = false;
		std::string endpoint;
		std::string api_base;
		std::string token;
		std::string selected_model;
		std::string requested_model;
		std::string model_folder;
		std::string ready_file;
		std::string last_error;
	};

	/// <summary>
	/// Shared application state for runtime services and all Dear ImGui views.
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
		std::string template_import_path_input;
		std::string template_manager_selected_id;
		std::string template_rename_input;
		std::string editing_chat_id;
		int editing_message_index = -1;
		std::string editing_message_text;
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
		bool open_template_manager_popup = false;
		bool template_catalog_dirty = true;
		std::vector<TemplateCatalogEntry> template_catalog;
		bool open_template_change_warning_popup = false;
		std::string pending_template_change_chat_id;
		std::string pending_template_change_override_id;
		std::unordered_set<std::string> collapsed_branch_chat_ids;
		std::unordered_set<std::string> chats_with_unseen_updates;
		std::string status_line;
		CenterViewMode center_view_mode = CenterViewMode::Structured;
		std::vector<std::unique_ptr<CliTerminalState>> cli_terminals;
		RagIndexService rag_index_service;
		LocalEngineRuntimeService runtime_model_service;
		OpenCodeBridgeState opencode_bridge;
		std::string loaded_runtime_model_id;
		std::unordered_map<std::string, VcsSnapshot> vcs_snapshot_by_workspace;
		std::unordered_set<std::string> vcs_snapshot_loaded_workspaces;
		std::unordered_map<std::string, std::string> rag_last_refresh_by_workspace;
		std::unordered_map<std::string, std::string> rag_last_rebuild_at_by_workspace;
		RagScanState rag_scan_state;
		double rag_finished_visible_until_s = 0.0;
		double rag_scan_status_last_emit_s = 0.0;
		std::string rag_scan_workspace_key;
		bool open_rag_console_popup = false;
		std::vector<std::string> rag_scan_reports;
		bool rag_scan_reports_scroll_to_bottom = false;
		std::string rag_manual_query_input;
		int rag_manual_query_max = 6;
		int rag_manual_query_min = 1;
		bool rag_manual_query_running = false;
		std::string rag_manual_query_error;
		std::string rag_manual_query_last_query;
		std::string rag_manual_query_workspace_key;
		std::vector<RagSnippet> rag_manual_query_results;
		bool open_vcs_output_popup = false;
		std::string vcs_output_popup_title;
		std::string vcs_output_popup_content;
		bool open_runtime_model_selection_popup = false;
		std::string runtime_model_selection_id;

		std::vector<PendingRuntimeCall> pending_calls;
		std::unordered_map<std::string, std::string> resolved_native_sessions_by_chat_id;
		AsyncCommandTask runtime_cli_version_check_task;
		AsyncCommandTask runtime_cli_pin_task;
		bool runtime_cli_version_checked = false;
		bool runtime_cli_version_supported = false;
		std::string runtime_cli_installed_version;
		std::string runtime_cli_version_raw_output;
		std::string runtime_cli_version_message;
		std::string runtime_cli_pin_output;
		bool scroll_to_bottom = false;
		platform::AsyncNativeChatLoadTask native_chat_load_task;
	};

} // namespace uam
