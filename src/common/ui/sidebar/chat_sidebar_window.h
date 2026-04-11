#pragma once

#include "common/ui/sidebar/chat_sidebar_folder_header.h"
#include "common/ui/sidebar/chat_sidebar_header.h"
#include "common/ui/sidebar/chat_sidebar_item.h"
#include "common/ui/sidebar/chat_sidebar_new_chat_popup.h"
#include "common/ui/sidebar/chat_sidebar_options_popup.h"
#include "common/ui/sidebar/chat_sidebar_tree.h"

/// <summary>
/// Draws the full left chat sidebar pane and related popups.
/// </summary>
inline void DrawLeftPane(AppState& app)
{
	BeginPanel("left_sidebar", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true, 0, ImVec2(ui::kSpace12, ui::kSpace12));
	ChatDomainService().EnsureNewChatFolderSelection(app);

	DrawChatSidebarHeader(app);

	std::string chat_to_open_options;
	DrawChatSidebarTree(app, chat_to_open_options);

	if (!chat_to_open_options.empty())
	{
		app.sidebar_chat_options_popup_chat_id = chat_to_open_options;
		app.open_sidebar_chat_options_popup = true;
	}

	DrawSidebarChatOptionsPopup(app);
	DrawSidebarNewChatPopup(app);

	EndPanel();
}
