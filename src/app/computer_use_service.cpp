#include "app/computer_use_service.h"

#include "computer_use/computer_use_mcp_config.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <sstream>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaximumHistoryEntries = 50;
		constexpr std::size_t kMaximumControlBytes = 4096;
		constexpr std::size_t kMaximumHistoryBytes = 512 * 1024;

		std::filesystem::path SessionDirectory(const AppState& app, const std::string& chat_id)
		{
			return app.data_root / "computer-use" / chat_id;
		}

		std::string NormalizeState(std::string state, std::string fallback)
		{
			return state == "running" || state == "paused" || state == "stopped" ? state : fallback;
		}

		std::string FileSignature(const std::filesystem::path& path)
		{
			std::error_code error;
			const auto size = std::filesystem::file_size(path, error);
			if (error) return "missing";
			const auto modified = std::filesystem::last_write_time(path, error);
			return error ? std::to_string(size) : std::to_string(size) + ":" +
			    std::to_string(static_cast<long long>(modified.time_since_epoch().count()));
		}

		std::string SourceSignature(const AppState& app, const ChatSession& chat)
		{
			const std::filesystem::path directory = SessionDirectory(app, chat.id);
			return FileSignature(directory / "control.json") + ":" +
			    FileSignature(directory / "history.jsonl");
		}

		ComputerUseRuntimeState ReadState(const AppState& app, const ChatSession& chat)
		{
			ComputerUseRuntimeState result;
			result.state = "stopped";
			const std::filesystem::path directory = SessionDirectory(app, chat.id);
			std::string control_text;
			(void)uam::io::TryReadTextFile(directory / "control.json", control_text, kMaximumControlBytes);
			const nlohmann::json control = nlohmann::json::parse(control_text, nullptr, false);
			if (control.is_object())
			{
				result.state = NormalizeState(
				    std::string(uam::nlohmann_json::StringViewOrEmpty(control, "state")),
				    result.state);
				result.target_kind = std::string(uam::nlohmann_json::StringViewOrEmpty(control, "targetKind"));
				result.target_id = std::string(uam::nlohmann_json::StringViewOrEmpty(control, "targetId"));
				result.target_process_id = std::string(uam::nlohmann_json::StringViewOrEmpty(control, "targetProcessId"));
				result.target_title = std::string(uam::nlohmann_json::StringViewOrEmpty(control, "targetTitle"));
				result.target_input_mode = std::string(uam::nlohmann_json::StringViewOrEmpty(control, "targetInputMode"));
				if (result.target_id.empty() && result.state != "stopped")
				{
					result.target_kind = chat.computer_use_target_kind;
					result.target_id = chat.computer_use_target_id;
					result.target_process_id = chat.computer_use_target_process_id;
					result.target_title = chat.computer_use_target_title;
					result.target_input_mode = chat.computer_use_target_input_mode;
				}
			}

			std::string history_text;
			(void)uam::io::TryReadTextFile(directory / "history.jsonl", history_text, kMaximumHistoryBytes);
			std::istringstream history(history_text);
			std::string line;
			while (std::getline(history, line))
			{
				const nlohmann::json entry = nlohmann::json::parse(line, nullptr, false);
				if (entry.is_object())
				{
					const nlohmann::json* time =
					    uam::nlohmann_json::FindStringField(entry, "time");
					const nlohmann::json* action =
					    uam::nlohmann_json::FindStringField(entry, "action");
					const nlohmann::json* status =
					    uam::nlohmann_json::FindStringField(entry, "status");
					const nlohmann::json* detail =
					    uam::nlohmann_json::FindStringField(entry, "detail");
					if (time == nullptr || action == nullptr || status == nullptr ||
					    detail == nullptr)
						continue;
					result.history.push_back({
					    time->get<std::string>(), action->get<std::string>(),
					    status->get<std::string>(), detail->get<std::string>(),
					});
					if (result.history.size() > kMaximumHistoryEntries) result.history.erase(result.history.begin());
				}
			}

			nlohmann::json fingerprint = {{"state", result.state},
			    {"targetKind", result.target_kind},
			    {"targetId", result.target_id}, {"targetProcessId", result.target_process_id},
			    {"targetTitle", result.target_title}, {"targetInputMode", result.target_input_mode},
			    {"history", nlohmann::json::array()}};
			for (const ComputerUseHistoryEntry& entry : result.history)
			{
				fingerprint["history"].push_back({
				    {"time", entry.time}, {"action", entry.action}, {"status", entry.status},
				    {"detail", entry.detail},
				});
			}
			result.fingerprint = fingerprint.dump();
			return result;
		}
	} // namespace

	bool ComputerUseService::ResetControlsForStartup(AppState& app)
	{
		bool reset = true;
		for (ChatSession& chat : app.chats)
		{
			chat.computer_use_enabled = false;
			if (!uam::computer_use::IsPortableMcpChatId(chat.id))
				continue;
			const std::filesystem::path control =
			    SessionDirectory(app, chat.id) / "control.json";
			if (uam::paths::PathExistsNoThrow(control) &&
			    !uam::io::WriteTextFile(control,
			        nlohmann::json{{"state", "stopped"}}.dump() + "\n"))
				reset = false;
		}
		app.computer_use_by_chat_id.clear();
		return reset;
	}

	bool ComputerUseService::Poll(AppState& app)
	{
		bool changed = false;
		for (ChatSession& chat : app.chats)
		{
			if (!uam::computer_use::IsPortableMcpChatId(chat.id))
			{
				changed = app.computer_use_by_chat_id.erase(chat.id) > 0 || changed;
				continue;
			}
			if (!uam::computer_use::UsesUamBackend(chat))
			{
				changed = app.computer_use_by_chat_id.erase(chat.id) > 0 || changed;
				continue;
			}
			const std::string source_signature = SourceSignature(app, chat);
			const auto current = app.computer_use_by_chat_id.find(chat.id);
			if (current != app.computer_use_by_chat_id.end() &&
			    current->second.source_signature == source_signature) continue;
			ComputerUseRuntimeState next = ReadState(app, chat);
			next.source_signature = source_signature;
			const bool enabled = next.state != "stopped" && !next.target_id.empty();
			if (chat.computer_use_enabled != enabled ||
			    chat.computer_use_target_kind != (enabled ? next.target_kind : "window") ||
			    chat.computer_use_target_id != (enabled ? next.target_id : "") ||
			    chat.computer_use_target_process_id != (enabled ? next.target_process_id : "") ||
			    chat.computer_use_target_title != (enabled ? next.target_title : "") ||
			    chat.computer_use_target_input_mode != (enabled ? next.target_input_mode : ""))
			{
				chat.computer_use_enabled = enabled;
				chat.computer_use_target_kind = enabled ? next.target_kind : "window";
				chat.computer_use_target_id = enabled ? next.target_id : "";
				chat.computer_use_target_process_id = enabled ? next.target_process_id : "";
				chat.computer_use_target_title = enabled ? next.target_title : "";
				chat.computer_use_target_input_mode = enabled ? next.target_input_mode : "";
				changed = true;
			}
			auto found = app.computer_use_by_chat_id.find(chat.id);
			if (found == app.computer_use_by_chat_id.end() || found->second.fingerprint != next.fingerprint)
			{
				app.computer_use_by_chat_id[chat.id] = std::move(next);
				changed = true;
			}
			else
			{
				found->second.source_signature = source_signature;
			}
		}
		return changed;
	}

	bool ComputerUseService::SetControlState(AppState& app, const std::string& chat_id,
	    std::string_view state, std::string* error)
	{
		if (state != "running" && state != "paused" && state != "stopped")
		{
			if (error != nullptr) *error = "Computer-use state must be running, paused, or stopped.";
			return false;
		}
		if (!uam::computer_use::IsPortableMcpChatId(chat_id))
		{
			if (error != nullptr)
				*error = "This chat identifier is not portable enough for computer use.";
			return false;
		}

		const std::filesystem::path directory = SessionDirectory(app, chat_id);
		std::error_code path_error;
		uam::paths::CreateDirectoriesNoThrow(directory, &path_error);
		std::string control_text;
		(void)uam::io::TryReadTextFile(directory / "control.json", control_text,
		    kMaximumControlBytes);
		nlohmann::json control = nlohmann::json::parse(control_text, nullptr, false);
		if (!control.is_object()) control = nlohmann::json::object();
		if (state == "stopped")
			control = nlohmann::json{{"state", "stopped"}};
		else
			control["state"] = state;
		if (path_error || !uam::io::WriteTextFile(directory / "control.json",
		                      control.dump() + "\n"))
		{
			if (error != nullptr) *error = "Failed to update computer-use controls.";
			return false;
		}
		(void)Poll(app);
		return true;
	}
} // namespace uam
