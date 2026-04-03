#pragma once
#include "common/platform/platform_services.h"

/// <summary>
/// Draws an inline RAG toggle chip that acts as a clickable pill button.
/// Returns true if the toggle state changed.
/// </summary>
inline bool DrawRagToggleChip(const char* chip_id, const bool enabled)
{
	ImGui::PushID(chip_id);
	const bool light = IsLightPaletteActive();
	const char* label = enabled ? "\xe2\x97\x8f RAG" : "\xe2\x97\x8b RAG";
	const ImVec2 text_size = ImGui::CalcTextSize(label);
	const float pad_x = ScaleUiLength(8.0f);
	const float pad_y = ScaleUiLength(3.0f);
	const ImVec2 chip_size(text_size.x + pad_x * 2.0f, text_size.y + pad_y * 2.0f);
	const ImVec2 min = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("rag_chip", chip_size);
	const bool clicked = ImGui::IsItemClicked();
	const bool hovered = ImGui::IsItemHovered();
	const ImVec2 max(min.x + chip_size.x, min.y + chip_size.y);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float rounding = ScaleUiLength(ui::kRadiusSmall);

	if (enabled)
	{
		draw->AddRectFilled(min, max, ImGui::GetColorU32(ui::kAccentSoft), rounding);
		draw->AddRect(min, max, ImGui::GetColorU32(light ? Rgb(66, 126, 228, 0.35f) : Rgb(94, 160, 255, 0.30f)), rounding, 0, 1.0f);
		draw->AddText(ImVec2(min.x + pad_x, min.y + pad_y), ImGui::GetColorU32(ui::kAccent), label);
	}
	else
	{
		const ImVec4 hover_fill = light ? Rgb(0, 0, 0, 0.04f) : Rgb(255, 255, 255, 0.04f);
		draw->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? hover_fill : ui::kTransparent), rounding);
		draw->AddRect(min, max, ImGui::GetColorU32(ui::kBorder), rounding, 0, 1.0f);
		draw->AddText(ImVec2(min.x + pad_x, min.y + pad_y), ImGui::GetColorU32(ui::kTextMuted), label);
	}

	ImGui::PopID();
	return clicked;
}

/// <summary>
/// Draws the composer container — text area + single-row bottom chip toolbar (T3-style).
/// </summary>
inline void DrawInputContainer(AppState& app, ChatSession& chat)
{
	BeginPanel("input_container", ImVec2(0.0f, 162.0f), PanelTone::Secondary, true, 0, ImVec2(10.0f, 10.0f), ui::kRadiusInput);

	ImGui::PushID(chat.id.c_str());

	// --- Status line (if any) ---
	if (!app.status_line.empty())
	{
		ImGui::TextColored(ui::kTextMuted, "%s", app.status_line.c_str());
	}

	// --- Text area ---
	PushInputChrome(ui::kRadiusInput);

	const bool send_visible = FrontendActionVisible(app, "send_prompt", true);
	// Reserve chip_h + 14px overhead (covers 2x ImGui ItemSpacing + bottom padding rounding)
	const float chip_h = ScaleUiLength(26.0f);
	const float input_h = std::max(ScaleUiLength(60.0f), ImGui::GetContentRegionAvail().y - chip_h - ScaleUiLength(14.0f));

	ImGui::InputTextMultiline("##composer", &app.composer_text, ImVec2(-1.0f, input_h), ImGuiInputTextFlags_AllowTabInput);

	PopInputChrome();

	// --- Bottom chip toolbar (single layout line, no extra Dummy spacer) ---

	// Left: RAG toggle chip
	bool rag_enabled_for_chat = chat.rag_enabled;

	if (DrawRagToggleChip("rag_toggle", rag_enabled_for_chat))
	{
		chat.rag_enabled = !chat.rag_enabled;
		chat.updated_at = TimestampNow();
		SaveAndUpdateStatus(app, chat, chat.rag_enabled ? "RAG enabled for this chat." : "RAG disabled for this chat.", "Failed to save chat RAG setting.");
	}

	ImGui::SameLine(0.0f, ScaleUiLength(6.0f));

	// Left: Sources button
	if (DrawButton("Sources \xe2\x96\xbe", ImVec2(ScaleUiLength(82.0f), chip_h), ButtonKind::Ghost))
	{
		ImGui::OpenPopup("composer_rag_sources_popup");
	}

	// Right: hint text + send button — all on the same SameLine row
	if (send_visible)
	{
		const float send_btn_w = ScaleUiLength(32.0f);
		const float hint_w = ImGui::CalcTextSize("\xe2\x8c\x83\xe2\x86\xb5").x + ScaleUiLength(8.0f);
		const float gap = ScaleUiLength(8.0f);
		const float right_edge = ImGui::GetContentRegionMax().x;

		// Right-align hint then send button on the same row
		ImGui::SameLine(right_edge - send_btn_w - hint_w - gap);
		ImGui::TextColored(ui::kTextMuted, "\xe2\x8c\x83\xe2\x86\xb5");

		const bool can_send = !HasPendingCallForChat(app, chat.id);

		if (!can_send)
		{
			ImGui::BeginDisabled();
		}

		ImGui::SameLine(right_edge - send_btn_w);

		if (DrawButton("\xe2\x86\x91", ImVec2(send_btn_w, chip_h), ButtonKind::Accent))
		{
			ProviderRequestService().StartSelectedChatRequest(app);
		}

		if (!can_send)
		{
			ImGui::EndDisabled();
		}
	}

	// Sources popup (logic unchanged)
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
			append_unique_path(candidate_folders, PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(selected_source));
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
				const fs::path source_root = NormalizeAbsolutePath(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(chat.rag_source_directories[i]));
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

	// Ctrl+Enter shortcut
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl && !HasPendingCallForChat(app, chat.id))
	{
		ProviderRequestService().StartSelectedChatRequest(app);
	}

	EndPanel();
}
