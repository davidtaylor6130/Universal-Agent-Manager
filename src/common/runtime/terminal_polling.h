#pragma once

static std::string CodepointToUtf8(const uint32_t cp) {
  std::string out;
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

static void ResizeCliTerminal(CliTerminalState& terminal, const int rows, const int cols) {
  const int safe_rows = std::max(8, rows);
  const int safe_cols = std::max(20, cols);
  if (terminal.rows == safe_rows && terminal.cols == safe_cols) {
    return;
  }
  terminal.rows = safe_rows;
  terminal.cols = safe_cols;
  if (CliTerminalRuntimeReady(terminal)) {
    vterm_set_size(terminal.vt, terminal.rows, terminal.cols);
  }
#if defined(__unix__) || defined(__APPLE__)
  if (terminal.master_fd >= 0) {
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(terminal.rows);
    ws.ws_col = static_cast<unsigned short>(terminal.cols);
    ioctl(terminal.master_fd, TIOCSWINSZ, &ws);
  }
#elif defined(_WIN32)
  if (terminal.pseudo_console != nullptr) {
    COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
    ResizePseudoConsoleSafe(terminal.pseudo_console, size);
  }
#endif
}

#if defined(_WIN32)
static std::ptrdiff_t ReadCliTerminalOutput(CliTerminalState& terminal, char* buffer, const size_t buffer_size) {
  if (terminal.pipe_output == INVALID_HANDLE_VALUE) {
    return -1;
  }
  DWORD available = 0;
  if (!PeekNamedPipe(terminal.pipe_output, nullptr, 0, nullptr, &available, nullptr)) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE) {
      return 0;
    }
    return -1;
  }
  if (available == 0) {
    return -2;
  }
  const DWORD to_read = static_cast<DWORD>(std::min<size_t>(buffer_size, available));
  DWORD bytes_read = 0;
  if (!ReadFile(terminal.pipe_output, buffer, to_read, &bytes_read, nullptr)) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE) {
      return 0;
    }
    return -1;
  }
  if (bytes_read == 0) {
    return -2;
  }
  return static_cast<std::ptrdiff_t>(bytes_read);
}
#endif

