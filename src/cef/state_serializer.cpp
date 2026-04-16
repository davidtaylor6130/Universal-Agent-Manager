#include "cef/state_serializer.h"

#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_chat_sync.h"

#include <cstdint>
#include <iomanip>
#include <sstream>

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

constexpr std::uint64_t kFingerprintHashOffset = 1469598103934665603ull;
constexpr std::uint64_t kFingerprintHashPrime = 1099511628211ull;

void FingerprintHashBytes(std::uint64_t& hash, const unsigned char* data, const std::size_t len)
{
	for (std::size_t i = 0; i < len; ++i)
	{
		hash ^= static_cast<std::uint64_t>(data[i]);
		hash *= kFingerprintHashPrime;
	}
}

void FingerprintHashString(std::uint64_t& hash, const std::string& value)
{
	FingerprintHashBytes(hash, reinterpret_cast<const unsigned char*>(value.data()), value.size());

	const unsigned char separator = 0xFF;
	FingerprintHashBytes(hash, &separator, 1);
}

void FingerprintHashBool(std::uint64_t& hash, const bool value)
{
	const unsigned char byte = value ? 1u : 0u;
	FingerprintHashBytes(hash, &byte, 1);
}

std::string FingerprintHashHex(const std::uint64_t hash)
{
	std::ostringstream out;
	out << std::hex << std::setw(16) << std::setfill('0') << hash;
	return out.str();
}

std::string MessageDigestForFingerprint(const ChatSession& session)
{
	std::uint64_t hash = kFingerprintHashOffset;

	FingerprintHashString(hash, session.updated_at);
	FingerprintHashString(hash, std::to_string(session.messages.size()));

	if (!session.messages.empty())
	{
		const Message& last_message = session.messages.back();
		FingerprintHashString(hash, RoleStr(last_message.role));
		FingerprintHashString(hash, last_message.created_at);
		FingerprintHashString(hash, last_message.provider);
		FingerprintHashString(hash, std::to_string(last_message.content.size()));
		FingerprintHashString(hash, std::to_string(last_message.tool_calls.size()));
		FingerprintHashString(hash, std::to_string(last_message.thoughts.size()));
		FingerprintHashBool(hash, last_message.interrupted);
	}

	return FingerprintHashHex(hash);
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

nlohmann::json SerializeChatTerminalSummary(const AppState& app, const ChatSession& chat)
{
	const bool ready_since_last_select = app.chats_with_unseen_updates.find(chat.id) != app.chats_with_unseen_updates.end();
	const bool selected = chat.id == CliSelectedChatId(app);
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
		return terminal_json;
	}

	const bool processing = has_pending_call;
	nlohmann::json terminal_json;
	terminal_json["running"] = false;
	terminal_json["turnState"] = "idle";
	terminal_json["processing"] = processing;
	terminal_json["readySinceLastSelect"] = ready_since_last_select;
	terminal_json["active"] = false;
	terminal_json["lastError"] = "";
	return terminal_json;
}

nlohmann::json SerializeFingerprintSession(const AppState& app, const ChatSession& chat)
{
	nlohmann::json chat_json;
	chat_json["id"] = chat.id;
	chat_json["title"] = chat.title;
	chat_json["folderId"] = chat.folder_id;
	chat_json["providerId"] = chat.provider_id;
	chat_json["createdAt"] = chat.created_at;
	chat_json["updatedAt"] = chat.updated_at;
	chat_json["messageCount"] = chat.messages.size();
	chat_json["messagesDigest"] = MessageDigestForFingerprint(chat);
	chat_json["cliTerminal"] = SerializeChatTerminalSummary(app, chat);
	return chat_json;
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
		j["settings"]                = settings;
	}

	return j;
}

nlohmann::json StateSerializer::SerializeFingerprint(const AppState& app)
{
	nlohmann::json j;

	auto folders_arr = nlohmann::json::array();
	for (const auto& folder : app.folders)
	{
		folders_arr.push_back(SerializeFolder(folder));
	}
	j["folders"] = folders_arr;

	auto chats_arr = nlohmann::json::array();
	for (const auto& chat : app.chats)
	{
		chats_arr.push_back(SerializeFingerprintSession(app, chat));
	}
	j["chats"] = chats_arr;

	if (app.selected_chat_index >= 0 &&
	    app.selected_chat_index < static_cast<int>(app.chats.size()))
	{
		j["selectedChatId"] = app.chats[static_cast<std::size_t>(app.selected_chat_index)].id;
	}
	else
	{
		j["selectedChatId"] = nullptr;
	}

	auto providers_arr = nlohmann::json::array();
	for (const auto& profile : app.provider_profiles)
	{
		providers_arr.push_back(SerializeProvider(profile));
	}
	j["providers"] = providers_arr;

	{
		nlohmann::json settings;
		settings["activeProviderId"] = app.settings.active_provider_id;
		settings["theme"] = app.settings.ui_theme;
		j["settings"] = settings;
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
