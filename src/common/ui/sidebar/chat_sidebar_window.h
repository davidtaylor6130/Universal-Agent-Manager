#pragma once

#include "common/ui/sidebar/chat_sidebar_folder_header.h"
#include "common/ui/sidebar/chat_sidebar_header.h"
#include "common/ui/sidebar/chat_sidebar_item.h"
#include "common/ui/sidebar/chat_sidebar_new_chat_popup.h"
#include "common/ui/sidebar/chat_sidebar_new_folder_popup.h"
#include "common/ui/sidebar/chat_sidebar_options_popup.h"
#include "common/ui/sidebar/chat_sidebar_tree.h"

/// <summary>
/// Draws the full left chat sidebar pane and related popups.
/// </summary>
static void DrawLeftPane(AppState& app) {
  BeginPanel("left_sidebar", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true, 0, ImVec2(ui::kSpace12, ui::kSpace12));
  EnsureNewChatFolderSelection(app);

  DrawChatSidebarHeader(app);

  std::string chat_to_delete;
  std::string chat_to_open_options;
  DrawChatSidebarTree(app, chat_to_delete, chat_to_open_options);

  if (!chat_to_delete.empty()) {
    if (app.settings.confirm_delete_chat) {
      app.pending_delete_chat_id = chat_to_delete;
      app.open_delete_chat_popup = true;
    } else {
      RemoveChatById(app, chat_to_delete);
    }
  }
  if (!chat_to_open_options.empty()) {
    app.sidebar_chat_options_popup_chat_id = chat_to_open_options;
    app.open_sidebar_chat_options_popup = true;
  }
  DrawSidebarChatOptionsPopup(app);
  DrawSidebarNewChatPopup(app);
  DrawSidebarNewFolderPopup(app);

  EndPanel();
}
