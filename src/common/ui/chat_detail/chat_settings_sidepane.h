#pragma once

#include "common/ui/chat_detail/chat_settings_section_basics.h"
#include "common/ui/chat_detail/chat_settings_command_card.h"
#include "common/ui/chat_detail/chat_settings_local_gemini_card.h"
#include "common/ui/chat_detail/chat_settings_move_folder_card.h"
#include "common/ui/chat_detail/chat_settings_rag_card.h"
#include "common/ui/chat_detail/chat_settings_repository_card.h"
#include "common/ui/chat_detail/chat_settings_terminal_card.h"
#include "common/ui/chat_detail/chat_settings_template_card.h"

/// <summary>
/// Draws the right-side chat settings pane by composing card-level components.
/// </summary>
inline void DrawSessionSidePane(AppState& app, ChatSession& chat)
{
	TemplateRuntimeService().RefreshTemplateCatalog(app);
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	BeginPanel("right_settings", ImVec2(0.0f, 0.0f), PanelTone::Secondary, true, 0, ImVec2(ui::kSpace16, ui::kSpace16));
	PushFontIfAvailable(g_font_title);
	ImGui::TextColored(ui::kTextPrimary, "Chat Settings");
	PopFontIfAvailable(g_font_title);
	ImGui::TextColored(ui::kTextMuted, "Prompt profile, folder, provider workspace, repository, and RAG");
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	DrawSoftDivider();

	DrawChatSettingsTemplateCard(app, chat);
	DrawChatSettingsMoveFolderCard(app, chat);

	if (ProviderRuntime::UsesGeminiPathBootstrap(provider))
	{
		DrawChatSettingsLocalGeminiCard(app, chat);
	}

	DrawChatSettingsRepositoryCard(app, chat);
	DrawChatSettingsRagCard(app, chat);

	if (ProviderRuntime::UsesCliOutput(provider))
	{
		DrawChatSettingsTerminalCard(app, chat);
	}

	DrawChatSettingsCommandPreviewCard(app, chat);

	EndPanel();
}
