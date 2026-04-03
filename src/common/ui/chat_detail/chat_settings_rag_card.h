#pragma once

/// <summary>
/// Draws RAG configuration and rebuild controls in the chat settings side pane.
/// </summary>
inline void DrawChatSettingsRagCard(AppState& app, ChatSession& chat)
{
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
	DrawSectionHeader("RAG");

	if (BeginSectionCard("rag_card"))
	{
		const fs::path chat_workspace_root = ResolveWorkspaceRootPath(app, chat);
		const std::vector<fs::path> rag_source_roots = ResolveRagSourceRootsForChat(app, chat, chat_workspace_root);
		const fs::path workspace_root = rag_source_roots.empty() ? ResolveProjectRagSourceRoot(app, chat_workspace_root) : rag_source_roots.front();
		const std::string workspace_key = workspace_root.lexically_normal().generic_string();
		EnsureRagManualQueryWorkspaceState(app, workspace_key);

		ImGui::TextColored(ui::kTextMuted, "Project source directory");
		std::string browse_error;
		DrawPathInputWithBrowseButton("##rag_project_source_directory", app.settings.rag_project_source_directory, "rag_card_source_directory_picker", PathBrowseTarget::Directory, -1.0f, nullptr, nullptr, &browse_error);

		if (!browse_error.empty())
		{
			app.status_line = browse_error;
		}

		if (Trim(app.settings.rag_project_source_directory).empty())
		{
			ImGui::TextColored(ui::kTextMuted, "(empty uses current chat workspace)");
		}

		if (DrawButton("Use Chat Workspace", ImVec2(156.0f, 30.0f), ButtonKind::Ghost))
		{
			app.settings.rag_project_source_directory = chat_workspace_root.string();
			PersistenceCoordinator().SaveSettings(app);
			app.status_line = "RAG source directory set from chat workspace.";
			AppendRagScanReport(app, "Source directory set from chat workspace.");
		}

		ImGui::SameLine();

		if (DrawButton("Save Source", ImVec2(108.0f, 30.0f), ButtonKind::Ghost))
		{
			PersistenceCoordinator().SaveSettings(app);
			app.status_line = "RAG source directory saved.";
			AppendRagScanReport(app, "Source directory saved.");
		}

		std::error_code source_ec;
		const bool source_valid = (!workspace_root.empty() && fs::exists(workspace_root, source_ec) && fs::is_directory(workspace_root, source_ec));
		ImGui::TextColored(ui::kTextMuted, "Resolved source");
		ImGui::TextWrapped("%s", workspace_root.string().c_str());

		if (rag_source_roots.size() > 1)
		{
			ImGui::TextColored(ui::kTextMuted, "Custom sources selected: %zu (scan buttons target the first source)", rag_source_roots.size());
		}

		if (!source_valid)
		{
			ImGui::TextColored(ui::kTextMuted, "Source is invalid. Update the directory before scanning.");
		}

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
		DrawSoftDivider();
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
		ImGui::TextColored(ui::kTextMuted, "RAG mode");
		ImGui::TextColored(ui::kTextPrimary, "%s", app.settings.rag_enabled ? "Enabled (Structured mode)" : "Disabled");
		ImGui::TextColored(ui::kTextMuted, "Current state");
		const std::string rag_status = BuildRagStatusText(app);
		ImGui::TextColored(ui::kTextPrimary, "%s", rag_status.c_str());
		ImGui::TextColored(ui::kTextMuted, "Top K");
		ImGui::TextColored(ui::kTextPrimary, "%d", app.settings.rag_top_k);
		ImGui::TextColored(ui::kTextMuted, "Max snippet chars");
		ImGui::TextColored(ui::kTextPrimary, "%d", app.settings.rag_max_snippet_chars);
		ImGui::TextColored(ui::kTextMuted, "Max file bytes");
		ImGui::TextColored(ui::kTextPrimary, "%d", app.settings.rag_max_file_bytes);

		if (const auto it = app.rag_last_refresh_by_workspace.find(workspace_key); it != app.rag_last_refresh_by_workspace.end())
		{
			ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
			ImGui::TextColored(ui::kTextMuted, "Last refresh");
			ImGui::TextWrapped("%s", it->second.c_str());
		}

		app.settings.rag_scan_max_tokens = std::clamp(app.settings.rag_scan_max_tokens, 0, 32768);
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
		ImGui::TextColored(ui::kTextMuted, "Max embedding tokens");
		ImGui::SetNextItemWidth(108.0f);
		int rag_scan_max_tokens = app.settings.rag_scan_max_tokens;

		if (ImGui::InputInt("##rag_scan_max_tokens_card", &rag_scan_max_tokens))
		{
			rag_scan_max_tokens = std::clamp(rag_scan_max_tokens, 0, 32768);

			if (rag_scan_max_tokens != app.settings.rag_scan_max_tokens)
			{
				app.settings.rag_scan_max_tokens = rag_scan_max_tokens;
				PersistenceCoordinator().SaveSettings(app);

				if (rag_scan_max_tokens > 0)
				{
					app.status_line = "RAG embedding token cap set to " + std::to_string(rag_scan_max_tokens) + ".";
					AppendRagScanReport(app, app.status_line);
				}
				else
				{
					app.status_line = "RAG embedding token cap cleared (engine default).";
					AppendRagScanReport(app, app.status_line);
				}
			}
		}

		ImGui::SameLine();
		ImGui::TextColored(ui::kTextMuted, "0 = default");

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

		if (DrawButton("Rebuild Index", ImVec2(128.0f, 30.0f), ButtonKind::Primary))
		{
			std::string scan_error;

			if (!TriggerProjectRagScan(app, false, chat_workspace_root, &scan_error))
			{
				app.status_line = "RAG scan failed to start: " + scan_error;
			}
			else
			{
				app.status_line = "RAG scan started.";
			}
		}

		ImGui::SameLine();

		if (DrawButton("Rescan Previous", ImVec2(138.0f, 30.0f), ButtonKind::Ghost))
		{
			std::string scan_error;

			if (!TriggerProjectRagScan(app, true, chat_workspace_root, &scan_error))
			{
				app.status_line = "RAG rescan failed to start: " + scan_error;
			}
			else
			{
				app.status_line = "RAG rescan started (previous source).";
			}
		}

		ImGui::SameLine();

		if (DrawButton("Open RAG Console", ImVec2(148.0f, 30.0f), ButtonKind::Ghost))
		{
			app.open_rag_console_popup = true;
		}

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
		ImGui::TextColored(ui::kTextMuted, "Latest rebuild");

		if (const auto it = app.rag_last_rebuild_at_by_workspace.find(workspace_key); it != app.rag_last_rebuild_at_by_workspace.end() && !it->second.empty())
		{
			ImGui::TextColored(ui::kTextPrimary, "%s", it->second.c_str());
		}
		else
		{
			ImGui::TextColored(ui::kTextMuted, "(not rebuilt yet)");
		}

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
		DrawSoftDivider();
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
		ImGui::TextColored(ui::kTextMuted, "Manual retrieval test");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputText("##rag_test_query", &app.rag_manual_query_input);

		app.rag_manual_query_max = std::clamp(app.rag_manual_query_max, 1, 50);
		app.rag_manual_query_min = std::clamp(app.rag_manual_query_min, 1, app.rag_manual_query_max);
		ImGui::SetNextItemWidth(82.0f);
		ImGui::InputInt("Max", &app.rag_manual_query_max);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(82.0f);
		ImGui::InputInt("Min", &app.rag_manual_query_min);

		const bool has_query = !Trim(app.rag_manual_query_input).empty();

		if (!has_query || app.rag_manual_query_running)
		{
			ImGui::BeginDisabled();
		}

		if (DrawButton("Run Test Query", ImVec2(140.0f, 30.0f), ButtonKind::Primary))
		{
			RunRagManualTestQuery(app, workspace_root);
		}

		if (!has_query || app.rag_manual_query_running)
		{
			ImGui::EndDisabled();
		}

		if (app.rag_manual_query_running)
		{
			ImGui::TextColored(ui::kTextMuted, "Running query...");
		}

		if (!app.rag_manual_query_last_query.empty())
		{
			ImGui::TextColored(ui::kTextMuted, "Last query");
			ImGui::TextWrapped("%s", app.rag_manual_query_last_query.c_str());
		}

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
		ImGui::TextColored(ui::kTextMuted, "Results");
		ImGui::BeginChild("rag_test_results", ImVec2(0.0f, 190.0f), true);

		if (!app.rag_manual_query_error.empty())
		{
			ImGui::TextWrapped("%s", app.rag_manual_query_error.c_str());
		}
		else if (app.rag_manual_query_last_query.empty())
		{
			ImGui::TextColored(ui::kTextMuted, "Run a manual test query to verify retrieval output.");
		}
		else if (app.rag_manual_query_results.empty())
		{
			ImGui::TextColored(ui::kTextMuted, "No snippets found for this query.");
		}
		else
		{
			for (std::size_t i = 0; i < app.rag_manual_query_results.size(); ++i)
			{
				const RagSnippet& snippet = app.rag_manual_query_results[i];
				std::string label = "#" + std::to_string(i + 1);

				if (!snippet.relative_path.empty())
				{
					label += "  " + snippet.relative_path;

					if (snippet.start_line > 0 && snippet.end_line >= snippet.start_line)
					{
						label += ":" + std::to_string(snippet.start_line) + "-" + std::to_string(snippet.end_line);
					}
				}

				ImGui::TextColored(ui::kTextSecondary, "%s", label.c_str());
				ImGui::TextWrapped("%s", snippet.text.c_str());

				if ((i + 1) < app.rag_manual_query_results.size())
				{
					ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
					DrawSoftDivider();
					ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
				}
			}
		}

		ImGui::EndChild();
	}

	EndPanel();
}
