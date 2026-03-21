#pragma once

#include "common/ui/chat_bubbles/chat_bubble_common.h"

/// <summary>
/// Draws assistant message bubble chrome, role label, and colors.
/// </summary>
static void DrawAssistantMessageBubble(const Message& message, const ChatBubbleLayout& layout) {
  const bool light = IsLightPaletteActive();
  const ImVec4 background = light ? Rgb(252, 254, 255, 0.97f) : Rgb(23, 29, 39, 0.98f);
  const ImVec4 border = ui::kBorder;

  DrawChatBubbleFrame(layout, background, border);
  DrawChatBubbleRoleLabel(layout, "Assistant", ui::kSuccess);
  DrawChatBubbleContent(layout, message.content);
  DrawChatBubbleTimestamp(layout, message.created_at);
}
