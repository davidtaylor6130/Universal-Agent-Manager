#pragma once
#include "common/platform/sdl_includes.h"
#include "common/platform/platform_services.h"

static void FreeCliTerminalVTerm(CliTerminalState& terminal)
{
	if (terminal.vt != nullptr)
	{
		vterm_free(terminal.vt);
		terminal.vt = nullptr;
		terminal.screen = nullptr;
		terminal.state = nullptr;
	}
}

static void CloseCliTerminalHandles(CliTerminalState& terminal)
{
	PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
}

static bool WriteToCliTerminal(CliTerminalState& terminal, const char* bytes, const size_t len)
{
	const bool wrote = PlatformServicesFactory::Instance().terminal_runtime.WriteToCliTerminal(terminal, bytes, len);

	if (wrote && bytes != nullptr && len > 0)
	{
		terminal.last_activity_time_s = ImGui::GetTime();
	}

	return wrote;
}

static void QueueStructuredPromptForTerminal(CliTerminalState& terminal, const std::string& prompt)
{
	if (prompt.empty())
	{
		return;
	}

	terminal.pending_structured_prompts.push_back(prompt);
	terminal.generation_in_progress = true;
	terminal.last_activity_time_s = ImGui::GetTime();
}

static bool InjectPromptAsPasteAndSubmit(CliTerminalState& terminal, const std::string& prompt, std::string* error_out = nullptr)
{
	if (prompt.empty())
	{
		return true;
	}

	if (!terminal.running)
	{
		if (error_out != nullptr)
		{
			*error_out = "Provider terminal is not running.";
		}

		return false;
	}

	if (terminal.vt == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Provider terminal VT is not initialized.";
		}

		return false;
	}

	static constexpr char kBracketedPasteStart[] = "\x1b[200~";
	static constexpr char kBracketedPasteEnd[] = "\x1b[201~";

	if (!WriteToCliTerminal(terminal, kBracketedPasteStart, sizeof(kBracketedPasteStart) - 1))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write bracketed paste start marker.";
		}

		return false;
	}

	if (!WriteToCliTerminal(terminal, prompt.c_str(), prompt.size()))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write prompt to provider terminal.";
		}

		return false;
	}

	if (!WriteToCliTerminal(terminal, kBracketedPasteEnd, sizeof(kBracketedPasteEnd) - 1))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write bracketed paste end marker.";
		}

		return false;
	}

	vterm_keyboard_key(terminal.vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
	terminal.generation_in_progress = true;
	terminal.last_activity_time_s = ImGui::GetTime();
	return true;
}

static bool FlushQueuedStructuredPromptsForTerminal(CliTerminalState& terminal, std::string* error_out = nullptr)
{
	while (!terminal.pending_structured_prompts.empty())
	{
		std::string prompt = std::move(terminal.pending_structured_prompts.front());
		terminal.pending_structured_prompts.pop_front();

		if (InjectPromptAsPasteAndSubmit(terminal, prompt, error_out))
		{
			continue;
		}

		terminal.pending_structured_prompts.push_front(std::move(prompt));
		return false;
	}

	return true;
}

static void ReportDroppedQueuedStructuredPromptsForTerminal(AppState& app, const CliTerminalState& terminal, const std::string& reason)
{
	if (terminal.pending_structured_prompts.empty() || terminal.attached_chat_id.empty())
	{
		return;
	}

	std::string message = "Structured prompt delivery failed before terminal became ready.";

	if (!reason.empty())
	{
		message += " Reason: " + reason;
	}

	const int chat_index = FindChatIndexById(app, terminal.attached_chat_id);

	if (chat_index >= 0)
	{
		AddMessage(app.chats[chat_index], MessageRole::System, message);
		SaveChat(app, app.chats[chat_index]);
	}

	app.status_line = message;
}

static void WriteBytesToPty(const char* bytes, const size_t len, void* user)
{
	if (user == nullptr || bytes == nullptr || len == 0)
	{
		return;
	}

	auto* terminal = static_cast<CliTerminalState*>(user);

	if (!WriteToCliTerminal(*terminal, bytes, len))
	{
		terminal->last_error = "Failed to write to provider terminal.";
	}
}

static int OnVTermDamage(VTermRect, void* user)
{
	if (user != nullptr)
	{
		static_cast<CliTerminalState*>(user)->needs_full_refresh = true;
	}

	return 1;
}

