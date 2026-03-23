#pragma once

/// <summary>
/// Draws template-manager controls for global root selection and catalog folder access.
/// </summary>
static void DrawTemplateManagerGlobalRootSection(AppState& app,
                                                 const fs::path& global_root,
                                                 const fs::path& catalog_path) {
  (void)global_root;
  ImGui::TextColored(ui::kTextSecondary, "Global Root");
  PushInputChrome();
  ImGui::SetNextItemWidth(560.0f);
  ImGui::InputText("##prompt_profile_global_root", &app.settings.prompt_profile_root_path);
  PopInputChrome();
  if (DrawButton("Save Root", ImVec2(110.0f, 30.0f), ButtonKind::Ghost)) {
    if (Trim(app.settings.prompt_profile_root_path).empty()) {
      app.settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
    }
    SaveSettings(app);
    MarkTemplateCatalogDirty(app);
    RefreshTemplateCatalog(app, true);
    app.status_line = "Global prompt-profile root saved.";
  }
  ImGui::SameLine();
  if (DrawButton("Open Catalog Folder", ImVec2(148.0f, 30.0f), ButtonKind::Ghost)) {
    std::string error;
    if (!OpenFolderInFileManager(catalog_path, &error)) {
      app.status_line = error;
    }
  }
  ImGui::TextColored(ui::kTextMuted, "%s", catalog_path.string().c_str());
}
