#pragma once
#include <algorithm>
#include <functional>
#include "app/chat_domain_service.h"
#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"

/// <summary>
/// Draws the folder list and nested chat tree region in the sidebar.
/// </summary>
inline void DrawChatSidebarTree(AppState& app, std::string& chat_to_delete, std::string& chat_to_open_options)
{
	if (app.sidebar_search_query != app.last_sidebar_search_query)
	{
		app.last_sidebar_search_query = app.sidebar_search_query;
		app.filtered_chat_ids.clear();
		if (!app.sidebar_search_query.empty())
		{
			for (const auto& chat : app.chats)
			{
				bool match = uam::strings::ContainsCaseInsensitive(chat.title, app.sidebar_search_query);
				if (!match)
				{
					for (const auto& msg : chat.messages)
					{
						if (uam::strings::ContainsCaseInsensitive(msg.content, app.sidebar_search_query))
						{
							match = true;
							break;
						}
					}
				}
				if (match)
				{
					app.filtered_chat_ids.insert(chat.id);
				}
			}

			// Include ancestors to preserve tree structure
			std::vector<std::string> ancestors_to_add;
			for (const auto& id : app.filtered_chat_ids)
			{
				int current_idx = ChatDomainService().FindChatIndexById(app, id);
				while (current_idx >= 0 && !app.chats[current_idx].parent_chat_id.empty())
				{
					std::string parent_id = app.chats[current_idx].parent_chat_id;
					if (app.filtered_chat_ids.find(parent_id) != app.filtered_chat_ids.end()) break;
					ancestors_to_add.push_back(parent_id);
					current_idx = ChatDomainService().FindChatIndexById(app, parent_id);
				}
			}
			for (const auto& id : ancestors_to_add) app.filtered_chat_ids.insert(id);
		}
	}

	BeginPanel("sidebar_folder_scroll", ImVec2(0.0f, 0.0f), PanelTone::Primary, false, ImGuiWindowFlags_AlwaysVerticalScrollbar, ImVec2(4.0f, 4.0f), ui::kRadiusSmall);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

	const bool is_searching = !app.sidebar_search_query.empty();
	bool any_folder_visible = false;

	for (ChatFolder& folder : app.folders)
	{
		const int chat_count = ChatDomainService().CountChatsInFolder(app, folder.id);
		
		bool should_show_folder = (chat_count > 0);
		if (is_searching)
		{
			should_show_folder = false;
			for (const auto& chat : app.chats)
			{
				if (chat.folder_id == folder.id && app.filtered_chat_ids.find(chat.id) != app.filtered_chat_ids.end())
				{
					should_show_folder = true;
					break;
				}
			}
		}

		if (!should_show_folder) continue;
		any_folder_visible = true;

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

		if (!folder.collapsed || is_searching)
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
					if (is_searching && app.filtered_chat_ids.find(app.chats[i].id) == app.filtered_chat_ids.end())
					{
						continue;
					}
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
					bool collapsed_children = !is_searching && (app.collapsed_branch_chat_ids.find(sidebar_chat.id) != app.collapsed_branch_chat_ids.end());
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

	if (is_searching && !any_folder_visible)
	{
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace16));
		ImGui::Indent(ui::kSpace12);
		ImGui::TextColored(ui::kTextMuted, "No chats found matching \"%s\"", app.sidebar_search_query.c_str());
		ImGui::Unindent(ui::kSpace12);
	}

	ImGui::PopStyleVar();
	EndPanel();
}
