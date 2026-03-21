#pragma once

/// <summary>
/// Draws repository metadata and SVN actions in the chat settings side pane.
/// </summary>
static void DrawChatSettingsRepositoryCard(AppState& app, ChatSession& chat) {
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
  DrawSectionHeader("Repository");
  if (BeginSectionCard("repository_card")) {
    const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
    RefreshWorkspaceVcsSnapshot(app, workspace_root, false);
    const std::string workspace_key = workspace_root.lexically_normal().generic_string();
    VcsSnapshot snapshot;
    if (const auto it = app.vcs_snapshot_by_workspace.find(workspace_key); it != app.vcs_snapshot_by_workspace.end()) {
      snapshot = it->second;
    } else {
      snapshot.working_copy_root = workspace_key;
    }

    const char* repo_type_label = (snapshot.repo_type == VcsRepoType::Svn) ? "SVN" : "None";
    ImGui::TextColored(ui::kTextMuted, "Repository type");
    ImGui::TextColored(ui::kTextPrimary, "%s", repo_type_label);
    ImGui::TextColored(ui::kTextMuted, "Workspace");
    ImGui::TextWrapped("%s", workspace_key.c_str());
    if (snapshot.repo_type == VcsRepoType::Svn) {
      ImGui::TextColored(ui::kTextMuted, "Working copy root");
      ImGui::TextWrapped("%s", snapshot.working_copy_root.c_str());
      ImGui::TextColored(ui::kTextMuted, "Revision");
      ImGui::TextWrapped("%s", snapshot.revision.empty() ? "(unknown)" : snapshot.revision.c_str());
      ImGui::TextColored(ui::kTextMuted, "Branch path");
      ImGui::TextWrapped("%s", snapshot.branch_path.empty() ? "(unknown)" : snapshot.branch_path.c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

    if (DrawButton("Refresh", ImVec2(90.0f, 30.0f), ButtonKind::Ghost)) {
      if (RefreshWorkspaceVcsSnapshot(app, workspace_root, true)) {
        app.status_line = "Repository snapshot refreshed.";
      }
    }
    const bool enable_svn_actions = (snapshot.repo_type == VcsRepoType::Svn);
    if (!enable_svn_actions) {
      ImGui::BeginDisabled();
    }
    ImGui::SameLine();
    if (DrawButton("Status", ImVec2(90.0f, 30.0f), ButtonKind::Ghost)) {
      const VcsCommandResult result = VcsWorkspaceService::ReadStatus(workspace_root);
      ShowVcsCommandOutput(app, "SVN Status", result);
      app.status_line = result.ok ? "SVN status loaded." : "SVN status command failed.";
    }
    ImGui::SameLine();
    if (DrawButton("Diff", ImVec2(90.0f, 30.0f), ButtonKind::Ghost)) {
      const VcsCommandResult result = VcsWorkspaceService::ReadDiff(workspace_root);
      ShowVcsCommandOutput(app, "SVN Diff", result);
      app.status_line = result.ok ? "SVN diff loaded." : "SVN diff command failed.";
    }
    ImGui::SameLine();
    if (DrawButton("Log", ImVec2(90.0f, 30.0f), ButtonKind::Ghost)) {
      const VcsCommandResult result = VcsWorkspaceService::ReadLog(workspace_root);
      ShowVcsCommandOutput(app, "SVN Log", result);
      app.status_line = result.ok ? "SVN log loaded." : "SVN log command failed.";
    }
    if (!enable_svn_actions) {
      ImGui::EndDisabled();
    }
  }
  EndPanel();
}
