#pragma once
#include "common/platform/platform_services.h"

/// <summary>
/// Draws the structured composer container and send actions.
/// </summary>
static void DrawInputContainer(AppState& app, ChatSession& chat)
{
	BeginPanel("input_container", ImVec2(0.0f, 166.0f), PanelTone::Secondary, true, 0, ImVec2(12.0f, 12.0f), ui::kRadiusInput);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 min = ImGui::GetWindowPos();
	const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
	draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 4.0f), ImVec2(max.x + 2.0f, max.y + 4.0f), ImGui::GetColorU32(ui::kShadowSoft), ui::kRadiusInput);

	ImGui::TextColored(ui::kTextSecondary, "Composer");
	ImGui::SameLine();
	ImGui::PushID(chat.id.c_str());
	bool rag_enabled_for_chat = chat.rag_enabled;

	if (ImGui::Checkbox("RAG", &rag_enabled_for_chat))
	{
		chat.rag_enabled = rag_enabled_for_chat;
		chat.updated_at = TimestampNow();
		SaveAndUpdateStatus(app, chat, chat.rag_enabled ? "RAG enabled for this chat." : "RAG disabled for this chat.", "Failed to save chat RAG setting.");
	}

	ImGui::SameLine();

	if (DrawButton("Sources", ImVec2(92.0f, 28.0f), ButtonKind::Ghost))
	{
		ImGui::OpenPopup("composer_rag_sources_popup");
	}

	if (ImGui::BeginPopup("composer_rag_sources_popup"))
	{
		const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
		const fs::path fallback_source_root = ResolveProjectRagSourceRoot(app, workspace_root);
		auto append_unique_path = [](std::vector<fs::path>& paths, const fs::path& candidate)
		{
			const fs::path normalized = NormalizeAbsolutePath(candidate);

			if (normalized.empty())
			{
				return;
			}

			const std::string key = normalized.generic_string();

			for (const fs::path& existing : paths)
			{
				if (NormalizeAbsolutePath(existing).generic_string() == key)
				{
					return;
				}
			}

			paths.push_back(normalized);
		};

		std::vector<fs::path> candidate_folders = DiscoverRagSourceFolders(workspace_root);
		append_unique_path(candidate_folders, fallback_source_root);

		for (const std::string& selected_source : chat.rag_source_directories)
		{
			append_unique_path(candidate_folders, ExpandLeadingTildePath(selected_source));
		}

		std::sort(candidate_folders.begin(), candidate_folders.end(), [](const fs::path& lhs, const fs::path& rhs) { return lhs.generic_string() < rhs.generic_string(); });

		ImGui::TextColored(ui::kTextPrimary, "RAG source folders");

		if (!app.settings.rag_enabled)
		{
			ImGui::TextColored(ui::kWarning, "Global RAG is currently disabled in Settings.");
		}

		ImGui::TextColored(ui::kTextMuted, "Toggle folder databases on/off:");

		if (candidate_folders.empty())
		{
			ImGui::TextColored(ui::kTextMuted, "No folders discovered for this workspace.");
		}
		else if (ImGui::BeginCombo("Discovered folders", "Select folders"))
		{
			for (std::size_t i = 0; i < candidate_folders.size(); ++i)
			{
				const fs::path folder = NormalizeAbsolutePath(candidate_folders[i]);
				bool enabled = ChatHasRagSourceDirectory(chat, folder);
				const std::string label = folder.string() + "##rag_folder_toggle_" + std::to_string(i);

				if (ImGui::Checkbox(label.c_str(), &enabled))
				{
					bool changed = false;

					if (enabled)
					{
						changed = AddChatRagSourceDirectory(chat, folder);
					}
					else
					{
						changed = RemoveChatRagSourceDirectory(chat, folder);
					}

					if (changed)
					{
						chat.updated_at = TimestampNow();
						SaveAndUpdateStatus(app, chat, "Updated chat RAG folder selection.", "Failed to save chat RAG sources.");
					}
				}
			}

			ImGui::EndCombo();
		}

		ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

		if (chat.rag_source_directories.empty())
		{
			ImGui::TextColored(ui::kTextMuted, "Using default source folder:");
			ImGui::TextWrapped("%s", fallback_source_root.string().c_str());
			const std::string fallback_db_name = RagDatabaseNameForSourceRoot(app.settings, fallback_source_root);

			if (!fallback_db_name.empty())
			{
				ImGui::TextColored(ui::kTextMuted, "Database: %s", fallback_db_name.c_str());
			}
		}
		else
		{
			ImGui::TextColored(ui::kTextMuted, "Enabled folders:");
			ImGui::BeginChild("composer_rag_source_list", ImVec2(560.0f, 140.0f), true);

			for (std::size_t i = 0; i < chat.rag_source_directories.size(); ++i)
			{
				const fs::path source_root = NormalizeAbsolutePath(ExpandLeadingTildePath(chat.rag_source_directories[i]));
				const std::string source_display = source_root.string();
				const std::string database_name = RagDatabaseNameForSourceRoot(app.settings, source_root);

				ImGui::TextWrapped("%s", source_display.c_str());

				if (!database_name.empty())
				{
					ImGui::TextColored(ui::kTextMuted, "DB: %s", database_name.c_str());
				}

				std::error_code exists_ec;
				const bool source_exists = fs::exists(source_root, exists_ec) && fs::is_directory(source_root, exists_ec);

				if (!source_exists)
				{
					ImGui::TextColored(ui::kWarning, "Folder is missing.");
				}

				const std::string scan_label = "Scan##rag_scan_source_" + std::to_string(i);

				if (!source_exists)
				{
					ImGui::BeginDisabled();
				}

				if (DrawButton(scan_label.c_str(), ImVec2(78.0f, 26.0f), ButtonKind::Ghost))
				{
					std::string scan_error;

					if (!TriggerProjectRagScan(app, false, source_root, &scan_error))
					{
						app.status_line = "RAG scan failed to start: " + scan_error;
					}
					else
					{
						app.status_line = "RAG scan started.";
					}

					ImGui::CloseCurrentPopup();
				}

				if (!source_exists)
				{
					ImGui::EndDisabled();
				}

				if (i + 1 < chat.rag_source_directories.size())
				{
					ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
					DrawSoftDivider();
					ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
				}
			}

			ImGui::EndChild();
		}

		const bool has_custom_sources = !chat.rag_source_directories.empty();

		if (!has_custom_sources)
		{
			ImGui::BeginDisabled();
		}

		if (DrawButton("Reset", ImVec2(82.0f, 28.0f), ButtonKind::Ghost))
		{
			chat.rag_source_directories.clear();
			chat.updated_at = TimestampNow();
			SaveAndUpdateStatus(app, chat, "Reset to default RAG source folder.", "Failed to save chat RAG sources.");
		}

		if (!has_custom_sources)
		{
			ImGui::EndDisabled();
		}

		ImGui::EndPopup();
	}

	ImGui::PopID();
	ImGui::SameLine();
	ImGui::TextColored(ui::kTextMuted, "Ctrl+Enter to send");
	PushInputChrome(ui::kRadiusInput);
	const bool send_visible = FrontendActionVisible(app, "send_prompt", true);

	if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
	{
		// Windows-only composer fit: reserve right-side width for the SEND button so
		// the multiline input shrinks first at larger UI scales. If this starts to
		// happen on macOS later, we can make this universal.
		const float send_gap = ScaleUiLength(8.0f);
		const float send_button_w = ScaleUiLength(80.0f);
		const float composer_h = std::max(ScaleUiLength(110.0f), ImGui::GetTextLineHeight() * 5.2f);
		const float reserved_send_w = send_visible ? (send_button_w + send_gap + ScaleUiLength(2.0f)) : 0.0f;
		const float input_w = std::max(ScaleUiLength(180.0f), ImGui::GetContentRegionAvail().x - reserved_send_w);
		ImGui::InputTextMultiline("##composer", &app.composer_text, ImVec2(input_w, composer_h), ImGuiInputTextFlags_AllowTabInput);
	}
	else
	{
		ImGui::InputTextMultiline("##composer", &app.composer_text, ImVec2(-96.0f, 110.0f), ImGuiInputTextFlags_AllowTabInput);
	}

	PopInputChrome();

	float send_same_line_gap = 0.0f;

	if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
	{
		send_same_line_gap = ScaleUiLength(8.0f);
	}

	ImGui::SameLine(0.0f, send_same_line_gap);
	const bool can_send = !HasPendingCallForChat(app, chat.id);

	if (!can_send)
	{
		ImGui::BeginDisabled();
	}

	if (send_visible)
	{
		float send_y_nudge = 36.0f;

		if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
		{
			const float composer_h = std::max(ScaleUiLength(110.0f), ImGui::GetTextLineHeight() * 5.2f);
			const float send_h = ScaleUiLength(42.0f);
			send_y_nudge = std::max(0.0f, (composer_h - send_h) * 0.5f);
		}

		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + send_y_nudge);

		if (DrawButton("SEND", ImVec2(80.0f, 42.0f), ButtonKind::Accent))
		{
			StartGeminiRequest(app);
		}
	}

	if (!can_send)
	{
		ImGui::EndDisabled();
	}

	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl && !HasPendingCallForChat(app, chat.id))
	{
		StartGeminiRequest(app);
	}

	EndPanel();
}
