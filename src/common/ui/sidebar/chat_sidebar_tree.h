#pragma once
#include "app/chat_domain_service.h"
#include "common/platform/platform_services.h"

/// <summary>
/// Draws the folder list and nested chat tree region in the sidebar.
/// </summary>
inline void DrawChatSidebarTree(AppState& app, std::string& chat_to_delete, std::string& chat_to_open_options)
{
	BeginPanel("sidebar_folder_scroll", ImVec2(0.0f, 0.0f), PanelTone::Primary, false, ImGuiWindowFlags_AlwaysVerticalScrollbar, ImVec2(4.0f, 4.0f), ui::kRadiusSmall);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

	for (ChatFolder& folder : app.folders)
	{
		const int chat_count = ChatDomainService().CountChatsInFolder(app, folder.id);
		const FolderHeaderAction folder_action = DrawFolderHeaderItem(folder, chat_count);

		if (folder_action.quick_create)
		{
			CreateAndSelectChatInFolder(app, folder.id);
		}
		else if (folder_action.open_settings)
		{
			app.pending_folder_settings_id = folder.id;
			app.folder_settings_title_input = folder.title;
			app.folder_settings_directory_input = folder.directory;
			app.open_folder_settings_popup = true;
		}
		else if (folder_action.toggle)
		{
			app.new_chat_folder_id = folder.id;
			folder.collapsed = !folder.collapsed;
			ChatFolderStore::Save(app.data_root, app.folders);
		}

		if (!folder.collapsed)
		{
			float folder_indent = 8.0f;

			if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
			{
				folder_indent = ScaleUiLength(8.0f);
			}

			ImGui::Indent(folder_indent);

			std::vector<int> folder_chat_indices;
			std::unordered_set<std::string> folder_chat_ids;

			for (int i = 0; i < static_cast<int>(app.chats.size()); ++i)
			{
				if (app.chats[i].folder_id == folder.id)
				{
					folder_chat_indices.push_back(i);
					folder_chat_ids.insert(app.chats[i].id);
				}
			}

			std::unordered_map<std::string, std::vector<int>> children_by_parent;

			for (const int chat_index : folder_chat_indices)
			{
				const ChatSession& chat = app.chats[chat_index];
				std::string parent_id = chat.parent_chat_id;

				if (parent_id.empty() || folder_chat_ids.find(parent_id) == folder_chat_ids.end())
				{
					parent_id.clear();
				}

				children_by_parent[parent_id].push_back(chat_index);
			}

			if (auto roots_it = children_by_parent.find(""); roots_it != children_by_parent.end())
			{
				std::sort(roots_it->second.begin(), roots_it->second.end(), [&](const int lhs, const int rhs) { return app.chats[lhs].updated_at > app.chats[rhs].updated_at; });
			}

			for (auto& pair : children_by_parent)
			{
				if (pair.first.empty())
				{
					continue;
				}

				std::sort(pair.second.begin(), pair.second.end(), [&](const int lhs, const int rhs) { return app.chats[lhs].created_at < app.chats[rhs].created_at; });
			}

			std::function<void(const std::string&, int)> draw_tree = [&](const std::string& parent_id, const int depth)
			{
				const auto it = children_by_parent.find(parent_id);

				if (it == children_by_parent.end())
				{
					return;
				}

				for (const int chat_index : it->second)
				{
					ChatSession& sidebar_chat = app.chats[chat_index];
					const auto children_it = children_by_parent.find(sidebar_chat.id);
					const bool has_children = (children_it != children_by_parent.end() && !children_it->second.empty());
					bool collapsed_children = (app.collapsed_branch_chat_ids.find(sidebar_chat.id) != app.collapsed_branch_chat_ids.end());
					bool toggle_children = false;
					const std::string sidebar_item_id = "session_" + sidebar_chat.id + "##" + std::to_string(chat_index);
					const SidebarItemAction item_action = DrawSidebarItem(app, sidebar_chat, chat_index == app.selected_chat_index, sidebar_item_id, depth, has_children, collapsed_children, &toggle_children);

					if (toggle_children && has_children)
					{
						if (collapsed_children)
						{
							app.collapsed_branch_chat_ids.erase(sidebar_chat.id);
							collapsed_children = false;
						}
						else
						{
							app.collapsed_branch_chat_ids.insert(sidebar_chat.id);
							collapsed_children = true;
						}
					}

					if (item_action.select)
					{
						ChatDomainService().SelectChatById(app, sidebar_chat.id);
						PersistenceCoordinator().SaveSettings(app);

						if (const ChatSession* selected = ChatDomainService().SelectedChat(app); selected != nullptr && ProviderResolutionService().ChatUsesCliOutput(app, *selected))
						{
							MarkSelectedCliTerminalForLaunch(app);
						}
					}

					if (item_action.request_delete)
					{
						chat_to_delete = sidebar_chat.id;
					}

					if (item_action.request_open_options)
					{
						chat_to_open_options = sidebar_chat.id;
					}

					if (has_children && !collapsed_children)
					{
						draw_tree(sidebar_chat.id, depth + 1);
					}
				}
			};

			draw_tree("", 0);

			if (folder_chat_indices.empty())
			{
				ImGui::TextColored(ui::kTextMuted, "No chats in folder");
				float empty_spacing = 4.0f;

				if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
				{
					empty_spacing = ScaleUiLength(4.0f);
				}

				ImGui::Dummy(ImVec2(0.0f, empty_spacing));
			}

			ImGui::Unindent(folder_indent);
		}
	}

	ImGui::PopStyleVar();
	EndPanel();
}