static int OnVTermMoveRect(VTermRect, VTermRect, void* user)
{
	if (user != nullptr)
	{
		static_cast<CliTerminalState*>(user)->needs_full_refresh = true;
	}

	return 1;
}

static int OnVTermMoveCursor(VTermPos, VTermPos, int, void* user)
{
	if (user != nullptr)
	{
		static_cast<CliTerminalState*>(user)->needs_full_refresh = true;
	}

	return 1;
}

static int OnVTermResize(int rows, int cols, void* user)
{
	if (user != nullptr)
	{
		auto* terminal = static_cast<CliTerminalState*>(user);
		terminal->rows = rows;
		terminal->cols = cols;
		terminal->scrollback_view_offset = std::clamp(terminal->scrollback_view_offset, 0, static_cast<int>(terminal->scrollback_lines.size()));
		terminal->needs_full_refresh = true;
	}

	return 1;
}

static VTermScreenCell BlankTerminalCell()
{
	VTermScreenCell cell{};
	cell.width = 1;
	return cell;
}

static int OnVTermScrollbackPushLine(int cols, const VTermScreenCell* cells, void* user)
{
	if (user == nullptr || cells == nullptr || cols <= 0)
	{
		return 1;
	}

	auto* terminal = static_cast<CliTerminalState*>(user);
	TerminalScrollbackLine line;
	line.cells.assign(cells, cells + cols);
	terminal->scrollback_lines.push_back(std::move(line));

	if (terminal->scrollback_lines.size() > kTerminalScrollbackMaxLines)
	{
		terminal->scrollback_lines.pop_front();
	}

	if (terminal->scrollback_view_offset > 0)
	{
		terminal->scrollback_view_offset = std::min(terminal->scrollback_view_offset + 1, static_cast<int>(terminal->scrollback_lines.size()));
	}

	terminal->needs_full_refresh = true;
	return 1;
}

static int OnVTermScrollbackPopLine(int cols, VTermScreenCell* cells, void* user)
{
	if (user == nullptr || cells == nullptr || cols <= 0)
	{
		return 0;
	}

	auto* terminal = static_cast<CliTerminalState*>(user);

	if (terminal->scrollback_lines.empty())
	{
		return 0;
	}

	TerminalScrollbackLine line = std::move(terminal->scrollback_lines.back());
	terminal->scrollback_lines.pop_back();

	const int copy_cols = std::min(cols, static_cast<int>(line.cells.size()));

	for (int i = 0; i < copy_cols; ++i)
	{
		cells[i] = line.cells[static_cast<std::size_t>(i)];
	}

	for (int i = copy_cols; i < cols; ++i)
	{
		cells[i] = BlankTerminalCell();
	}

	terminal->scrollback_view_offset = std::clamp(terminal->scrollback_view_offset, 0, static_cast<int>(terminal->scrollback_lines.size()));
	terminal->needs_full_refresh = true;
	return 1;
}

static int OnVTermScrollbackClear(void* user)
{
	if (user == nullptr)
	{
		return 1;
	}

	auto* terminal = static_cast<CliTerminalState*>(user);
	terminal->scrollback_lines.clear();
	terminal->scrollback_view_offset = 0;
	terminal->needs_full_refresh = true;
	return 1;
}

static const VTermScreenCallbacks kVTermScreenCallbacks = {OnVTermDamage, OnVTermMoveRect, OnVTermMoveCursor, nullptr, nullptr, OnVTermResize, OnVTermScrollbackPushLine, OnVTermScrollbackPopLine, OnVTermScrollbackClear, nullptr};

static void RequestCliTerminalQuit(CliTerminalState& terminal)
{
	if (!terminal.running || !uam::platform::CliTerminalHasWritableInput(terminal))
	{
		return;
	}

	static constexpr char kQuitCommand[] = "/quit\r\n";
	(void)WriteToCliTerminal(terminal, kQuitCommand, sizeof(kQuitCommand) - 1);
}

enum class CliTerminalStopMode
{
	Graceful,
	FastExit,
};

