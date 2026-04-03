#pragma once

/// <summary>
/// Draws the project-wide RAG console modal with scan progress and manual retrieval testing.
/// </summary>
inline void DrawRagConsoleModal(AppState& app)
{
	if (app.open_rag_console_popup)
	{
		ImGui::OpenPopup("rag_console_popup");
		app.open_rag_console_popup = false;
	}

	ImGui::SetNextWindowSize(ImVec2(980.0f, 760.0f), ImGuiCond_Appearing);

	if (!ImGui::BeginPopupModal("rag_console_popup", nullptr, ImGuiWindowFlags_NoResize))
	{
		return;
	}

	const ChatSession* selected_chat = ChatDomainService().SelectedChat(app);
	const bool has_selected_chat = (selected_chat != nullptr);
	const fs::path selected_chat_workspace = has_selected_chat ? ResolveWorkspaceRootPath(app, *selected_chat) : fs::path{};
	const fs::path fallback_source_root = ResolveCurrentRagFallbackSourceRoot(app);
	const std::vector<fs::path> rag_source_roots = has_selected_chat ? ResolveRagSourceRootsForChat(app, *selected_chat, fallback_source_root) : std::vector<fs::path>{};
	const fs::path workspace_root = rag_source_roots.empty() ? ResolveProjectRagSourceRoot(app, fallback_source_root) : rag_source_roots.front();
	const std::string workspace_key = workspace_root.lexically_normal().generic_string();
	EnsureRagManualQueryWorkspaceState(app, workspace_key);

	std::error_code source_ec;
	const bool source_valid = (!workspace_root.empty() && fs::exists(workspace_root, source_ec) && fs::is_directory(workspace_root, source_ec));
	const RagScanState scan_state = EffectiveRagScanState(app);
	const std::string rag_status = BuildRagStatusText(app);

	ImGui::TextColored(ui::kTextPrimary, "RAG Console");
	ImGui::SameLine();
	ImGui::TextColored(ui::kTextMuted, "Project-wide indexing and retrieval");
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	DrawSoftDivider();

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	ImGui::TextColored(ui::kTextMuted, "Project source directory");
	std::string browse_error;
	DrawPathInputWithBrowseButton("##rag_console_source_directory", app.settings.rag_project_source_directory, "rag_console_source_directory_picker", PathBrowseTarget::Directory, -1.0f, nullptr, nullptr, &browse_error);

	if (!browse_error.empty())
	{
		app.status_line = browse_error;
	}

	if (Trim(app.settings.rag_project_source_directory).empty())
	{
		ImGui::TextColored(ui::kTextMuted, "(empty uses selected chat workspace; falls back to current working directory)");
	}

	if (!has_selected_chat)
	{
		ImGui::BeginDisabled();
	}

	if (DrawButton("Use Chat Workspace", ImVec2(160.0f, 30.0f), ButtonKind::Ghost))
	{
		app.settings.rag_project_source_directory = selected_chat_workspace.string();
		SaveSettings(app);
		app.status_line = "RAG source directory set from chat workspace.";
		AppendRagScanReport(app, "Source directory set from selected chat workspace.");
	}

	if (!has_selected_chat)
	{
		ImGui::EndDisabled();
	}

	ImGui::SameLine();

	if (DrawButton("Save Source", ImVec2(108.0f, 30.0f), ButtonKind::Ghost))
	{
		SaveSettings(app);
		app.status_line = "RAG source directory saved.";
		AppendRagScanReport(app, "Source directory saved.");
	}

	ImGui::SameLine();

	if (DrawButton("Clear Source", ImVec2(112.0f, 30.0f), ButtonKind::Ghost))
	{
		app.settings.rag_project_source_directory.clear();
		SaveSettings(app);
		app.status_line = "RAG source directory cleared.";
		AppendRagScanReport(app, "Source directory cleared; fallback source is now selected chat workspace or current working directory.");
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
	ImGui::TextColored(ui::kTextMuted, "Resolved source");
	ImGui::TextWrapped("%s", workspace_root.string().c_str());

	if (rag_source_roots.size() > 1)
	{
		ImGui::TextColored(ui::kTextMuted, "Custom sources selected: %zu (scan buttons target the first source)", rag_source_roots.size());
	}

	if (!source_valid)
	{
		ImGui::TextColored(ui::kWarning, "Source is invalid. Update the directory before scanning.");
	}

	if (has_selected_chat)
	{
		ImGui::TextColored(ui::kTextMuted, "Selected chat workspace: %s", selected_chat_workspace.string().c_str());
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	DrawSoftDivider();
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

	ImGui::TextColored(ui::kTextMuted, "Current scan state");
	ImGui::TextColored(ui::kTextPrimary, "%s", rag_status.c_str());

	if (scan_state.lifecycle == RagScanLifecycleState::Running)
	{
		if (scan_state.total_files > 0)
		{
			const float progress = std::clamp(static_cast<float>(scan_state.files_processed) / static_cast<float>(scan_state.total_files), 0.0f, 1.0f);
			std::string overlay = std::to_string(scan_state.files_processed) + "/" + std::to_string(scan_state.total_files) + " files";

			if (scan_state.vector_database_size > 0)
			{
				overlay += " | " + std::to_string(scan_state.vector_database_size) + " vectors";
			}

			ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), overlay.c_str());
		}
		else
		{
			ImGui::TextColored(ui::kTextMuted, "Scanning file tree...");
		}
	}
	else if (scan_state.lifecycle == RagScanLifecycleState::Finished)
	{
		ImGui::TextColored(ui::kSuccess, "Last run finished.");
	}
	else
	{
		ImGui::TextColored(ui::kTextMuted, "Idle.");
	}

	if (const auto it = app.rag_last_refresh_by_workspace.find(workspace_key); it != app.rag_last_refresh_by_workspace.end())
	{
		ImGui::TextColored(ui::kTextMuted, "Last refresh: %s", it->second.c_str());
	}

	if (const auto it = app.rag_last_rebuild_at_by_workspace.find(workspace_key); it != app.rag_last_rebuild_at_by_workspace.end() && !it->second.empty())
	{
		ImGui::TextColored(ui::kTextMuted, "Latest rebuild: %s", it->second.c_str());
	}

	app.settings.rag_scan_max_tokens = std::clamp(app.settings.rag_scan_max_tokens, 0, 32768);
	ImGui::TextColored(ui::kTextMuted, "Max embedding tokens");
	ImGui::SetNextItemWidth(116.0f);
	int rag_scan_max_tokens = app.settings.rag_scan_max_tokens;

	if (ImGui::InputInt("##rag_scan_max_tokens_modal", &rag_scan_max_tokens))
	{
		rag_scan_max_tokens = std::clamp(rag_scan_max_tokens, 0, 32768);

		if (rag_scan_max_tokens != app.settings.rag_scan_max_tokens)
		{
			app.settings.rag_scan_max_tokens = rag_scan_max_tokens;
			SaveSettings(app);

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
	ImGui::TextColored(ui::kTextMuted, "0 = engine default");

	if (!source_valid)
	{
		ImGui::BeginDisabled();
	}

	if (DrawButton("Rebuild Index", ImVec2(132.0f, 32.0f), ButtonKind::Primary))
	{
		std::string scan_error;

		if (!TriggerProjectRagScan(app, false, fallback_source_root, &scan_error))
		{
			app.status_line = "RAG scan failed to start: " + scan_error;
		}
		else
		{
			app.status_line = "RAG scan started.";
		}
	}

	if (!source_valid)
	{
		ImGui::EndDisabled();
	}

	ImGui::SameLine();

	if (DrawButton("Rescan Previous", ImVec2(142.0f, 32.0f), ButtonKind::Ghost))
	{
		std::string scan_error;

		if (!TriggerProjectRagScan(app, true, fallback_source_root, &scan_error))
		{
			app.status_line = "RAG rescan failed to start: " + scan_error;
		}
		else
		{
			app.status_line = "RAG rescan started (previous source).";
		}
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	DrawSoftDivider();
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	ImGui::TextColored(ui::kTextMuted, "Manual retrieval test");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##rag_console_test_query", &app.rag_manual_query_input);

	app.rag_manual_query_max = std::clamp(app.rag_manual_query_max, 1, 50);
	app.rag_manual_query_min = std::clamp(app.rag_manual_query_min, 1, app.rag_manual_query_max);
	ImGui::SetNextItemWidth(88.0f);
	ImGui::InputInt("Max", &app.rag_manual_query_max);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(88.0f);
	ImGui::InputInt("Min", &app.rag_manual_query_min);

	const bool has_query = !Trim(app.rag_manual_query_input).empty();

	if (!has_query || app.rag_manual_query_running)
	{
		ImGui::BeginDisabled();
	}

	if (DrawButton("Run Test Query", ImVec2(146.0f, 30.0f), ButtonKind::Primary))
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
	ImGui::BeginChild("rag_console_test_results", ImVec2(0.0f, 170.0f), true);

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

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	DrawSoftDivider();
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	ImGui::TextColored(ui::kTextMuted, "Scan progress reports");
	ImGui::SameLine();

	if (DrawButton("Clear Reports", ImVec2(116.0f, 28.0f), ButtonKind::Ghost))
	{
		app.rag_scan_reports.clear();
		app.rag_scan_reports_scroll_to_bottom = false;
	}

	ImGui::BeginChild("rag_console_scan_reports", ImVec2(0.0f, 140.0f), true);

	if (app.rag_scan_reports.empty())
	{
		ImGui::TextColored(ui::kTextMuted, "No scan reports yet.");
	}
	else
	{
		for (const std::string& report : app.rag_scan_reports)
		{
			ImGui::TextUnformatted(report.c_str());
		}

		if (app.rag_scan_reports_scroll_to_bottom)
		{
			ImGui::SetScrollHereY(1.0f);
			app.rag_scan_reports_scroll_to_bottom = false;
		}
	}

	ImGui::EndChild();

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));

	if (DrawButton("Close", ImVec2(98.0f, 32.0f), ButtonKind::Ghost))
	{
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}
