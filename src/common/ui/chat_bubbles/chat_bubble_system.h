#pragma once

#include "common/ui/chat_bubbles/chat_bubble_common.h"

/// <summary>
/// Draws system message bubble chrome, role label, and colors.
/// </summary>
static void DrawSystemMessageBubble(const Message& message, const ChatBubbleLayout& layout) {
  const bool light = IsLightPaletteActive();
  const ImVec4 background = light ? Rgb(255, 244, 230, 0.98f) : Rgb(71, 52, 22, 0.38f);
  const ImVec4 border = light ? Rgb(245, 158, 11, 0.45f) : Rgb(245, 158, 11, 0.45f);

  DrawChatBubbleFrame(layout, background, border);
  DrawChatBubbleRoleLabel(layout, "System", ui::kWarning);
  DrawChatBubbleContent(layout, message.content);
  DrawChatBubbleTimestamp(layout, message.created_at);
}
