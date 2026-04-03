#pragma once

#include "common/ui/chat_detail/chat_detail_cli_panel.h"
#include "common/ui/chat_detail/chat_detail_composer_input.h"
#include "common/ui/chat_detail/chat_detail_edit_popup.h"
#include "common/ui/chat_detail/chat_detail_header_bar.h"
#include "common/ui/chat_detail/chat_detail_history_panel.h"

/// <summary>
/// Draws the center chat detail pane by composing header, body mode, history, and composer components.
/// </summary>
inline void DrawChatDetailPane(AppState& app, ChatSession& chat)
{
	MarkSelectedChatSeen(app);
	BeginPanel("main_chat_panel", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(ui::kSpace16, ui::kSpace16));

	DrawChatDetailHeaderBar(app, chat);

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);

	if (ProviderRuntime::UsesCliOutput(provider))
	{
		if (!ProviderRuntime::UsesInternalEngine(provider) && provider.supports_interactive)
		{
			MarkSelectedCliTerminalForLaunch(app);
		}

		DrawChatDetailCliConsoleBody(app, chat);
		EndPanel();
		return;
	}

	DrawChatDetailConversationHistory(app, chat);
	DrawEditUserMessagePopup(app, chat);
	DrawInputContainer(app, chat);

	EndPanel();
}
