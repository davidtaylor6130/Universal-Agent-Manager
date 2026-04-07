#pragma once

#include "app/chat_domain_service.h"
#include "app/provider_profile_migration_service.h"

/// <summary>
/// Draws duplicate-draft conflict handling when creating a new chat.
/// </summary>
inline void DrawDuplicateNewChatPopup(AppState& app)
{
	if (app.open_duplicate_new_chat_popup)
	{
		ImGui::OpenPopup("duplicate_new_chat_popup");
		app.open_duplicate_new_chat_popup = false;
	}

	if (!ImGui::BeginPopupModal("duplicate_new_chat_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	const std::string folder_id = app.pending_duplicate_new_chat_folder_id;
	const std::string provider_id = ResolveNewChatProviderId(app, app.pending_duplicate_new_chat_provider_id);
	const std::string existing_chat_id = app.pending_duplicate_new_chat_existing_id;
	std::string folder_label = "Unknown folder";
	std::string provider_label = provider_id.empty() ? "(none)" : provider_id;
	std::string existing_label = existing_chat_id.empty() ? "(none)" : CompactPreview(existing_chat_id, 42);

	if (const ChatFolder* folder = ChatDomainService().FindFolderById(app, folder_id); folder != nullptr)
	{
		folder_label = ChatDomainService().FolderTitleOrFallback(*folder);
	}

	if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, provider_id); profile != nullptr)
	{
		provider_label = profile->title.empty() ? profile->id : profile->title;
	}

	if (const int chat_index = ChatDomainService().FindChatIndexById(app, existing_chat_id); chat_index >= 0)
	{
		existing_label = CompactPreview(app.chats[chat_index].title, 42);
	}

	ImGui::TextColored(ui::kTextPrimary, "Empty draft already exists");
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
	ImGui::TextWrapped("A matching empty draft was found for this folder and provider. Choose what to do next.");
	ImGui::TextColored(ui::kTextMuted, "Folder: %s", folder_label.c_str());
	ImGui::TextColored(ui::kTextMuted, "Provider: %s", provider_label.c_str());
	ImGui::TextColored(ui::kTextMuted, "Existing draft: %s", existing_label.c_str());
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));

	if (DrawButton("Create New", ImVec2(110.0f, 32.0f), ButtonKind::Primary))
	{
		if (!folder_id.empty())
		{
			app.new_chat_folder_id = folder_id;
		}

		ChatDomainService().EnsureNewChatFolderSelection(app);
		CreateAndSelectChatWithProvider(app, provider_id, NewChatDuplicatePolicy::CreateNew);
		ImGui::CloseCurrentPopup();
	}

	ImGui::SetItemDefaultFocus();
	ImGui::SameLine();

	if (DrawButton("Reuse Existing", ImVec2(122.0f, 32.0f), ButtonKind::Ghost))
	{
		if (!folder_id.empty())
		{
			app.new_chat_folder_id = folder_id;
		}

		ChatDomainService().EnsureNewChatFolderSelection(app);
		CreateAndSelectChatWithProvider(app, provider_id, NewChatDuplicatePolicy::ReuseExisting);
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();

	if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
	{
		ClearPendingDuplicateNewChatDecision(app);
		app.status_line = "Create chat cancelled.";
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

/// <summary>
/// Draws the create-chat modal used to lock provider at chat creation time.
/// </summary>
inline void DrawSidebarNewChatPopup(AppState& app)
{
	if (app.open_new_chat_popup)
	{
		app.pending_new_chat_provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);
		ImGui::OpenPopup("new_chat_popup");
		app.open_new_chat_popup = false;
	}

	if (!ImGui::BeginPopupModal("new_chat_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		DrawDuplicateNewChatPopup(app);
		return;
	}

	ChatDomainService().EnsureNewChatFolderSelection(app);
	const std::string provider_id = ResolveNewChatProviderId(app, app.pending_new_chat_provider_id);
	app.pending_new_chat_provider_id = provider_id;

	std::string folder_label = "General";

	if (const ChatFolder* folder = ChatDomainService().FindFolderById(app, ChatDomainService().FolderForNewChat(app)); folder != nullptr)
	{
		folder_label = ChatDomainService().FolderTitleOrFallback(*folder);
	}

	ImGui::TextColored(ui::kTextPrimary, "Create chat");
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
	ImGui::TextColored(ui::kTextMuted, "Folder: %s", folder_label.c_str());
	ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

	const ProviderProfile* selected_profile = ProviderProfileStore::FindById(app.provider_profiles, provider_id);
	std::string selected_label = "(none)";

	if (selected_profile != nullptr)
	{
		selected_label = (selected_profile->title.empty() ? selected_profile->id : selected_profile->title) + std::string(ProviderRuntime::UsesCliOutput(*selected_profile) ? " [CLI]" : " [Structured]");
	}

	if (ImGui::BeginCombo("Provider", selected_label.c_str()))
	{
		for (const ProviderProfile& profile : app.provider_profiles)
		{
			if (!ProviderProfileMigrationService().ShouldShowProviderProfileInUi(profile))
			{
				continue;
			}

			const std::string profile_label = (profile.title.empty() ? profile.id : profile.title) + std::string(ProviderRuntime::UsesCliOutput(profile) ? " [CLI]" : " [Structured]");
			const bool selected = (provider_id == profile.id);

			if (ImGui::Selectable(profile_label.c_str(), selected))
			{
				app.pending_new_chat_provider_id = profile.id;
			}

			if (selected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}

		ImGui::EndCombo();
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));

	if (DrawButton("Create", ImVec2(96.0f, 32.0f), ButtonKind::Primary))
	{
		if (ConfirmCreateNewChat(app))
		{
			ImGui::CloseCurrentPopup();
		}
	}

	ImGui::SameLine();

	if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost))
	{
		app.pending_new_chat_provider_id.clear();
		ClearPendingDuplicateNewChatDecision(app);
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
	DrawDuplicateNewChatPopup(app);
}
