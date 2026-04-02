#pragma once

/// <summary>
/// About dialog modal renderer.
/// </summary>
static void DrawAboutModal(AppState& app)
{
	if (app.open_about_popup)
	{
		ImGui::OpenPopup("about_popup");
		app.open_about_popup = false;
	}

	if (!ImGui::BeginPopupModal("about_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	PushFontIfAvailable(g_font_title);
	ImGui::TextColored(ui::kTextPrimary, "%s", kAppDisplayName);
	PopFontIfAvailable(g_font_title);
	ImGui::TextColored(ui::kTextMuted, "Desktop application");
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	DrawSoftDivider();

	if (ImGui::BeginTable("about_table", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextColored(ui::kTextSecondary, "Version");
		ImGui::TableSetColumnIndex(1);
		ImGui::TextColored(ui::kTextPrimary, "%s", kAppVersion);

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextColored(ui::kTextSecondary, "Build");
		ImGui::TableSetColumnIndex(1);
		ImGui::TextColored(ui::kTextPrimary, "%s %s", __DATE__, __TIME__);

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextColored(ui::kTextSecondary, "Provider");
		ImGui::TableSetColumnIndex(1);
		ImGui::TextColored(ui::kTextPrimary, "Gemini CLI Compatible");
		ImGui::EndTable();
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	ImGui::TextWrapped("%s", kAppCopyright);
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));

	if (DrawButton("OK", ImVec2(96.0f, 32.0f), ButtonKind::Primary))
	{
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}