static void PollCliTerminal(AppState& app, CliTerminalState& terminal, const bool preserve_selection) {
  constexpr double kGenerationIdleSeconds = 1.15;
  constexpr double kStructuredInputReadyFallbackSeconds = 1.5;
  constexpr int kReadBudgetChunksPerTick = 72;
  constexpr std::size_t kReadBudgetBytesPerTick = 512 * 1024;
  const std::string selected_chat_id = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  const int terminal_chat_index = FindChatIndexById(app, terminal.attached_chat_id);
  const ChatSession* terminal_chat = (terminal_chat_index >= 0) ? &app.chats[terminal_chat_index] : nullptr;
  const bool terminal_uses_gemini_history = (terminal_chat != nullptr) && ChatUsesGeminiHistory(app, *terminal_chat);
  const ProviderProfile terminal_provider = (terminal_chat != nullptr)
                                                ? ProviderForChatOrDefault(app, *terminal_chat)
                                                : ActiveProviderOrDefault(app);
  const auto mark_unseen_if_background = [&]() {
    if (!terminal.attached_chat_id.empty() && terminal.attached_chat_id != selected_chat_id) {
      MarkChatUnseen(app, terminal.attached_chat_id);
    }
  };
#if defined(_WIN32)
  if (!terminal.running || terminal.pipe_output == INVALID_HANDLE_VALUE) {
    return;
  }
  if (!CliTerminalRuntimeReady(terminal)) {
    ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal VT state became invalid.");
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    terminal.last_error = "Provider terminal state became invalid. Relaunch the terminal.";
    app.status_line = terminal.last_error;
    return;
  }
  char buffer[8192];
  int chunks_read = 0;
  std::size_t bytes_read_total = 0;
  while (true) {
    if (chunks_read >= kReadBudgetChunksPerTick || bytes_read_total >= kReadBudgetBytesPerTick) {
      break;
    }
    const std::ptrdiff_t read_bytes = ReadCliTerminalOutput(terminal, buffer, sizeof(buffer));
    if (read_bytes > 0) {
      ++chunks_read;
      bytes_read_total += static_cast<std::size_t>(read_bytes);
      terminal.input_ready = true;
      terminal.last_output_time_s = ImGui::GetTime();
      terminal.last_activity_time_s = terminal.last_output_time_s;
      terminal.generation_in_progress = true;
      vterm_input_write(terminal.vt, buffer, static_cast<std::size_t>(read_bytes));
      terminal.needs_full_refresh = true;
      continue;
    }
    if (read_bytes == 0) {
      mark_unseen_if_background();
      ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal exited before input was ready.");
      terminal.last_error = "Provider terminal exited.";
      StopCliTerminal(terminal);
      terminal.should_launch = false;
      app.status_line = terminal.last_error;
      break;
    }
    if (read_bytes == -2) {
      break;
    }
    ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal read failed before input was ready.");
    terminal.last_error = "Provider terminal read failed.";
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    app.status_line = terminal.last_error;
    break;
  }

  if (terminal.process_info.hProcess != INVALID_HANDLE_VALUE &&
      WaitForSingleObject(terminal.process_info.hProcess, 0) == WAIT_OBJECT_0) {
    mark_unseen_if_background();
    ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal exited before input was ready.");
    terminal.last_error = "Provider terminal exited.";
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    app.status_line = terminal.last_error;
  }
#else
  if (!terminal.running || terminal.master_fd < 0) {
    return;
  }
  if (!CliTerminalRuntimeReady(terminal)) {
    ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal VT state became invalid.");
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    terminal.last_error = "Provider terminal state became invalid. Relaunch the terminal.";
    app.status_line = terminal.last_error;
    return;
  }

  char buffer[8192];
  int chunks_read = 0;
  std::size_t bytes_read_total = 0;
  while (true) {
    if (chunks_read >= kReadBudgetChunksPerTick || bytes_read_total >= kReadBudgetBytesPerTick) {
      break;
    }
    const ssize_t read_bytes = read(terminal.master_fd, buffer, sizeof(buffer));
    if (read_bytes > 0) {
      ++chunks_read;
      bytes_read_total += static_cast<std::size_t>(read_bytes);
      terminal.input_ready = true;
      terminal.last_output_time_s = ImGui::GetTime();
      terminal.last_activity_time_s = terminal.last_output_time_s;
      terminal.generation_in_progress = true;
      vterm_input_write(terminal.vt, buffer, static_cast<std::size_t>(read_bytes));
      terminal.needs_full_refresh = true;
      continue;
    }
    if (read_bytes == 0) {
      mark_unseen_if_background();
      ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal exited before input was ready.");
      terminal.last_error = "Provider terminal exited.";
      StopCliTerminal(terminal);
      terminal.should_launch = false;
      app.status_line = terminal.last_error;
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal read failed before input was ready.");
    terminal.last_error = "Provider terminal read failed.";
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    app.status_line = terminal.last_error;
    break;
  }

  int status = 0;
  if (terminal.child_pid > 0 && waitpid(terminal.child_pid, &status, WNOHANG) > 0) {
    mark_unseen_if_background();
    ReportDroppedQueuedStructuredPromptsForTerminal(app, terminal, "Provider terminal exited before input was ready.");
    terminal.last_error = "Provider terminal exited.";
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    app.status_line = terminal.last_error;
  }
#endif

  const double now = ImGui::GetTime();
  if (terminal.running &&
      !terminal.input_ready &&
      terminal.startup_time_s > 0.0 &&
      (now - terminal.startup_time_s) >= kStructuredInputReadyFallbackSeconds) {
    terminal.input_ready = true;
  }
  if (terminal.running && terminal.input_ready && !terminal.pending_structured_prompts.empty()) {
    std::string flush_error;
    if (!FlushQueuedStructuredPromptsForTerminal(terminal, &flush_error)) {
      app.status_line =
          "Provider terminal prompt flush failed: " + (flush_error.empty() ? std::string("unknown error.") : flush_error);
    }
  }
  if (now - terminal.last_sync_time_s > 1.25) {
    terminal.last_sync_time_s = now;
    if (terminal_uses_gemini_history) {
#if defined(_WIN32)
      // Windows-only: run native session scanning off the UI thread to prevent
      // this port from freezing when a large or malformed chat JSON appears.
      // macOS is currently stable with synchronous loading; if it starts to show
      // the same behavior, we can make this path universal.
      std::vector<ChatSession> native_now;
      std::string native_load_error;
      const bool has_loaded_snapshot = TryConsumeAsyncNativeChatLoad(app, native_now, native_load_error);
      StartAsyncNativeChatLoad(app);

      if (!native_load_error.empty()) {
        app.status_line = "Native chat refresh failed: " + native_load_error;
      }
      if (has_loaded_snapshot && native_load_error.empty()) {
        if (terminal.attached_session_id.empty()) {
          const std::vector<std::string> candidates = CollectNewSessionIds(native_now, terminal.session_ids_before);
          std::unordered_set<std::string> blocked_ids;
          for (const auto& other_terminal : app.cli_terminals) {
            if (other_terminal == nullptr || other_terminal.get() == &terminal || other_terminal->attached_session_id.empty()) {
              continue;
            }
            blocked_ids.insert(other_terminal->attached_session_id);
          }
          for (const auto& resolved : app.resolved_native_sessions_by_chat_id) {
            if (!resolved.second.empty()) {
              blocked_ids.insert(resolved.second);
            }
          }
          const std::string discovered = PickFirstUnblockedSessionId(candidates, blocked_ids);
          if (!discovered.empty()) {
            const std::string previous_chat_id = terminal.attached_chat_id;
            const int previous_chat_index = FindChatIndexById(app, previous_chat_id);
            if (previous_chat_index >= 0 &&
                IsLocalDraftChatId(previous_chat_id) &&
                !app.chats[previous_chat_index].uses_native_session) {
              PersistLocalDraftNativeSessionLink(app, app.chats[previous_chat_index], discovered);
            }
            terminal.attached_session_id = discovered;
            terminal.attached_chat_id = discovered;
            if (!previous_chat_id.empty()) {
              app.resolved_native_sessions_by_chat_id[previous_chat_id] = discovered;
            }
            app.chats.erase(std::remove_if(app.chats.begin(), app.chats.end(),
                                           [&](const ChatSession& c) {
                                             return !c.uses_native_session && c.id == previous_chat_id;
                                           }),
                            app.chats.end());
          }
        }
        const std::string preferred_id =
            terminal.attached_session_id.empty() ? terminal.attached_chat_id : terminal.attached_session_id;
        SyncChatsFromLoadedNative(app, std::move(native_now), preferred_id, preserve_selection);
      }
#else
      RefreshGeminiChatsDir(app);
      const std::vector<ChatSession> native_now = LoadNativeGeminiChats(app.gemini_chats_dir, terminal_provider);
      if (terminal.attached_session_id.empty()) {
        const std::vector<std::string> candidates = CollectNewSessionIds(native_now, terminal.session_ids_before);
        std::unordered_set<std::string> blocked_ids;
        for (const auto& other_terminal : app.cli_terminals) {
          if (other_terminal == nullptr || other_terminal.get() == &terminal || other_terminal->attached_session_id.empty()) {
            continue;
          }
          blocked_ids.insert(other_terminal->attached_session_id);
        }
        for (const auto& resolved : app.resolved_native_sessions_by_chat_id) {
          if (!resolved.second.empty()) {
            blocked_ids.insert(resolved.second);
          }
        }
        const std::string discovered = PickFirstUnblockedSessionId(candidates, blocked_ids);
        if (!discovered.empty()) {
          const std::string previous_chat_id = terminal.attached_chat_id;
          const int previous_chat_index = FindChatIndexById(app, previous_chat_id);
          if (previous_chat_index >= 0 &&
              IsLocalDraftChatId(previous_chat_id) &&
              !app.chats[previous_chat_index].uses_native_session) {
            PersistLocalDraftNativeSessionLink(app, app.chats[previous_chat_index], discovered);
          }
          terminal.attached_session_id = discovered;
          terminal.attached_chat_id = discovered;
          if (!previous_chat_id.empty()) {
            app.resolved_native_sessions_by_chat_id[previous_chat_id] = discovered;
          }
          app.chats.erase(std::remove_if(app.chats.begin(), app.chats.end(),
                                         [&](const ChatSession& c) {
                                           return !c.uses_native_session && c.id == previous_chat_id;
                                         }),
                          app.chats.end());
        }
      }
      std::string preferred_id = terminal.attached_session_id.empty() ? terminal.attached_chat_id : terminal.attached_session_id;
      SyncChatsFromNative(app, preferred_id, preserve_selection);
#endif
    } else {
      SyncChatsFromNative(app, terminal.attached_chat_id, preserve_selection);
    }
  }

  if (terminal.running &&
      terminal.generation_in_progress &&
      terminal.last_output_time_s > 0.0 &&
      (now - terminal.last_output_time_s) > kGenerationIdleSeconds) {
    terminal.generation_in_progress = false;
    mark_unseen_if_background();
  }
}

static void RefreshChatHistory(AppState& app) {
  const std::string selected_id = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  SyncChatsFromNative(app, selected_id, true);
  app.status_line = "Chat history refreshed.";
}

static void PollAllCliTerminals(AppState& app) {
  const std::string selected_chat_id = (SelectedChat(app) != nullptr) ? SelectedChat(app)->id : "";
  const double now = ImGui::GetTime();
  for (auto& terminal : app.cli_terminals) {
    if (terminal == nullptr) {
      continue;
    }
    const bool selected_terminal = (!selected_chat_id.empty() && terminal->attached_chat_id == selected_chat_id);
    const double min_poll_interval_s = selected_terminal ? 0.0 : 0.08;
    if (terminal->last_polled_time_s > 0.0 && (now - terminal->last_polled_time_s) < min_poll_interval_s) {
      continue;
    }
    terminal->last_polled_time_s = now;
    const bool preserve_selection = !selected_chat_id.empty() && terminal->attached_chat_id != selected_chat_id;
    PollCliTerminal(app, *terminal, preserve_selection);
    if (!terminal->running || selected_terminal || terminal->attached_chat_id.empty()) {
      continue;
    }
    if (HasPendingCallForChat(app, terminal->attached_chat_id) || terminal->generation_in_progress) {
      continue;
    }
    const double terminal_activity = std::max({terminal->last_activity_time_s, terminal->last_output_time_s, terminal->last_sync_time_s});
    const double idle_timeout = static_cast<double>(std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600));
    if (terminal_activity > 0.0 && (now - terminal_activity) > idle_timeout) {
      ReportDroppedQueuedStructuredPromptsForTerminal(
          app, *terminal, "Provider terminal stopped due to idle timeout before queued prompt delivery.");
      StopCliTerminal(*terminal, false);
      terminal->should_launch = false;
      app.status_line = "Stopped idle background terminal for chat " + CompactPreview(terminal->attached_chat_id, 36) + ".";
    }
  }
}

static void StartGeminiRequest(AppState& app) {
  ChatSession* chat = SelectedChat(app);
  if (chat == nullptr) {
    app.status_line = "Select or create a chat first.";
    return;
  }

  const std::string prompt_text = Trim(app.composer_text);
  if (QueueGeminiPromptForChat(app, *chat, prompt_text, false)) {
    app.composer_text.clear();
  }
}