static void StopCliTerminal(CliTerminalState& terminal, const bool clear_identity = false, const CliTerminalStopMode stop_mode = CliTerminalStopMode::Graceful)
{
	PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, stop_mode == CliTerminalStopMode::FastExit);

	CloseCliTerminalHandles(terminal);
	FreeCliTerminalVTerm(terminal);
	terminal.running = false;
	terminal.scrollback_lines.clear();
	terminal.scrollback_view_offset = 0;
	terminal.needs_full_refresh = true;
	terminal.input_ready = false;
	terminal.startup_time_s = 0.0;
	terminal.pending_structured_prompts.clear();
	terminal.generation_in_progress = false;
	terminal.last_output_time_s = 0.0;

	if (clear_identity)
	{
		terminal.attached_chat_id.clear();
		terminal.attached_session_id.clear();
		terminal.session_ids_before.clear();
		terminal.linked_files_snapshot.clear();
		terminal.should_launch = false;
	}
}

static CliTerminalState* FindCliTerminalForChat(AppState& app, const std::string& chat_id)
{
	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && terminal->attached_chat_id == chat_id)
		{
			return terminal.get();
		}
	}

	return nullptr;
}

static bool ForwardEscapeToSelectedCliTerminal(AppState& app, const SDL_Event& event)
{
	if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP)
	{
		return false;
	}

	if (event.key.keysym.sym != SDLK_ESCAPE)
	{
		return false;
	}

	if (event.type == SDL_KEYUP)
	{
		return true;
	}

	if (event.key.repeat != 0)
	{
		return true;
	}

	ChatSession* selected = SelectedChat(app);

	if (selected == nullptr)
	{
		return true;
	}

	CliTerminalState* terminal = FindCliTerminalForChat(app, selected->id);

	if (terminal == nullptr || !terminal->running || terminal->vt == nullptr)
	{
		return true;
	}

	VTermModifier mod = VTERM_MOD_NONE;
	const SDL_Keymod key_mod = static_cast<SDL_Keymod>(event.key.keysym.mod);

	if ((key_mod & KMOD_CTRL) != 0)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
	}

	if ((key_mod & KMOD_SHIFT) != 0)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
	}

	if ((key_mod & KMOD_ALT) != 0)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
	}

	vterm_keyboard_key(terminal->vt, VTERM_KEY_ESCAPE, mod);
	terminal->needs_full_refresh = true;
	return true;
}

static bool HasPendingCallForChat(const AppState& app, const std::string& chat_id)
{
	for (const PendingGeminiCall& call : app.pending_calls)
	{
		if (call.chat_id == chat_id)
		{
			return true;
		}
	}

	return false;
}

static bool HasAnyPendingCall(const AppState& app)
{
	return !app.pending_calls.empty();
}

static const PendingGeminiCall* FirstPendingCallForChat(const AppState& app, const std::string& chat_id)
{
	for (const PendingGeminiCall& call : app.pending_calls)
	{
		if (call.chat_id == chat_id)
		{
			return &call;
		}
	}

	return nullptr;
}

static bool ChatHasRunningGemini(const AppState& app, const std::string& chat_id)
{
	if (chat_id.empty())
	{
		return false;
	}

	if (HasPendingCallForChat(app, chat_id))
	{
		return true;
	}

	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && terminal->running && terminal->attached_chat_id == chat_id && terminal->generation_in_progress)
		{
			return true;
		}
	}

	return false;
}

static void MarkChatUnseen(AppState& app, const std::string& chat_id)
{
	if (chat_id.empty())
	{
		return;
	}

	const ChatSession* selected = SelectedChat(app);

	if (selected != nullptr && selected->id == chat_id)
	{
		return;
	}

	app.chats_with_unseen_updates.insert(chat_id);
}

static void MarkSelectedChatSeen(AppState& app)
{
	const ChatSession* selected = SelectedChat(app);

	if (selected != nullptr)
	{
		app.chats_with_unseen_updates.erase(selected->id);
	}
}

static CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat)
{
	const std::string resume_id = ResolveResumeSessionIdForChat(app, chat);
	const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);
	const bool can_launch_terminal = ProviderRuntime::IsRuntimeEnabled(provider) && ProviderRuntime::UsesCliOutput(provider) && !ProviderRuntime::UsesInternalEngine(provider) && provider.supports_interactive;

	if (CliTerminalState* existing = FindCliTerminalForChat(app, chat.id))
	{
		if (existing->attached_session_id.empty() && !resume_id.empty())
		{
			existing->attached_session_id = resume_id;
		}

		if (!can_launch_terminal)
		{
			existing->should_launch = false;
		}

		return *existing;
	}

	auto terminal = std::make_unique<CliTerminalState>();
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = resume_id;
	terminal->should_launch = can_launch_terminal;
	app.cli_terminals.push_back(std::move(terminal));
	return *app.cli_terminals.back();
}

