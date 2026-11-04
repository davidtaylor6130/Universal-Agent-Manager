#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>

namespace uam::copilot::presentation
{
using Json = nlohmann::json;
inline std::string Text(const Json& value, const char* key)
{
	const auto found = value.find(key);
	return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// Validate only the native reset envelope; this grants no setup or execution authority.
inline bool ValidSessionReset(const Json& data, const std::string& type, const std::string& session_id)
{
	const auto text = [&](const char* key, std::size_t maximum)
	{
		const auto value = data.find(key);
		return value != data.end() && value->is_string() && !value->get_ref<const std::string&>().empty() &&
		    value->get_ref<const std::string&>().size() <= maximum && value->get_ref<const std::string&>().find('\0') == std::string::npos;
	};
	const auto count = [&](const char* key)
	{
		const auto value = data.find(key);
		return value != data.end() && value->is_number_integer() &&
		    (value->is_number_unsigned() || value->get<std::int64_t>() >= 0);
	};
	if (type == "session.resume") return count("eventCount") && text("resumeTime", 128);
	return type == "session.start" && Text(data, "sessionId") == session_id && text("startTime", 128) &&
	    text("copilotVersion", 128) && text("producer", 512) && count("version");
}

inline std::optional<std::string> NativeTurn(const Json& source, bool& valid)
{
	const auto turn = source.find("turnId");
	if (turn == source.end()) return std::nullopt;
	if (!turn->is_string() || turn->get_ref<const std::string&>().empty() ||
	    turn->get_ref<const std::string&>().size() > 512 || turn->get_ref<const std::string&>().find('\0') != std::string::npos)
	{
		valid = false;
		return std::nullopt;
	}
	return turn->get<std::string>();
}

inline std::optional<std::string> CurrentTurn(const Json& state, const std::string& agent, bool& valid)
{
	const auto turns = state.find("turns");
	if (turns == state.end() || !turns->is_object()) return std::nullopt;
	const auto scope = turns->find(agent);
	if (scope == turns->end()) return std::nullopt;
	if (!scope->value("valid", false)) valid = false;
	return NativeTurn(*scope, valid);
}

// Invoke only after existing event dedupe and scope acceptance, never on replay.
inline void AcceptedBoundary(Json& state, const Json& event, const Json& data)
{
	if (!state.is_object() || state.value("overflow", false)) state = Json::object();
	const std::string agent = Text(event, "agentId");
	if (agent.size() > 512 || agent.find('\0') != std::string::npos || (event.contains("agentId") && !event["agentId"].is_string()))
	{
		state = {{"overflow", true}}; return;
	}
	auto calls = state.find("calls");
	if (calls != state.end() && calls->is_object())
		for (auto call = calls->begin(); call != calls->end();)
		{
			if (Text(*call, "agent") == agent) call = calls->erase(call);
			else ++call;
		}
	auto& turns = state["turns"];
	if (!turns.is_object()) turns = Json::object();
	if (!turns.contains(agent) && turns.size() >= 512) { state = {{"overflow", true}}; return; }
	bool valid = true;
	const auto turn = NativeTurn(data, valid);
	if (Text(event, "type") == "assistant.turn_start" && !turn) valid = false;
	turns[agent] = {{"valid", valid}};
	if (turn) turns[agent]["turnId"] = *turn;
}

// Native provenance is presentation correlation only; it cannot grant a managed control.
// Keep rejected call IDs until consumption/reset so later events cannot rehabilitate them.
inline void Observe(Json& state, const Json& event, const Json& call, const Json* message = nullptr)
{
	if (!call.is_object()) return;
	const std::string id = Text(call, "toolCallId");
	if (id.empty() || id.size() > 512 || id.find('\0') != std::string::npos) return;
	const bool provenance = call.contains("mcpServerName") || call.contains("mcpConfigServerName") || call.contains("mcpToolName");
	if (!state.is_object()) state = Json::object();
	if (state.value("overflow", false)) return;
	auto& calls = state["calls"];
	if (!calls.is_object()) calls = Json::object();
	if (!provenance && !calls.contains(id)) return;
	if (!calls.contains(id) && calls.size() >= 512)
	{
		calls.clear(); state["overflow"] = true; return;
	}
	const std::string agent = Text(event, "agentId");
	if (agent.size() > 512 || agent.find('\0') != std::string::npos) { calls[id] = {{"owned", false}, {"agent", ""}}; return; }
	bool valid_turn = true;
	const auto turn = NativeTurn(message ? *message : call, valid_turn);
	const auto current_turn = CurrentTurn(state, agent, valid_turn);
	const auto previous = calls.find(id);
	const bool previous_owned = previous != calls.end() && previous->value("owned", false);
	bool previous_turn_valid = true;
	const auto previous_turn = previous != calls.end() ? NativeTurn(*previous, previous_turn_valid) : std::nullopt;
	const bool consistent = previous == calls.end() || (previous_owned && Text(*previous, "agent") == agent &&
	    previous_turn_valid && (!previous_turn || !turn || *previous_turn == *turn));
	const auto captured_turn = turn ? turn : previous_turn ? previous_turn : current_turn;
	const bool current_turn_matches = !current_turn || !captured_turn || *current_turn == *captured_turn;
	const bool origin = provenance ? Text(call, "mcpToolName") == "agent_delegate" &&
	    (call.contains("mcpConfigServerName") ? Text(call, "mcpConfigServerName") == "uam-control" : Text(call, "mcpServerName") == "uam-control") &&
	    (!call.contains("mcpServerName") || Text(call, "mcpServerName") == "uam-control") : previous_owned;
	calls[id] = {{"owned", origin && consistent && current_turn_matches && valid_turn && (!event.contains("agentId") || event["agentId"].is_string())}, {"agent", agent}};
	// An absent optional field invents no native ID; retain only an actual prior witness.
	if (captured_turn) calls[id]["turnId"] = *captured_turn;
}

inline void ObserveRequests(Json& state, const Json& event, const Json& data)
{
	const auto requests = data.find("toolRequests");
	if (requests == data.end() || !requests->is_array()) return;
	if (requests->size() > 512) { state = {{"overflow", true}}; return; }
	for (const auto& call : *requests) Observe(state, event, call, &data);
}

inline std::string Consume(Json& state, const Json& event, const Json& data, bool success, const std::string& content)
{
	if (!state.is_object() || state.value("overflow", false)) return content;
	auto calls = state.find("calls");
	if (calls == state.end() || !calls->is_object()) return content;
	const auto found = calls->find(Text(data, "toolCallId"));
	if (found == calls->end()) return content;
	bool valid_turn = true;
	const auto turn = NativeTurn(data, valid_turn);
	const auto origin_turn = NativeTurn(*found, valid_turn);
	const auto current_turn = CurrentTurn(state, Text(event, "agentId"), valid_turn);
	const bool owned = found->value("owned", false) && Text(*found, "agent") == Text(event, "agentId") &&
	    (!event.contains("agentId") || event["agentId"].is_string()) && valid_turn && (!turn || !origin_turn || *turn == *origin_turn) &&
	    (!current_turn || !origin_turn || *current_turn == *origin_turn) && (!current_turn || !turn || *current_turn == *turn);
	calls->erase(found);
	if (!owned || !success || content.size() > 8192) return content;
	return Json{{"server", "uam-control"}, {"tool", "agent_delegate"}, {"status", "completed"},
	    {"result", {{"content", Json::array({{{"type", "text"}, {"text", content}}})}}}}.dump();
}
} // namespace uam::copilot::presentation
