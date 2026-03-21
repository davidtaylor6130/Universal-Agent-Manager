#pragma once

/// <summary>
/// Draws RAG configuration and rebuild controls in the chat settings side pane.
/// </summary>
static void DrawChatSettingsRagCard(AppState& app, ChatSession& chat) {
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
  DrawSectionHeader("RAG");
  if (BeginSectionCard("rag_card")) {
    const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
    const std::string workspace_key = workspace_root.lexically_normal().generic_string();
    ImGui::TextColored(ui::kTextMuted, "RAG mode");
    ImGui::TextColored(ui::kTextPrimary, "%s", app.settings.rag_enabled ? "Enabled (Structured mode)" : "Disabled");
    ImGui::TextColored(ui::kTextMuted, "Top K");
    ImGui::TextColored(ui::kTextPrimary, "%d", app.settings.rag_top_k);
    ImGui::TextColored(ui::kTextMuted, "Max snippet chars");
    ImGui::TextColored(ui::kTextPrimary, "%d", app.settings.rag_max_snippet_chars);
    ImGui::TextColored(ui::kTextMuted, "Max file bytes");
    ImGui::TextColored(ui::kTextPrimary, "%d", app.settings.rag_max_file_bytes);

    if (const auto it = app.rag_last_refresh_by_workspace.find(workspace_key); it != app.rag_last_refresh_by_workspace.end()) {
      ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
      ImGui::TextColored(ui::kTextMuted, "Last refresh");
      ImGui::TextWrapped("%s", it->second.c_str());
    }

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
    if (DrawButton("Rebuild Index", ImVec2(128.0f, 30.0f), ButtonKind::Primary)) {
      const RagRefreshResult rebuild = app.rag_index_service.RebuildIndex(workspace_root);
      if (!rebuild.ok) {
        app.rag_last_refresh_by_workspace[workspace_key] = rebuild.error;
        app.status_line = "RAG index rebuild failed: " + rebuild.error;
      } else {
        app.rag_last_refresh_by_workspace[workspace_key] =
            "Indexed files: " + std::to_string(rebuild.indexed_files) +
            ", updated: " + std::to_string(rebuild.updated_files) +
            ", removed: " + std::to_string(rebuild.removed_files);
        app.rag_last_rebuild_at_by_workspace[workspace_key] = TimestampNow();
        app.status_line = "RAG index rebuilt.";
      }
    }
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
    ImGui::TextColored(ui::kTextMuted, "Latest rebuild");
    if (const auto it = app.rag_last_rebuild_at_by_workspace.find(workspace_key);
        it != app.rag_last_rebuild_at_by_workspace.end() && !it->second.empty()) {
      ImGui::TextColored(ui::kTextPrimary, "%s", it->second.c_str());
    } else {
      ImGui::TextColored(ui::kTextMuted, "(not rebuilt yet)");
    }
  }
  EndPanel();
}
