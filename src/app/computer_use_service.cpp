#include "app/computer_use_service.h"

#include "computer_use/computer_use_mcp_config.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/time_utils.h"
#include "common/runtime/app_time.h"
#include "common/utils/diagnostic_log.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaximumHistoryEntries = 50;
		constexpr std::size_t kMaximumControlBytes = 4096;
		constexpr std::size_t kMaximumHistoryBytes = 512 * 1024;
		constexpr std::size_t kMaximumTrustedTaskPromptBytes = 1 * 1024 * 1024;
		constexpr std::size_t kMaximumTrustedTaskFileBytes = 8 * 1024 * 1024;

		std::filesystem::path SessionDirectory(const AppState& app, const std::string& chat_id)
		{
			return app.data_root / "computer-use" / chat_id;
		}

		std::string NormalizeState(std::string state, std::string fallback)
		{
			return state == "armed" || state == "running" || state == "paused" || state == "stopped" ? state : fallback;
		}

		std::string FileSignature(const std::filesystem::path& path)
		{
#if defined(_WIN32)
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
			    (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				return "missing";
			const unsigned long long size =
			    (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
			const unsigned long long modified =
			    (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
			    data.ftLastWriteTime.dwLowDateTime;
			return std::to_string(size) + ":" + std::to_string(modified);
#else
			struct stat data{};
			if (::stat(path.c_str(), &data) != 0 || !S_ISREG(data.st_mode)) return "missing";
#if defined(__APPLE__)
			const long long modified = static_cast<long long>(data.st_mtimespec.tv_sec) * 1000000000LL +
			    data.st_mtimespec.tv_nsec;
#else
			const long long modified = static_cast<long long>(data.st_mtim.tv_sec) * 1000000000LL +
			    data.st_mtim.tv_nsec;
#endif
			return std::to_string(static_cast<unsigned long long>(data.st_size)) + ":" +
			    std::to_string(modified);
#endif
		}

		std::string SourceSignature(const AppState& app, const ChatSession& chat)
		{
			const std::filesystem::path directory = SessionDirectory(app, chat.id);
			return FileSignature(directory / "control.json") + ":" +
			    FileSignature(directory / "history.jsonl");
		}

		std::optional<std::unordered_set<std::string>> ExistingSessionIds(const AppState& app)
		{
			std::unordered_set<std::string> result;
			std::error_code error;
			const std::filesystem::path root = app.data_root / "computer-use";
			std::filesystem::directory_iterator entries(root, error);
			if (error == std::errc::no_such_file_or_directory) return result;
			if (error) return std::nullopt;
			for (; entries != std::filesystem::directory_iterator(); entries.increment(error))
				result.insert(entries->path().filename().string());
			return error ? std::nullopt : std::optional<std::unordered_set<std::string>>(std::move(result));
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

	struct ComputerUseService::Scan
	{
		AppState state;
		std::vector<ChatSession> baseline;
		std::vector<std::size_t> changed_indices;
		std::unordered_map<std::string, std::string> signatures;
	};

	namespace
	{
		ChatSession ControlSnapshot(const ChatSession& chat)
		{
			ChatSession result;
			result.id = chat.id;
			result.provider_id = chat.provider_id;
			result.execution_host_id = chat.execution_host_id;
			result.computer_use_backend = chat.computer_use_backend;
			result.computer_use_enabled = chat.computer_use_enabled;
			result.computer_use_target_kind = chat.computer_use_target_kind;
			result.computer_use_target_id = chat.computer_use_target_id;
			result.computer_use_target_process_id = chat.computer_use_target_process_id;
			result.computer_use_target_title = chat.computer_use_target_title;
			result.computer_use_target_input_mode = chat.computer_use_target_input_mode;
			return result;
		}

		bool SameControlIdentity(const ChatSession& left, const ChatSession& right)
		{
			return std::tie(left.id, left.provider_id, left.execution_host_id, left.computer_use_backend,
			    left.computer_use_enabled, left.computer_use_target_kind, left.computer_use_target_id,
			    left.computer_use_target_process_id, left.computer_use_target_title, left.computer_use_target_input_mode) ==
			    std::tie(right.id, right.provider_id, right.execution_host_id, right.computer_use_backend,
			    right.computer_use_enabled, right.computer_use_target_kind, right.computer_use_target_id,
			    right.computer_use_target_process_id, right.computer_use_target_title, right.computer_use_target_input_mode);
		}
	}

	bool ComputerUseService::PollInBackground(AppState& app)
	{
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		bool changed = false;
		if (m_scan.valid() && m_scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			std::shared_ptr<Scan> scan;
			try { scan = m_scan.get(); }
			catch (const std::exception& error) { diagnostics::Write(std::string("Computer-use background scan failed: ") + error.what()); m_nextScanTime = GetAppTimeSeconds() + 1.0; }
			if (scan && !scan->changed_indices.empty() && scan->state.data_root == app.data_root)
			{
				std::unordered_map<std::string, ChatSession*> current_by_id;
				current_by_id.reserve(app.chats.size());
				for (ChatSession& chat : app.chats) current_by_id.emplace(chat.id, &chat);
				for (const std::size_t index : scan->changed_indices)
				{
					const ChatSession& baseline = scan->baseline[index];
					const std::unordered_map<std::string, ChatSession*>::const_iterator found = current_by_id.find(baseline.id);
					if (found == current_by_id.end()) continue;
					ChatSession* const current = found->second;
					if (!SameControlIdentity(*current, baseline)) continue;
					const auto prior = app.computer_use_by_chat_id.find(baseline.id);
					const std::string signature = prior == app.computer_use_by_chat_id.end() ? "" : prior->second.source_signature;
					if (signature != scan->signatures[baseline.id]) continue;
					const ChatSession& next = scan->state.chats[index];
					if (!SameControlIdentity(*current, next))
					{
						current->computer_use_enabled = next.computer_use_enabled;
						current->computer_use_target_kind = next.computer_use_target_kind;
						current->computer_use_target_id = next.computer_use_target_id;
						current->computer_use_target_process_id = next.computer_use_target_process_id;
						current->computer_use_target_title = next.computer_use_target_title;
						current->computer_use_target_input_mode = next.computer_use_target_input_mode;
						changed = true;
					}
					const auto result = scan->state.computer_use_by_chat_id.find(baseline.id);
					if (result == scan->state.computer_use_by_chat_id.end()) changed = app.computer_use_by_chat_id.erase(baseline.id) > 0 || changed;
					else
					{
						changed = prior == app.computer_use_by_chat_id.end() || prior->second.fingerprint != result->second.fingerprint || changed;
						app.computer_use_by_chat_id[baseline.id] = result->second;
					}
				}
			}
			m_retiredScan = std::move(scan);
		}
		const std::chrono::steady_clock::time_point publication_finished = std::chrono::steady_clock::now();
		if (!m_scan.valid() && GetAppTimeSeconds() >= m_nextScanTime)
		{
			std::shared_ptr<Scan> scan = std::make_shared<Scan>();
			scan->state.data_root = app.data_root;
			scan->state.computer_use_by_chat_id = app.computer_use_by_chat_id;
			scan->baseline.reserve(app.chats.size());
			scan->signatures.reserve(app.chats.size());
			for (const ChatSession& chat : app.chats)
			{
				scan->baseline.push_back(ControlSnapshot(chat));
				const auto prior = app.computer_use_by_chat_id.find(chat.id);
				scan->signatures[chat.id] = prior == app.computer_use_by_chat_id.end() ? "" : prior->second.source_signature;
			}
			// Copy and release large scan vectors on the worker, outside interactive GUI work.
			std::packaged_task<std::shared_ptr<Scan>()> task([scan, retired = std::move(m_retiredScan)]() mutable
			{
				retired.reset();
				std::unordered_map<std::string, std::pair<std::string, std::string>> prior_states;
				prior_states.reserve(scan->state.computer_use_by_chat_id.size());
				for (const std::pair<const std::string, ComputerUseRuntimeState>& entry : scan->state.computer_use_by_chat_id)
					prior_states.emplace(entry.first, std::make_pair(entry.second.fingerprint, entry.second.source_signature));
				scan->state.chats = scan->baseline;
				(void)Poll(scan->state);
				// Unchanged scans need no catalog lookup or runtime-state copying on the GUI thread.
				for (std::size_t index = 0; index < scan->baseline.size(); ++index)
				{
					const ChatSession& baseline = scan->baseline[index];
					const std::unordered_map<std::string, std::pair<std::string, std::string>>::const_iterator prior = prior_states.find(baseline.id);
					const std::unordered_map<std::string, ComputerUseRuntimeState>::const_iterator next = scan->state.computer_use_by_chat_id.find(baseline.id);
					if (!SameControlIdentity(baseline, scan->state.chats[index]) ||
					    (prior == prior_states.end()) != (next == scan->state.computer_use_by_chat_id.end()) ||
					    (prior != prior_states.end() && next != scan->state.computer_use_by_chat_id.end() &&
					     (prior->second.first != next->second.fingerprint || prior->second.second != next->second.source_signature)))
						scan->changed_indices.push_back(index);
				}
				return scan;
			});
			m_scan = task.get_future();
			std::thread(std::move(task)).detach();
			m_nextScanTime = GetAppTimeSeconds() + 0.25;
		}
		const std::chrono::steady_clock::time_point finished = std::chrono::steady_clock::now();
		static std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::time_point::min();
		if (finished - started >= std::chrono::milliseconds(50) &&
		    (last_report == std::chrono::steady_clock::time_point::min() || finished - last_report >= std::chrono::seconds(5)))
		{
			diagnostics::Write("[performance] Computer-use GUI poll: publication=" +
			    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(publication_finished - started).count()) +
			    ",preparation=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(finished - publication_finished).count()) +
			    " ms; chats=" + std::to_string(app.chats.size()) + ".");
			last_report = finished;
		}
		return changed;
	}

	bool ComputerUseService::ResetControlsForStartup(AppState& app)
	{
		bool reset = true;
		for (ChatSession& chat : app.chats)
		{
			chat.computer_use_enabled = false;
			if (!uam::computer_use::AvailableForChat(chat)) continue;
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

	bool ComputerUseService::Poll(AppState& app, std::string_view chat_id)
	{
		bool changed = false;
		const std::optional<std::unordered_set<std::string>> existing_session_ids =
		    chat_id.empty() ? ExistingSessionIds(app) : std::optional<std::unordered_set<std::string>>{{std::string(chat_id)}};
		for (ChatSession& chat : app.chats)
		{
			if (!chat_id.empty() && chat.id != chat_id) continue;
			if (!uam::computer_use::AvailableForChat(chat))
			{
				chat.computer_use_enabled = false;
				changed = app.computer_use_by_chat_id.erase(chat.id) > 0 || changed;
				continue;
			}
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
			const bool has_prior_state = app.computer_use_by_chat_id.contains(chat.id) ||
			    chat.computer_use_enabled;
			if (existing_session_ids.has_value() && !existing_session_ids->contains(chat.id) &&
			    !has_prior_state)
				continue;
			const std::string source_signature = SourceSignature(app, chat);
			const auto current = app.computer_use_by_chat_id.find(chat.id);
			if (current != app.computer_use_by_chat_id.end() &&
			    current->second.source_signature == source_signature) continue;
			ComputerUseRuntimeState next = ReadState(app, chat);
			next.source_signature = source_signature;
			const bool enabled = next.state == "armed" || (next.state != "stopped" && !next.target_id.empty());
			const bool target_approved = next.state != "stopped" && !next.target_id.empty();
			if (chat.computer_use_enabled != enabled ||
			    chat.computer_use_target_kind != (target_approved ? next.target_kind : "window") ||
			    chat.computer_use_target_id != (target_approved ? next.target_id : "") ||
			    chat.computer_use_target_process_id != (target_approved ? next.target_process_id : "") ||
			    chat.computer_use_target_title != (target_approved ? next.target_title : "") ||
			    chat.computer_use_target_input_mode != (target_approved ? next.target_input_mode : ""))
			{
				chat.computer_use_enabled = enabled;
				chat.computer_use_target_kind = target_approved ? next.target_kind : "window";
				chat.computer_use_target_id = target_approved ? next.target_id : "";
				chat.computer_use_target_process_id = target_approved ? next.target_process_id : "";
				chat.computer_use_target_title = target_approved ? next.target_title : "";
				chat.computer_use_target_input_mode = target_approved ? next.target_input_mode : "";
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
		if (state != "armed" && state != "running" && state != "paused" && state != "stopped")
		{
			if (error != nullptr) *error = "Computer-use state must be armed, running, paused, or stopped.";
			return false;
		}
		if (!uam::computer_use::IsPortableMcpChatId(chat_id))
		{
			if (error != nullptr)
				*error = "This chat identifier is not portable enough for computer use.";
			return false;
		}
		const auto chat = std::ranges::find_if(app.chats,
		    [&chat_id](const ChatSession& candidate) { return candidate.id == chat_id; });
		if (chat == app.chats.end() || !uam::computer_use::AvailableForChat(*chat))
		{
			if (error != nullptr) *error = "Computer Use is disabled for remote execution hosts.";
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
		(void)Poll(app, chat_id);
		return true;
	}

	bool ComputerUseService::PersistTrustedTask(AppState& app, const std::string& chat_id,
	    std::string_view prompt, std::string* error)
	{
		if (prompt.empty() || prompt.size() > kMaximumTrustedTaskPromptBytes)
		{
			if (error != nullptr) *error = "Computer-use prompts must be at most 1 MiB.";
			return false;
		}
		const auto chat = std::ranges::find_if(app.chats,
		    [&chat_id](const ChatSession& candidate) { return candidate.id == chat_id; });
		if (chat == app.chats.end() || !uam::computer_use::AvailableForChat(*chat) ||
		    !uam::computer_use::UsesUamBackend(*chat) || !chat->computer_use_enabled)
		{
			if (error != nullptr) *error = "Computer Use is not enabled for this local chat.";
			return false;
		}
		if (!uam::computer_use::IsPortableMcpChatId(chat_id))
		{
			if (error != nullptr) *error = "This chat identifier is not portable enough for computer use.";
			return false;
		}

		const std::filesystem::path directory = SessionDirectory(app, chat_id);
		std::error_code path_error;
		if (!uam::paths::CreateDirectoriesNoThrow(directory, &path_error) || path_error)
		{
			if (error != nullptr) *error = "Computer-use task directory could not be created.";
			return false;
		}
		const std::filesystem::path task_file = directory / "task.json";
		std::string existing_text;
		nlohmann::json existing = nlohmann::json::value_t::discarded;
		if (uam::io::TryReadTextFile(task_file, existing_text, kMaximumTrustedTaskFileBytes))
			existing = nlohmann::json::parse(existing_text, nullptr, false);
		std::string task_id;
		if (existing.is_object())
		{
			const auto existing_prompt = existing.find("prompt");
			const auto existing_id = existing.find("id");
			if (existing_prompt != existing.end() && existing_id != existing.end() &&
			    existing_prompt->is_string() && existing_id->is_string() &&
			    existing_prompt->get_ref<const std::string&>() == prompt)
				task_id = existing_id->get<std::string>();
		}
		if (task_id.empty())
		{
			task_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
			if (task_id.empty()) task_id = uam::time::SystemEpochMicrosecondsTokenNow();
		}
		const nlohmann::json task = {{"id", task_id}, {"prompt", std::string(prompt)}};
		if (!uam::io::AtomicWriteFile(task_file, task.dump() + "\n"))
		{
			if (error != nullptr) *error = "Computer-use trusted task could not be saved.";
			return false;
		}
		return true;
	}
} // namespace uam
