#pragma once

/// <summary>
/// Draws the local Gemini workspace card in the chat settings side pane.
/// </summary>
inline void DrawChatSettingsLocalGeminiCard(AppState& app, ChatSession& chat)
{
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
	DrawSectionHeader("Local Gemini");

	if (BeginSectionCard("local_gemini_card"))
	{
		const fs::path local_root = WorkspacePromptProfileRootPath(app, chat);
		const fs::path local_template = WorkspacePromptProfileTemplatePath(app, chat);
		ImGui::TextColored(ui::kTextMuted, "Workspace root");
		ImGui::TextWrapped("%s", local_root.string().c_str());
		ImGui::TextColored(ui::kTextMuted, "Template file");
		ImGui::TextWrapped("%s", local_template.string().c_str());
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

		if (DrawButton("Open .gemini", ImVec2(120.0f, 30.0f), ButtonKind::Ghost))
		{
			std::string error;

			if (!OpenFolderInFileManager(local_root, &error))
			{
				app.status_line = error;
			}
		}

		ImGui::SameLine();

		if (DrawButton("Reveal gemini.md", ImVec2(138.0f, 30.0f), ButtonKind::Ghost))
		{
			std::string error;

			if (!RevealPathInFileManager(local_template, &error))
			{
				app.status_line = error;
			}
		}

		if (DrawButton("Open Lessons", ImVec2(120.0f, 30.0f), ButtonKind::Ghost))
		{
			std::string error;

			if (!OpenFolderInFileManager(local_root / "Lessons", &error))
			{
				app.status_line = error;
			}
		}

		ImGui::SameLine();

		if (DrawButton("Open Failures", ImVec2(120.0f, 30.0f), ButtonKind::Ghost))
		{
			std::string error;

			if (!OpenFolderInFileManager(local_root / "Failures", &error))
			{
				app.status_line = error;
			}
		}

		ImGui::SameLine();

		if (DrawButton("Open auto-test", ImVec2(120.0f, 30.0f), ButtonKind::Ghost))
		{
			std::string error;

			if (!OpenFolderInFileManager(local_root / "auto-test", &error))
			{
				app.status_line = error;
			}
		}

		if (DrawButton("Sync Template Now", ImVec2(132.0f, 32.0f), ButtonKind::Primary))
		{
			std::string sync_status;
			const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);
			const TemplatePreflightOutcome outcome = PreflightWorkspaceTemplateForChat(app, provider, chat, nullptr, &sync_status);

			if (outcome == TemplatePreflightOutcome::BlockingError)
			{
				app.status_line = sync_status.empty() ? "Template sync failed." : sync_status;
			}
			else
			{
				app.status_line = (outcome == TemplatePreflightOutcome::ReadyWithTemplate) ? "Synced effective template to .gemini/gemini.md." : sync_status;
			}
		}
	}

	EndPanel();
}
