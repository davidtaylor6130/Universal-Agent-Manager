#pragma once

/// <summary>
/// Draws the sidebar chat context menu popup.
/// </summary>
static void DrawSidebarChatOptionsPopup(AppState& app) {
  if (app.open_sidebar_chat_options_popup && !app.sidebar_chat_options_popup_chat_id.empty()) {
    ImGui::OpenPopup("sidebar_chat_options_popup");
    app.open_sidebar_chat_options_popup = false;
  }
  if (!ImGui::BeginPopup("sidebar_chat_options_popup")) {
    return;
  }

  const int chat_index = FindChatIndexById(app, app.sidebar_chat_options_popup_chat_id);
  if (chat_index < 0) {
    ImGui::TextColored(ui::kTextMuted, "Chat no longer exists.");
    ImGui::EndPopup();
    return;
  }

  ChatSession& popup_chat = app.chats[chat_index];
  const auto ensure_selected_chat = [&]() {
    if (app.selected_chat_index != chat_index) {
      SelectChatById(app, popup_chat.id);
      SaveSettings(app);
    }
  };

  if (ImGui::BeginMenu("View Mode")) {
    if (ImGui::MenuItem("Structured", nullptr, app.center_view_mode == CenterViewMode::Structured)) {
      ensure_selected_chat();
      app.center_view_mode = CenterViewMode::Structured;
      SaveSettings(app);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Terminal", nullptr, app.center_view_mode == CenterViewMode::CliConsole)) {
      ensure_selected_chat();
      app.center_view_mode = CenterViewMode::CliConsole;
      MarkSelectedCliTerminalForLaunch(app);
      SaveSettings(app);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Repository")) {
    const fs::path workspace_root = ResolveWorkspaceRootPath(app, popup_chat);
    RefreshWorkspaceVcsSnapshot(app, workspace_root, false);
    const std::string workspace_key = workspace_root.lexically_normal().generic_string();
    VcsSnapshot snapshot;
    if (const auto it = app.vcs_snapshot_by_workspace.find(workspace_key); it != app.vcs_snapshot_by_workspace.end()) {
      snapshot = it->second;
    }
    const bool is_svn = (snapshot.repo_type == VcsRepoType::Svn);

    if (ImGui::MenuItem("Refresh")) {
      RefreshWorkspaceVcsSnapshot(app, workspace_root, true);
      ImGui::CloseCurrentPopup();
    }
    if (!is_svn) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Status")) {
      const VcsCommandResult result = VcsWorkspaceService::ReadStatus(workspace_root);
      ShowVcsCommandOutput(app, "SVN Status", result);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Diff")) {
      const VcsCommandResult result = VcsWorkspaceService::ReadDiff(workspace_root);
      ShowVcsCommandOutput(app, "SVN Diff", result);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Log")) {
      const VcsCommandResult result = VcsWorkspaceService::ReadLog(workspace_root);
      ShowVcsCommandOutput(app, "SVN Log", result);
      ImGui::CloseCurrentPopup();
    }
    if (!is_svn) {
      ImGui::EndDisabled();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("RAG")) {
    if (ImGui::MenuItem("Open RAG Console...")) {
      ensure_selected_chat();
      app.open_rag_console_popup = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rebuild Index")) {
      const fs::path workspace_root = ResolveWorkspaceRootPath(app, popup_chat);
      std::string scan_error;
      if (!TriggerProjectRagScan(app, false, workspace_root, &scan_error)) {
        app.status_line = "RAG scan failed to start: " + scan_error;
      } else {
        app.status_line = "RAG scan started.";
      }
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Rescan Previous Source")) {
      const fs::path workspace_root = ResolveWorkspaceRootPath(app, popup_chat);
      std::string scan_error;
      if (!TriggerProjectRagScan(app, true, workspace_root, &scan_error)) {
        app.status_line = "RAG rescan failed to start: " + scan_error;
      } else {
        app.status_line = "RAG rescan started (previous source).";
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndMenu();
  }
  ImGui::EndPopup();
}
