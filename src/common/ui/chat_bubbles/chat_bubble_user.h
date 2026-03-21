#pragma once

#include "common/ui/chat_bubbles/chat_bubble_common.h"

/// <summary>
/// Draws user message bubble chrome, role label, and user actions.
/// </summary>
static void DrawUserMessageBubble(AppState& app,
                                  ChatSession& chat,
                                  const int message_index,
                                  const Message& message,
                                  const ChatBubbleLayout& layout) {
  const bool light = IsLightPaletteActive();
  const ImVec4 background = light ? Rgb(66, 126, 228, 0.16f) : Rgb(94, 160, 255, 0.20f);
  const ImVec4 border = light ? Rgb(66, 126, 228, 0.42f) : Rgb(111, 171, 255, 0.50f);

  DrawChatBubbleFrame(layout, background, border);
  DrawChatBubbleRoleLabel(layout, "You", ui::kAccent);

  ImGui::SetCursorScreenPos(ImVec2(layout.m_max.x - 158.0f, layout.m_min.y + 7.0f));
  if (DrawButton("Branch", ImVec2(72.0f, 24.0f), ButtonKind::Ghost)) {
    app.pending_branch_chat_id = chat.id;
    app.pending_branch_message_index = message_index;
  }

  ImGui::SetCursorScreenPos(ImVec2(layout.m_max.x - 80.0f, layout.m_min.y + 7.0f));
  if (FrontendActionVisible(app, "edit_resubmit", true) &&
      DrawButton("Edit", ImVec2(70.0f, 24.0f), ButtonKind::Ghost)) {
    BeginEditMessage(app, chat, message_index);
  }

  DrawChatBubbleContent(layout, message.content);
  DrawChatBubbleTimestamp(layout, message.created_at);
}