static void StopAndEraseCliTerminalForChat(AppState& app, const std::string& chat_id)
{
	auto matches_chat_terminal = [&](std::unique_ptr<CliTerminalState>& terminal)
	{
		if (terminal == nullptr || terminal->attached_chat_id != chat_id)
		{
			return false;
		}

		StopCliTerminal(*terminal, true, CliTerminalStopMode::FastExit);
		return true;
	};

	app.cli_terminals.erase(std::remove_if(app.cli_terminals.begin(), app.cli_terminals.end(), matches_chat_terminal), app.cli_terminals.end());
}

static void StopAllCliTerminals(AppState& app, const bool clear_identity = true)
{
	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr)
		{
			StopCliTerminal(*terminal, clear_identity);
		}
	}
}

static void FastStopCliTerminalsForExit(AppState& app)
{
	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
		{
			continue;
		}

		StopCliTerminal(*terminal_ptr, true, CliTerminalStopMode::FastExit);
	}
}

static void MarkSelectedCliTerminalForLaunch(AppState& app)
{
	ChatSession* selected = SelectedChat(app);

	if (selected == nullptr)
	{
		return;
	}

	const ProviderProfile& provider = ProviderForChatOrDefault(app, *selected);

	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		std::string reason = ProviderRuntime::DisabledReason(provider);
		app.status_line = reason.empty() ? "Selected provider runtime is disabled in this build." : reason;
		return;
	}

	if (!ProviderRuntime::UsesCliOutput(provider))
	{
		app.status_line = "CLI output is unavailable for the selected provider.";
		return;
	}

	if (ProviderRuntime::UsesInternalEngine(provider) || !provider.supports_interactive)
	{
		app.status_line = "Provider does not expose an interactive CLI runtime.";
		return;
	}

	CliTerminalState& terminal = EnsureCliTerminalForChat(app, *selected);
	terminal.should_launch = true;
}

static void FinalizeChatSyncSelection(AppState& app, const std::string& selected_before, const std::string& preferred_chat_id, const bool preserve_selection)
{
	const std::string previous_selected = !selected_before.empty() ? selected_before : preferred_chat_id;

	if (preserve_selection && !selected_before.empty() && FindChatIndexById(app, selected_before) >= 0)
	{
		SelectChatById(app, selected_before);
	}
	else if (!preferred_chat_id.empty() && FindChatIndexById(app, preferred_chat_id) >= 0)
	{
		SelectChatById(app, preferred_chat_id);
	}
	else if (!previous_selected.empty() && FindChatIndexById(app, previous_selected) >= 0)
	{
		SelectChatById(app, previous_selected);
	}
	else if (!app.chats.empty())
	{
		app.selected_chat_index = 0;
	}
	else
	{
		app.selected_chat_index = -1;
	}

	for (auto it = app.chats_with_unseen_updates.begin(); it != app.chats_with_unseen_updates.end();)
	{
		if (FindChatIndexById(app, *it) < 0)
		{
			it = app.chats_with_unseen_updates.erase(it);
		}
		else
		{
			++it;
		}
	}

	for (auto it = app.collapsed_branch_chat_ids.begin(); it != app.collapsed_branch_chat_ids.end();)
	{
		if (FindChatIndexById(app, *it) < 0)
		{
			it = app.collapsed_branch_chat_ids.erase(it);
		}
		else
		{
			++it;
		}
	}

	const ChatSession* selected_now = SelectedChat(app);
	const std::string selected_now_id = (selected_now != nullptr) ? selected_now->id : "";

	if (selected_now_id != selected_before)
	{
		app.composer_text.clear();
	}

	MarkSelectedChatSeen(app);
}

static void SyncChatsFromLoadedNative(AppState& app, std::vector<ChatSession> native_chats, const std::string& preferred_chat_id, const bool preserve_selection = false)
{
	const std::string selected_before = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
	ApplyLocalOverrides(app, native_chats);
	MigrateChatProviderBindingsToFixedModes(app);
	FinalizeChatSyncSelection(app, selected_before, preferred_chat_id, preserve_selection);
}

