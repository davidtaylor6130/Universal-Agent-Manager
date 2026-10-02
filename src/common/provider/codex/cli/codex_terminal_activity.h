#pragma once

#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/provider_runtime.h"
#include "common/state/app_state.h"

#include <array>
#include <fstream>

namespace uam
{

inline void CheckpointCodexTerminalSubmission(CliTerminalState& terminal)
{
	if (terminal.codex_activity_rollout.empty()) return;
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(terminal.codex_activity_rollout, error);
	terminal.codex_activity_read_failed = terminal.codex_activity_read_failed || static_cast<bool>(error);
	if (!error) terminal.codex_activity_offset = size;
	terminal.codex_activity_partial_line.clear();
	terminal.codex_activity_discard_line = false;
	terminal.codex_activity_turn_id.clear();
	terminal.codex_activity_completed = false;
	terminal.codex_activity_awaiting_turn = true;
}

/// <summary>Consume only appended records from an attached, cwd-verified rollout.
/// Startup history and unmatched completions cannot authorize CLI-to-chat handoff.</summary>
inline ProviderTerminalActivity PollCodexTerminalActivity(CliTerminalState& terminal, std::string_view session_id,
    const std::filesystem::path& cwd, bool ambiguous, const std::filesystem::path& codex_home = codex::CodexHomePath())
{
	const std::string valid_id = codex::ValidThreadIdOrEmpty(session_id);
	// Remember the controller-local working directory before the owned status reply
	// attaches its identity, so deferred input can checkpoint before being flushed.
	if (valid_id.empty() && terminal.codex_activity_session_id.empty()) terminal.codex_activity_cwd = cwd;
	if (ambiguous || valid_id.empty() || cwd.empty())
		return terminal.codex_activity_awaiting_turn || ambiguous ? ProviderTerminalActivity::Busy : ProviderTerminalActivity::Unavailable;

	if (terminal.codex_activity_session_id != valid_id || terminal.codex_activity_cwd != cwd)
	{
		terminal.codex_activity_session_id = valid_id;
		terminal.codex_activity_cwd = cwd;
		terminal.codex_activity_rollout.clear();
		terminal.codex_activity_turn_id.clear();
		terminal.codex_activity_partial_line.clear();
		terminal.codex_activity_discard_line = false;
		terminal.codex_activity_completed = false;
		terminal.codex_activity_read_failed = true;
		const std::optional<std::filesystem::path> rollout = codex::FindRolloutFileForSession(valid_id, codex_home);
		if (rollout)
		{
			std::ifstream file(*rollout, std::ios::binary);
			std::array<char, 65536> prelude{};
			file.read(prelude.data(), static_cast<std::streamsize>(prelude.size()));
			const std::string_view text(prelude.data(), static_cast<std::size_t>(file.gcount()));
			std::size_t offset = 0;
			for (int line = 0; line < codex::kMaxRolloutPreludeLines && offset < text.size(); ++line)
			{
				const std::size_t end = text.find('\n', offset);
				if (end == std::string_view::npos) break;
				const nlohmann::json record = nlohmann::json::parse(text.substr(offset, end - offset), nullptr, false);
				offset = end + 1;
				if (record.is_discarded() || !record.is_object() || uam::nlohmann_json::StringViewOrEmpty(record, "type") != "session_meta") continue;
				const nlohmann::json payload = record.value("payload", nlohmann::json::object());
				if (!payload.is_object() || codex::ValidThreadIdOrEmpty(uam::nlohmann_json::StringViewOrEmpty(payload, "id")) != valid_id ||
				    uam::nlohmann_json::StringViewOrEmpty(payload, "cwd").empty() ||
				    !codex::PathsMatch(uam::nlohmann_json::StringViewOrEmpty(payload, "cwd"), cwd)) break;
				std::error_code error;
				terminal.codex_activity_offset = std::filesystem::file_size(*rollout, error);
				if (!error)
				{
					terminal.codex_activity_rollout = *rollout;
					terminal.codex_activity_read_failed = false;
				}
				break;
			}
		}
	}
	if (terminal.codex_activity_rollout.empty())
		return terminal.codex_activity_awaiting_turn ? ProviderTerminalActivity::Busy : ProviderTerminalActivity::Unavailable;

	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(terminal.codex_activity_rollout, error);
	if (error || size < terminal.codex_activity_offset)
	{
		terminal.codex_activity_read_failed = true;
		return ProviderTerminalActivity::Busy;
	}
	if (terminal.codex_activity_read_failed) return ProviderTerminalActivity::Busy;
	if (size == terminal.codex_activity_offset)
		return terminal.codex_activity_awaiting_turn || !terminal.codex_activity_turn_id.empty() ||
		    !terminal.codex_activity_partial_line.empty() || terminal.codex_activity_discard_line ? ProviderTerminalActivity::Busy :
		    terminal.codex_activity_completed ? ProviderTerminalActivity::Complete : ProviderTerminalActivity::Unavailable;

	std::ifstream file(terminal.codex_activity_rollout, std::ios::binary);
	file.seekg(static_cast<std::streamoff>(terminal.codex_activity_offset));
	std::array<char, 65536> bytes{};
	file.read(bytes.data(), static_cast<std::streamsize>(std::min<std::uintmax_t>(size - terminal.codex_activity_offset, bytes.size())));
	if (file.bad() || file.gcount() == 0)
	{
		terminal.codex_activity_read_failed = true;
		return ProviderTerminalActivity::Busy;
	}
	terminal.codex_activity_offset += static_cast<std::uintmax_t>(file.gcount());
	for (std::streamsize index = 0; index < file.gcount(); ++index)
	{
		const char byte = bytes[static_cast<std::size_t>(index)];
		if (byte != '\n')
		{
			if (!terminal.codex_activity_discard_line)
			{
				if (terminal.codex_activity_partial_line.size() < 262144) terminal.codex_activity_partial_line.push_back(byte);
				else
				{
					terminal.codex_activity_discard_line = true;
				}
			}
			continue;
		}
		std::string top_level_key;
		std::string top_level_type;
		const nlohmann::json record = nlohmann::json::parse(terminal.codex_activity_partial_line,
		    [&top_level_key, &top_level_type](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
		    {
			    if (depth == 1 && event == nlohmann::json::parse_event_t::key) top_level_key = value.get<std::string>();
			    if (depth == 1 && event == nlohmann::json::parse_event_t::value && top_level_key == "type" && value.is_string())
				    top_level_type = value.get<std::string>();
			    return true;
		    }, false);
		terminal.codex_activity_partial_line.clear();
		// Large response items are ordinary text/tool payloads. Their parsed top-level
		// type can be identified from the bounded prefix without retaining the payload.
		if (terminal.codex_activity_discard_line && top_level_type == "response_item")
		{
			terminal.codex_activity_discard_line = false;
			continue;
		}
		if (terminal.codex_activity_discard_line || record.is_discarded() || !record.is_object())
		{
			terminal.codex_activity_discard_line = false;
			terminal.codex_activity_read_failed = true;
			continue;
		}
		terminal.codex_activity_discard_line = false;
		if (uam::nlohmann_json::StringViewOrEmpty(record, "type").empty())
		{
			terminal.codex_activity_read_failed = true;
			continue;
		}
		if (uam::nlohmann_json::StringViewOrEmpty(record, "type") != "event_msg") continue;
		const nlohmann::json payload = record.value("payload", nlohmann::json::object());
		if (!payload.is_object())
		{
			terminal.codex_activity_read_failed = true;
			continue;
		}
		const std::string type{uam::nlohmann_json::StringViewOrEmpty(payload, "type")};
		if (type.empty())
		{
			terminal.codex_activity_read_failed = true;
			continue;
		}
		const std::string turn_id{uam::nlohmann_json::StringViewOrEmpty(payload, "turn_id")};
		const bool start = type == "task_started" || type == "turn_started";
		const bool complete = type == "task_complete" || type == "turn_complete" || type == "turn_aborted";
		if (turn_id.empty())
		{
			if (start || complete) terminal.codex_activity_read_failed = true;
			continue;
		}
		if (start)
		{
			terminal.codex_activity_turn_id = turn_id;
			terminal.codex_activity_awaiting_turn = false;
			terminal.codex_activity_completed = false;
		}
		else if (complete && turn_id == terminal.codex_activity_turn_id)
		{
			terminal.codex_activity_turn_id.clear();
			terminal.codex_activity_completed = true;
		}
	}
	// A later start, partial record, or unread tail must be consumed before declaring idle.
	if (terminal.codex_activity_read_failed || terminal.codex_activity_awaiting_turn || !terminal.codex_activity_turn_id.empty() ||
	    !terminal.codex_activity_partial_line.empty() || terminal.codex_activity_discard_line || terminal.codex_activity_offset < size)
		return ProviderTerminalActivity::Busy;
	return terminal.codex_activity_completed ? ProviderTerminalActivity::Complete : ProviderTerminalActivity::Unavailable;
}

} // namespace uam
