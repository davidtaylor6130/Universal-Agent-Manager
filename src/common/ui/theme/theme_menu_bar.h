#pragma once

/// <summary>
/// Desktop menu bar rendering and command dispatch.
/// </summary>
static void DrawDesktopMenuBar(AppState& app, bool& done) {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("About Universal Agent Manager")) {
      app.open_about_popup = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("New Chat", "Ctrl+N")) {
      CreateAndSelectChat(app);
    }
    if (ImGui::MenuItem("Refresh Chats", "Ctrl+R")) {
      RefreshChatHistory(app);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("App Settings...", "Ctrl+,")) {
      app.open_app_settings_popup = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
      done = true;
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    ChatSession* selected = SelectedChat(app);
    const bool can_delete = (selected != nullptr);
    if (!can_delete) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Delete Selected Chat")) {
      RequestDeleteSelectedChat(app);
    }
    if (!can_delete) {
      ImGui::EndDisabled();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Templates")) {
    RefreshTemplateCatalog(app);
    if (ImGui::BeginMenu("Default Template")) {
      const bool none_selected = Trim(app.settings.default_gemini_template_id).empty();
      if (ImGui::MenuItem("None", nullptr, none_selected)) {
        app.settings.default_gemini_template_id.clear();
        SaveSettings(app);
        app.status_line = "Default Gemini template cleared.";
      }
      ImGui::Separator();
      for (const TemplateCatalogEntry& entry : app.template_catalog) {
        const bool selected = (app.settings.default_gemini_template_id == entry.id);
        if (ImGui::MenuItem(entry.display_name.c_str(), nullptr, selected)) {
          app.settings.default_gemini_template_id = entry.id;
          SaveSettings(app);
          app.status_line = "Default Gemini template set to " + entry.display_name + ".";
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Manage Templates...")) {
      app.open_template_manager_popup = true;
    }
    if (ImGui::MenuItem("Open Template Folder")) {
      std::string error;
      const fs::path template_path = GeminiTemplateCatalog::CatalogPath(ResolveGeminiGlobalRootPath(app.settings));
      if (!OpenFolderInFileManager(template_path, &error)) {
        app.status_line = error;
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("RAG")) {
    const ChatSession* selected_chat = SelectedChat(app);
    const bool has_selected_chat = (selected_chat != nullptr);
    const fs::path fallback_source_root = ResolveCurrentRagFallbackSourceRoot(app);
    if (ImGui::MenuItem("Open RAG Console...")) {
      app.open_rag_console_popup = true;
    }
    ImGui::Separator();
    if (!has_selected_chat) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("Use Selected Chat Workspace As Source")) {
      app.settings.rag_project_source_directory = ResolveWorkspaceRootPath(app, *selected_chat).string();
      SaveSettings(app);
      app.status_line = "RAG source directory set from selected chat workspace.";
      AppendRagScanReport(app, "Source directory set from selected chat workspace.");
    }
    if (!has_selected_chat) {
      ImGui::EndDisabled();
    }
    if (ImGui::MenuItem("Clear Project Source")) {
      app.settings.rag_project_source_directory.clear();
      SaveSettings(app);
      app.status_line = "RAG source directory cleared.";
      AppendRagScanReport(app, "Source directory cleared.");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rebuild Index")) {
      std::string scan_error;
      if (!TriggerProjectRagScan(app, false, fallback_source_root, &scan_error)) {
        app.status_line = "RAG scan failed to start: " + scan_error;
      } else {
        app.status_line = "RAG scan started.";
      }
    }
    if (ImGui::MenuItem("Rescan Previous Source")) {
      std::string scan_error;
      if (!TriggerProjectRagScan(app, true, fallback_source_root, &scan_error)) {
        app.status_line = "RAG rescan failed to start: " + scan_error;
      } else {
        app.status_line = "RAG rescan started (previous source).";
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    const bool structured_active = (app.center_view_mode == CenterViewMode::Structured);
    if (ImGui::MenuItem("Structured Mode", nullptr, structured_active)) {
      app.center_view_mode = CenterViewMode::Structured;
      SaveSettings(app);
    }
    const bool terminal_active = (app.center_view_mode == CenterViewMode::CliConsole);
    if (ImGui::MenuItem("Terminal Mode", nullptr, terminal_active)) {
      app.center_view_mode = CenterViewMode::CliConsole;
      MarkSelectedCliTerminalForLaunch(app);
      SaveSettings(app);
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Theme")) {
      const auto set_theme = [&](const char* choice) {
        app.settings.ui_theme = choice;
        ApplyThemeFromSettings(app);
        SaveSettings(app);
      };
      if (ImGui::MenuItem("Dark", nullptr, app.settings.ui_theme == "dark")) {
        set_theme("dark");
      }
      if (ImGui::MenuItem("Light", nullptr, app.settings.ui_theme == "light")) {
        set_theme("light");
      }
      if (ImGui::MenuItem("System", nullptr, app.settings.ui_theme == "system")) {
        set_theme("system");
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}
