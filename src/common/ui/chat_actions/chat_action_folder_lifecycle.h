#pragma once

/// <summary>
/// Folder deletion, chat creation, and delete-request dispatch actions.
/// </summary>
static bool DeleteFolderById(AppState& app, const std::string& folder_id) {
  if (folder_id.empty() || folder_id == kDefaultFolderId) {
    app.status_line = "The default folder cannot be deleted.";
    return false;
  }
  const int folder_index = FindFolderIndexById(app, folder_id);
  if (folder_index < 0) {
    app.status_line = "Folder no longer exists.";
    return false;
  }

  bool all_chat_saves_ok = true;
  int moved_chat_count = 0;
  for (ChatSession& existing_chat : app.chats) {
    if (existing_chat.folder_id != folder_id) {
      continue;
    }
    existing_chat.folder_id = kDefaultFolderId;
    existing_chat.updated_at = TimestampNow();
    if (!SaveChat(app, existing_chat)) {
      all_chat_saves_ok = false;
    }
    ++moved_chat_count;
  }

  app.folders.erase(app.folders.begin() + folder_index);
  EnsureDefaultFolder(app);
  if (app.new_chat_folder_id == folder_id) {
    app.new_chat_folder_id = kDefaultFolderId;
  }
  SaveFolders(app);
  if (all_chat_saves_ok) {
    app.status_line = "Folder deleted. Moved " + std::to_string(moved_chat_count) + " chat(s) to General.";
  } else {
    app.status_line = "Folder deleted, but failed to persist one or more moved chats.";
  }
  return true;
}

static void CreateAndSelectChat(AppState& app) {
  ChatSession chat = CreateNewChat(FolderForNewChat(app));
  const std::string id = chat.id;
  app.chats.push_back(chat);
  NormalizeChatBranchMetadata(app);
  SortChatsByRecent(app.chats);
  SelectChatById(app, id);
  SaveSettings(app);
  if (app.center_view_mode == CenterViewMode::CliConsole) {
    MarkSelectedCliTerminalForLaunch(app);
  }
  if (!SaveChat(app, app.chats[app.selected_chat_index])) {
    app.status_line = "Created chat in memory, but failed to persist.";
  } else {
    app.status_line = "New chat created.";
  }
}

static void RequestDeleteSelectedChat(AppState& app) {
  const ChatSession* chat = SelectedChat(app);
  if (chat == nullptr) {
    app.status_line = "Select a chat to delete.";
    return;
  }
  if (app.settings.confirm_delete_chat) {
    app.pending_delete_chat_id = chat->id;
    app.open_delete_chat_popup = true;
    return;
  }
  RemoveChatById(app, chat->id);
}

static void RequestDeleteFolder(AppState& app, const std::string& folder_id) {
  if (folder_id.empty()) {
    app.status_line = "No folder selected.";
    return;
  }
  if (app.settings.confirm_delete_folder) {
    app.pending_delete_folder_id = folder_id;
    app.open_delete_folder_popup = true;
    return;
  }
  DeleteFolderById(app, folder_id);
}
