#include "cef/state_serializer.h"

#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_chat_sync.h"

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

const uam::CliTerminalState* FindTerminalForChat(const uam::AppState& app, const ChatSession& chat)
{
	const std::string native_session_id = chat.native_session_id;

	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal == nullptr)
		{
			continue;
		}

		if (terminal->frontend_chat_id == chat.id || terminal->attached_chat_id == chat.id)
		{
			return terminal.get();
		}
	}

	if (!native_session_id.empty())
	{
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal == nullptr)
			{
				continue;
			}

			if (terminal->attached_session_id == native_session_id)
			{
				return terminal.get();
			}
		}
	}

	return nullptr;
}

nlohmann::json SerializeCliDebugState(const AppState& app)
{
	nlohmann::json cli_debug;
	cli_debug["selectedChatId"] = CliSelectedChatId(app);
	cli_debug["terminalCount"] = app.cli_terminals.size();

	std::size_t running_count = 0;
	std::size_t busy_count = 0;
	auto terminals = nlohmann::json::array();

	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
		{
			continue;
		}

		const CliTerminalState& terminal = *terminal_ptr;
		if (terminal.running)
		{
			++running_count;
		}

		if (terminal.turn_state == CliTerminalTurnState::Busy)
		{
			++busy_count;
		}

		nlohmann::json terminal_json;
		terminal_json["terminalId"] = terminal.terminal_id;
		terminal_json["frontendChatId"] = terminal.frontend_chat_id;
		terminal_json["sourceChatId"] = terminal.attached_chat_id;
		terminal_json["attachedSessionId"] = terminal.attached_session_id;
		terminal_json["providerId"] = CliProviderIdForDiagnostics(app, terminal);
		terminal_json["nativeSessionId"] = CliNativeSessionIdForDiagnostics(app, terminal);
		terminal_json["processId"] = CliProcessHandleLabel(terminal);
		terminal_json["running"] = terminal.running;
		terminal_json["uiAttached"] = terminal.ui_attached;
		terminal_json["turnState"] = CliTurnStateLabel(terminal);
		terminal_json["inputReady"] = terminal.input_ready;
		terminal_json["generationInProgress"] = terminal.generation_in_progress;
		terminal_json["lastUserInputAt"] = terminal.last_user_input_time_s;
		terminal_json["lastAiOutputAt"] = terminal.last_ai_output_time_s;
		terminal_json["lastPolledAt"] = terminal.last_polled_time_s;
		terminal_json["lastError"] = terminal.last_error;
		terminals.push_back(std::move(terminal_json));
	}

	cli_debug["runningTerminalCount"] = running_count;
	cli_debug["busyTerminalCount"] = busy_count;
	cli_debug["terminals"] = std::move(terminals);
	return cli_debug;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// StateSerializer implementation
// ---------------------------------------------------------------------------

nlohmann::json StateSerializer::Serialize(const AppState& app)
{
	nlohmann::json j;
	j["stateRevision"] = app.state_revision;

	// Folders
	auto folders_arr = nlohmann::json::array();
	for (const auto& folder : app.folders)
		folders_arr.push_back(SerializeFolder(folder));
	j["folders"] = folders_arr;

	// Chat sessions
	auto chats_arr = nlohmann::json::array();
	const std::string selected_chat_id = (app.selected_chat_index >= 0 && app.selected_chat_index < static_cast<int>(app.chats.size()))
		? app.chats[static_cast<std::size_t>(app.selected_chat_index)].id
		: "";
	for (const auto& chat : app.chats)
	{
		nlohmann::json chat_json = SerializeSession(chat);

		const bool ready_since_last_select = app.chats_with_unseen_updates.find(chat.id) != app.chats_with_unseen_updates.end();
		const bool selected = chat.id == selected_chat_id;
		const bool has_pending_call = HasPendingCallForChat(app, chat.id);

		if (const CliTerminalState* terminal = FindTerminalForChat(app, chat); terminal != nullptr)
		{
			const bool terminal_processing = terminal->running &&
				(terminal->turn_state == CliTerminalTurnState::Busy || terminal->generation_in_progress);
			const bool processing = has_pending_call || terminal_processing;
			nlohmann::json terminal_json;
			terminal_json["terminalId"] = terminal->terminal_id;
			terminal_json["frontendChatId"] = terminal->frontend_chat_id;
			terminal_json["sourceChatId"] = terminal->attached_chat_id;
			terminal_json["running"] = terminal->running;
			terminal_json["turnState"] = terminal->turn_state == CliTerminalTurnState::Busy ? "busy" : "idle";
			terminal_json["processing"] = processing;
			terminal_json["readySinceLastSelect"] = ready_since_last_select;
			terminal_json["active"] = terminal->running && !selected && !processing && !ready_since_last_select;
			terminal_json["lastError"] = terminal->last_error;
			chat_json["cliTerminal"] = terminal_json;
		}
		else
		{
			const bool processing = has_pending_call;
			nlohmann::json terminal_json;
			terminal_json["running"] = false;
			terminal_json["turnState"] = "idle";
			terminal_json["processing"] = processing;
			terminal_json["readySinceLastSelect"] = ready_since_last_select;
			terminal_json["active"] = false;
			terminal_json["lastError"] = "";
			chat_json["cliTerminal"] = terminal_json;
		}

		chats_arr.push_back(std::move(chat_json));
	}
	j["chats"] = chats_arr;
	j["cliDebug"] = SerializeCliDebugState(app);

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
	j["directory"] = folder.directory;
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
