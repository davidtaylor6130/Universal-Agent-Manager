#pragma once

/// <summary>
/// Runtime model picker modal shown when local structured runtime has no selected model.
/// </summary>
static void DrawRuntimeModelSelectionModal(AppState& app) {
  if (app.open_runtime_model_selection_popup) {
    ImGui::OpenPopup("runtime_model_selection_popup");
    app.open_runtime_model_selection_popup = false;
  }
  if (!ImGui::BeginPopupModal("runtime_model_selection_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  ImGui::TextColored(ui::kTextPrimary, "Select Runtime Model");
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
  ImGui::TextWrapped("This local runtime provider needs a model before sending prompts.");
  ImGui::TextColored(ui::kTextMuted, "Models folder directory");
  bool input_deactivated_after_edit = false;
  bool picked_with_dialog = false;
  std::string browse_error;
  const bool folder_changed = DrawPathInputWithBrowseButton(
      "##runtime_model_folder_directory",
      app.settings.models_folder_directory,
      "runtime_model_folder_picker",
      PathBrowseTarget::Directory,
      -1.0f,
      &input_deactivated_after_edit,
      &picked_with_dialog,
      &browse_error);
  if (!browse_error.empty()) {
    app.status_line = browse_error;
  }
  if (folder_changed) {
    app.settings.models_folder_directory = Trim(app.settings.models_folder_directory);
    app.loaded_runtime_model_id.clear();
  }
  if (input_deactivated_after_edit || picked_with_dialog) {
    SaveSettings(app);
  }

  const fs::path model_folder = ResolveRagModelFolder(app);
  app.local_runtime_engine.SetModelFolder(model_folder);
  const std::vector<std::string> runtime_models = app.local_runtime_engine.ListModels();
  const bool has_models = !runtime_models.empty();

  ImGui::TextColored(ui::kTextMuted, "Resolved folder: %s", model_folder.string().c_str());
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  const auto selected_it = std::find(runtime_models.begin(), runtime_models.end(), app.runtime_model_selection_id);
  if (has_models && selected_it == runtime_models.end()) {
    app.runtime_model_selection_id = runtime_models.front();
  }

  const std::string combo_preview = has_models
                                        ? (app.runtime_model_selection_id.empty() ? std::string("(select model)")
                                                                                  : app.runtime_model_selection_id)
                                        : std::string("(no models found)");
  if (ImGui::BeginCombo("Available models", combo_preview.c_str())) {
    if (!has_models) {
      ImGui::BeginDisabled();
      ImGui::Selectable("No local models found", false);
      ImGui::EndDisabled();
    } else {
      for (const std::string& model : runtime_models) {
        const bool selected = (app.runtime_model_selection_id == model);
        if (ImGui::Selectable(model.c_str(), selected)) {
          app.runtime_model_selection_id = model;
        }
      }
    }
    ImGui::EndCombo();
  }

  if (!has_models) {
    ImGui::TextColored(ui::kWarning, "No local models were found.");
    ImGui::TextWrapped("Add a model to the folder above, then retry.");
  }

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
  const bool can_load = has_models && !Trim(app.runtime_model_selection_id).empty();
  if (!can_load) {
    ImGui::BeginDisabled();
  }
  if (DrawButton("Load Model", ImVec2(106.0f, 32.0f), ButtonKind::Primary)) {
    app.settings.selected_model_id = Trim(app.runtime_model_selection_id);
    std::string load_error;
    if (!EnsureLocalRuntimeModelLoaded(app, &load_error)) {
      app.status_line = "Local runtime model load failed: " +
                        (load_error.empty() ? std::string("unknown error") : load_error);
    } else {
      SaveSettings(app);
      app.status_line = "Loaded local runtime model: " + app.settings.selected_model_id + ".";
      ImGui::CloseCurrentPopup();
    }
  }
  if (!can_load) {
    ImGui::EndDisabled();
  }
  if (!has_models) {
    ImGui::SameLine();
    if (DrawButton("Open Settings", ImVec2(116.0f, 32.0f), ButtonKind::Ghost)) {
      app.open_app_settings_popup = true;
    }
  }
  ImGui::SameLine();
  if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
    app.runtime_model_selection_id.clear();
    app.status_line = "Runtime model selection cancelled.";
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}
