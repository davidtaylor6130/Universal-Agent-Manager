#include "ui_orchestration_controller.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"

#include "common/runtime/terminal_common.h"
#include "common/ui/ui_sections.h"

#include <algorithm>
#include <filesystem>
#include <thread>

using uam::AppState;
using uam::CliTerminalState;
namespace fs = std::filesystem;

void ChatDetailView::Draw(uam::AppState& p_app, ChatSession* p_selectedChat) const
{
	if (p_selectedChat == nullptr)
	{
		BeginPanel("empty_main", ImVec2(0.0f, 0.0f), PanelTone::Primary, true, 0, ImVec2(ui::kSpace24, ui::kSpace24));
		ImGui::TextWrapped("No chat selected. Create one from the left panel.");
		EndPanel();
		return;
	}

	DrawChatDetailPane(p_app, *p_selectedChat);
}

void ModalHostView::Draw(uam::AppState& p_app, const float p_platformUiScale) const
{
	DrawAboutModal(p_app);
	DrawDeleteChatConfirmationModal(p_app);
	DrawDeleteFolderConfirmationModal(p_app);
	DrawFolderSettingsModal(p_app);
	DrawTemplateChangeWarningModal(p_app);
	DrawMarkdownTemplateManagerModal(p_app);
	DrawVcsOutputModal(p_app);
	DrawRuntimeModelSelectionModal(p_app);
	DrawRagConsoleModal(p_app);
	DrawAppSettingsModal(p_app, p_platformUiScale);
	ChatDomainService().ConsumePendingBranchRequest(p_app);
}

void UiController::DrawFrame(uam::AppState& p_app,
                             bool& p_done,
                             const float p_platformUiScale,
                             const IPlatformUiTraits& p_uiTraits,
                             const ChatDetailView& p_chatDetail,
                             const ModalHostView& p_modalHost) const
{
	ImGuiIO& l_io = ImGui::GetIO();
	ApplyUserUiScale(l_io, p_app.settings.ui_scale_multiplier);
	PollRagScanState(p_app);

	HandleGlobalShortcuts(p_app);
	DrawDesktopMenuBar(p_app, p_done);

	const ImGuiViewport* lcp_viewport = ImGui::GetMainViewport();
	DrawAmbientBackdrop(lcp_viewport->Pos, lcp_viewport->Size, float(ImGui::GetTime()));

	ImGui::SetNextWindowPos(ImVec2(lcp_viewport->WorkPos.x, lcp_viewport->WorkPos.y));
	ImGui::SetNextWindowSize(ImVec2(lcp_viewport->WorkSize.x, lcp_viewport->WorkSize.y));

	const ImGuiWindowFlags l_windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
	ImGui::Begin("Universal Agent Manager", nullptr, l_windowFlags);

	const float l_layoutWidth = ImGui::GetContentRegionAvail().x;
	float l_sidebarWidth = std::clamp(l_layoutWidth * 0.25f, 250.0f, 360.0f);

	if (l_layoutWidth < 1020.0f)
	{
		l_sidebarWidth = std::clamp(l_layoutWidth * 0.30f, 230.0f, 320.0f);
	}

	l_sidebarWidth = p_uiTraits.AdjustSidebarWidth(l_layoutWidth, l_sidebarWidth, EffectiveUiScale());

	if (ImGui::BeginTable("layout_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBody))
	{
		ImGui::TableSetupColumn("Chats", ImGuiTableColumnFlags_WidthFixed, l_sidebarWidth);
		ImGui::TableSetupColumn("Conversation", ImGuiTableColumnFlags_WidthStretch, 0.72f);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		DrawLeftPane(p_app);

		ChatSession* lp_selectedChat = ChatDomainService().SelectedChat(p_app);
		ImGui::TableSetColumnIndex(1);
		p_chatDetail.Draw(p_app, lp_selectedChat);
		ImGui::EndTable();
	}

	ImGui::End();
	p_modalHost.Draw(p_app, p_platformUiScale);
}
