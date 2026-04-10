#include "cef/state_serializer.h"

namespace uam
{

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

std::string RoleStr(MessageRole role)
{
	switch (role)
	{
	case MessageRole::User:      return "user";
	case MessageRole::Assistant: return "assistant";
	case MessageRole::System:    return "system";
	}
	return "user";
}

const uam::CliTerminalState* FindTerminalForSession(const uam::AppState& app, const std::string& session_id)
{
	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal == nullptr)
		{
			continue;
		}

		if (terminal->frontend_chat_id == session_id || terminal->attached_chat_id == session_id)
		{
			return terminal.get();
		}
	}

	return nullptr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// StateSerializer implementation
// ---------------------------------------------------------------------------

nlohmann::json StateSerializer::Serialize(const AppState& app)
{
	nlohmann::json j;

	// Folders
	auto folders_arr = nlohmann::json::array();
	for (const auto& folder : app.folders)
		folders_arr.push_back(SerializeFolder(folder));
	j["folders"] = folders_arr;

	// Chat sessions
	auto chats_arr = nlohmann::json::array();
	for (const auto& chat : app.chats)
	{
		nlohmann::json chat_json = SerializeSession(chat);

		if (const CliTerminalState* terminal = FindTerminalForSession(app, chat.id); terminal != nullptr)
		{
			nlohmann::json terminal_json;
			terminal_json["terminalId"] = terminal->terminal_id;
			terminal_json["frontendChatId"] = terminal->frontend_chat_id;
			terminal_json["sourceChatId"] = terminal->attached_chat_id;
			terminal_json["running"] = terminal->running;
			terminal_json["lastError"] = terminal->last_error;
			chat_json["cliTerminal"] = terminal_json;
		}

		chats_arr.push_back(std::move(chat_json));
	}
	j["chats"] = chats_arr;

	// Selected chat id (resolved from index)
	if (app.selected_chat_index >= 0 &&
	    app.selected_chat_index < static_cast<int>(app.chats.size()))
	{
		j["selectedChatId"] = app.chats[static_cast<std::size_t>(app.selected_chat_index)].id;
	}
	else
	{
		j["selectedChatId"] = nullptr;
	}

	// Provider profiles
	auto providers_arr = nlohmann::json::array();
	for (const auto& profile : app.provider_profiles)
		providers_arr.push_back(SerializeProvider(profile));
	j["providers"] = providers_arr;

	// Settings slice that the UI cares about
	{
		nlohmann::json settings;
		settings["activeProviderId"] = app.settings.active_provider_id;
		settings["theme"]            = app.settings.ui_theme;
		settings["ragEnabled"]       = app.settings.rag_enabled;
		j["settings"]                = settings;
	}

	return j;
}

nlohmann::json StateSerializer::SerializeSession(const ChatSession& session)
{
	nlohmann::json j;
	j["id"]         = session.id;
	j["title"]      = session.title;
	j["folderId"]   = session.folder_id;
	j["providerId"] = session.provider_id;
	j["createdAt"]  = session.created_at;
	j["updatedAt"]  = session.updated_at;

	auto msgs = nlohmann::json::array();
	for (const auto& msg : session.messages)
	{
		nlohmann::json m;
		m["role"]      = RoleStr(msg.role);
		m["content"]   = msg.content;
		m["createdAt"] = msg.created_at;
		msgs.push_back(m);
	}
	j["messages"] = msgs;

	return j;
}

nlohmann::json StateSerializer::SerializeFolder(const ChatFolder& folder)
{
	nlohmann::json j;
	j["id"]        = folder.id;
	j["title"]     = folder.title;
	j["collapsed"] = folder.collapsed;
	return j;
}

nlohmann::json StateSerializer::SerializeProvider(const ProviderProfile& profile)
{
	nlohmann::json j;
	j["id"]        = profile.id;
	j["name"]      = profile.title;
	j["shortName"] = profile.title;  // React derives a short name from this
	j["outputMode"] = profile.output_mode;
	return j;
}

} // namespace uam
