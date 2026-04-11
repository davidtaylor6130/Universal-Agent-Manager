#include "ui_orchestration_controller.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

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
	(void)p_platformUiScale;
	DrawRenameChatModal(p_app);
}

void UiController::DrawFrame(uam::AppState& p_app, bool& p_done, const float p_platformUiScale, const IPlatformUiTraits& p_uiTraits, const ChatDetailView& p_chatDetail, const ModalHostView& p_modalHost) const
{
	(void)p_done;
	ImGuiIO& l_io = ImGui::GetIO();
	ApplyUserUiScale(l_io, p_app.settings.ui_scale_multiplier);

	const ImGuiViewport* lcp_viewport = ImGui::GetMainViewport();
	DrawAmbientBackdrop(lcp_viewport->Pos, lcp_viewport->Size, float(ImGui::GetTime()));

	ImGui::SetNextWindowPos(ImVec2(lcp_viewport->WorkPos.x, lcp_viewport->WorkPos.y));
	ImGui::SetNextWindowSize(ImVec2(lcp_viewport->WorkSize.x, lcp_viewport->WorkSize.y));

	const ImGuiWindowFlags l_windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
	ImGui::Begin("Universal Agent Manager", nullptr, l_windowFlags);

	const ImVec2 l_layoutSize = ImGui::GetContentRegionAvail();
	const float l_layoutWidth = l_layoutSize.x;
	const float l_layoutHeight = l_layoutSize.y;
	p_app.settings.sidebar_width = p_uiTraits.AdjustSidebarWidth(l_layoutWidth, p_app.settings.sidebar_width, EffectiveUiScale());

	const ImGuiWindowFlags l_hostFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::BeginChild("left_pane_host", ImVec2(p_app.settings.sidebar_width, l_layoutHeight), false, l_hostFlags);
	DrawLeftPane(p_app);
	ImGui::EndChild();

	ImGui::SameLine(0.0f, 0.0f);
	const VerticalSplitterResult l_splitter = DrawVerticalSplitter("main_sidebar_splitter", l_layoutHeight, &p_app.sidebar_resize_drag_active);

	if (l_splitter.active && l_splitter.drag_delta_x != 0.0f)
	{
		const float l_proposedWidth = p_app.settings.sidebar_width + l_splitter.drag_delta_x;
		p_app.settings.sidebar_width = p_uiTraits.AdjustSidebarWidth(l_layoutWidth, l_proposedWidth, EffectiveUiScale());
	}

	if (l_splitter.released)
	{
		PersistenceCoordinator().SaveSettings(p_app);
	}

	ImGui::SameLine(0.0f, 0.0f);
	ImGui::BeginChild("main_pane_host", ImVec2(0.0f, l_layoutHeight), false, l_hostFlags);
	ChatSession* lp_selectedChat = ChatDomainService().SelectedChat(p_app);
	p_chatDetail.Draw(p_app, lp_selectedChat);
	ImGui::EndChild();
	ImGui::PopStyleVar();

	ImGui::End();
	p_modalHost.Draw(p_app, p_platformUiScale);
}