static void SyncChatsFromNative(AppState& app, const std::string& preferred_chat_id, const bool preserve_selection = false)
{
	const std::string selected_before = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";

	if (ActiveProviderUsesGeminiHistory(app))
	{
		RefreshGeminiChatsDir(app);
		std::vector<ChatSession> native = LoadNativeGeminiChats(app.gemini_chats_dir, ActiveProviderOrDefault(app));
		ApplyLocalOverrides(app, native);
	}
	else
	{
		app.chats = LoadChats(app);
		NormalizeChatBranchMetadata(app);
		NormalizeChatFolderAssignments(app);
	}

	MigrateChatProviderBindingsToFixedModes(app);
	FinalizeChatSyncSelection(app, selected_before, preferred_chat_id, preserve_selection);
}

static std::vector<std::string> ForceOpenCodeModelFlag(std::vector<std::string> argv, const std::string& provider_model_id)
{
	if (argv.empty() || Trim(provider_model_id).empty())
	{
		return argv;
	}

	std::vector<std::string> filtered;
	filtered.reserve(argv.size() + 2);

	for (std::size_t i = 0; i < argv.size(); ++i)
	{
		const std::string& arg = argv[i];

		if (arg == "--model" || arg == "-m")
		{
			if (i + 1 < argv.size())
			{
				++i;
			}

			continue;
		}

		if (arg.rfind("--model=", 0) == 0 || arg.rfind("-m=", 0) == 0)
		{
			continue;
		}

		filtered.push_back(arg);
	}

	filtered.push_back("--model");
	filtered.push_back(provider_model_id);
	return filtered;
}

static std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);
	ChatSession effective_chat = chat;

	if (!effective_chat.uses_native_session)
	{
		const std::string resume_id = ResolveResumeSessionIdForChat(app, chat);

		if (!resume_id.empty())
		{
			effective_chat.uses_native_session = true;
			effective_chat.native_session_id = resume_id;
		}
	}

	std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(provider, effective_chat, app.settings);

	if (ProviderUsesOpenCodeLocalBridge(provider))
	{
		std::string selected_model = Trim(app.opencode_bridge.selected_model);

		if (selected_model.empty())
		{
			selected_model = Trim(app.settings.selected_model_id);
		}

		if (!selected_model.empty())
		{
			std::string provider_model_id = selected_model;

			if (provider_model_id.rfind("uam_local/", 0) != 0)
			{
				provider_model_id = "uam_local/" + provider_model_id;
			}

			argv = ForceOpenCodeModelFlag(std::move(argv), provider_model_id);
		}
	}

	return argv;
}

static bool StartCliTerminalForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const int rows, const int cols);

static bool SendPromptToCliRuntime(AppState& app, ChatSession& chat, const std::string& prompt, std::string* error_out = nullptr)
{
	const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);

	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		if (error_out != nullptr)
		{
			*error_out = ProviderRuntime::DisabledReason(provider);

			if (error_out->empty())
			{
				*error_out = "Selected provider runtime is disabled in this build.";
			}
		}

		return false;
	}

	if (ProviderUsesOpenCodeLocalBridge(provider))
	{
		if (!EnsureOpenCodeBridgeRunning(app, error_out))
		{
			if (error_out != nullptr && error_out->empty())
			{
				*error_out = "Failed to start OpenCode bridge.";
			}

			return false;
		}
	}

	CliTerminalState& terminal = EnsureCliTerminalForChat(app, chat);

	if (!terminal.running)
	{
		if (!StartCliTerminalForChat(app, terminal, chat, 30, 120))
		{
			if (error_out != nullptr)
			{
				*error_out = terminal.last_error.empty() ? "Failed to start provider terminal." : terminal.last_error;
			}

			return false;
		}
	}

	QueueStructuredPromptForTerminal(terminal, prompt);

	if (!terminal.input_ready)
	{
		return true;
	}

	if (!FlushQueuedStructuredPromptsForTerminal(terminal, error_out))
	{
		if (error_out != nullptr && error_out->empty())
		{
			*error_out = "Failed to flush queued prompt(s) to provider terminal.";
		}

		return false;
	}

	return true;
}

