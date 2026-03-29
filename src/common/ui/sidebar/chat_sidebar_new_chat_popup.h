#pragma once

/// <summary>
/// Draws the create-chat modal used to lock provider at chat creation time.
/// </summary>
static void DrawSidebarNewChatPopup(AppState& app) {
  if (app.open_new_chat_popup) {
    app.pending_new_chat_provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);
    ImGui::OpenPopup("new_chat_popup");
    app.open_new_chat_popup = false;
  }

  if (!BeginCenteredPopupModal("New Chat###new_chat_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  EnsureNewChatFolderSelection(app);
  const std::string provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);
  app.pending_new_chat_provider_id = provider_id;

  std::string folder_label = "General";
  if (const ChatFolder* folder = FindFolderById(app, FolderForNewChat(app)); folder != nullptr) {
    folder_label = FolderTitleOrFallback(*folder);
  }

  ImGui::TextColored(ui::kTextPrimary, "New Chat");
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
  ImGui::TextColored(ui::kTextMuted, "Folder: %s", folder_label.c_str());
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

  const ProviderProfile* selected_profile = ProviderProfileStore::FindById(app.provider_profiles, provider_id);
  std::string selected_label = "(none)";
  if (selected_profile != nullptr) {
    selected_label = (selected_profile->title.empty() ? selected_profile->id : selected_profile->title) +
                     std::string(ProviderRuntime::UsesCliOutput(*selected_profile) ? " [CLI]" : " [Structured]");
  }

  if (ImGui::BeginCombo("Provider", selected_label.c_str())) {
    for (const ProviderProfile& profile : app.provider_profiles) {
      if (!ShouldShowProviderProfileInUi(profile)) {
        continue;
      }
      const std::string profile_label = (profile.title.empty() ? profile.id : profile.title) +
                                        std::string(ProviderRuntime::UsesCliOutput(profile) ? " [CLI]" : " [Structured]");
      const bool selected = (provider_id == profile.id);
      if (ImGui::Selectable(profile_label.c_str(), selected)) {
        app.pending_new_chat_provider_id = profile.id;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  if (DrawButton("Create", ImVec2(96.0f, 32.0f), ButtonKind::Primary)) {
    if (ConfirmCreateNewChat(app)) {
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
    app.pending_new_chat_provider_id.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}