static bool StartCliTerminalForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const int rows, const int cols)
{
	StopCliTerminal(terminal);
	const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);

	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		terminal.last_error = ProviderRuntime::DisabledReason(provider);

		if (terminal.last_error.empty())
		{
			terminal.last_error = "Selected provider runtime is disabled in this build.";
		}

		return false;
	}

	if (!ProviderRuntime::UsesCliOutput(provider))
	{
		terminal.last_error = "Selected provider is fixed to structured output.";
		return false;
	}

	if (ProviderRuntime::UsesInternalEngine(provider))
	{
		terminal.last_error = "Active provider does not support terminal mode.";
		return false;
	}

	if (!provider.supports_interactive)
	{
		terminal.last_error = "Active provider does not expose an interactive runtime command.";
		return false;
	}

	if (ProviderUsesOpenCodeLocalBridge(provider))
	{
		if (!EnsureSelectedLocalRuntimeModelForProvider(app))
		{
			terminal.last_error = "Select a local runtime model to continue.";
			return false;
		}

		std::string bridge_error;

		if (!EnsureOpenCodeBridgeRunning(app, &bridge_error))
		{
			terminal.last_error = bridge_error.empty() ? "Failed to start OpenCode bridge." : bridge_error;
			return false;
		}
	}

	if (ChatUsesGeminiHistory(app, chat))
	{
		RefreshGeminiChatsDir(app);
	}

	std::string template_status;
	std::string bootstrap_prompt;
	const TemplatePreflightOutcome template_outcome = PreflightWorkspaceTemplateForChat(app, provider, chat, &bootstrap_prompt, &template_status);

	if (template_outcome == TemplatePreflightOutcome::BlockingError)
	{
		terminal.last_error = template_status.empty() ? "Prompt profile preflight failed." : template_status;
		return false;
	}

	if (template_outcome == TemplatePreflightOutcome::ReadyWithoutTemplate && !template_status.empty())
	{
		app.status_line = template_status;
	}

	if (BuildProviderInteractiveArgv(app, chat).empty())
	{
		terminal.last_error = "Active provider does not expose an interactive CLI command.";
		return false;
	}

	terminal.rows = std::max(8, rows);
	terminal.cols = std::max(20, cols);
	terminal.attached_chat_id = chat.id;
	terminal.attached_session_id = ResolveResumeSessionIdForChat(app, chat);
	terminal.linked_files_snapshot = chat.linked_files;

	if (ChatUsesGeminiHistory(app, chat))
	{
		terminal.session_ids_before = SessionIdsFromChats(LoadNativeGeminiChats(app.gemini_chats_dir, provider));
	}
	else
	{
		terminal.session_ids_before.clear();
	}

	terminal.last_error.clear();
	terminal.last_sync_time_s = ImGui::GetTime();
	terminal.last_output_time_s = 0.0;
	terminal.last_activity_time_s = ImGui::GetTime();
	terminal.last_polled_time_s = 0.0;
	terminal.input_ready = false;
	terminal.startup_time_s = ImGui::GetTime();
	terminal.pending_structured_prompts.clear();
	terminal.generation_in_progress = false;

	terminal.vt = vterm_new(terminal.rows, terminal.cols);

	if (terminal.vt == nullptr)
	{
		terminal.last_error = "Failed to initialize libvterm.";
		StopCliTerminal(terminal, false);
		return false;
	}

	vterm_set_utf8(terminal.vt, 1);
	terminal.screen = vterm_obtain_screen(terminal.vt);
	terminal.state = vterm_obtain_state(terminal.vt);
	vterm_screen_set_callbacks(terminal.screen, &kVTermScreenCallbacks, &terminal);
	vterm_screen_set_damage_merge(terminal.screen, VTERM_DAMAGE_CELL);
	vterm_output_set_callback(terminal.vt, WriteBytesToPty, &terminal);
	vterm_screen_reset(terminal.screen, 1);

	if (!StartCliTerminalPlatform(app, terminal, chat))
	{
		terminal.last_error = terminal.last_error.empty() ? "Failed to start provider terminal." : terminal.last_error;
		StopCliTerminal(terminal, false);
		return false;
	}

	terminal.running = true;
	terminal.should_launch = false;
	terminal.needs_full_refresh = true;

	if (!chat.gemini_md_bootstrapped && chat.messages.empty() && template_outcome == TemplatePreflightOutcome::ReadyWithTemplate)
	{
		if (!bootstrap_prompt.empty())
		{
			std::string bootstrap_command = bootstrap_prompt + "\n";
			WriteToCliTerminal(terminal, bootstrap_command.c_str(), bootstrap_command.size());
		}

		const int chat_index = FindChatIndexById(app, chat.id);

		if (chat_index >= 0)
		{
			app.chats[chat_index].gemini_md_bootstrapped = true;
			SaveChat(app, app.chats[chat_index]);
		}
	}

	return true;
}
