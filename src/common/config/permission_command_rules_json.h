#pragma once

#include "common/provider/provider_ids.h"
#include "common/security/command_rules.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace uam::permission_command_rules
{

/// <summary>Frontend and settings-file shape: {id, action, tool, pattern, providerId, workspace, enabled}.</summary>
inline nlohmann::json Serialize(const std::vector<uam::command_rules::Rule>& rules)
{
	nlohmann::json result = nlohmann::json::array();
	for (const uam::command_rules::Rule& rule : rules)
	{
		result.push_back({
		    {"id", rule.id},
		    {"action", uam::command_rules::ActionName(rule.action)},
		    {"tool", uam::command_rules::ToolName(rule.tool)},
		    {"pattern", rule.pattern},
		    {"providerId", rule.provider_id},
		    {"workspace", rule.workspace},
		    {"enabled", rule.enabled},
		});
	}
	return result;
}

/// <summary>Reads one rule strictly; returns an error message for anything that is not a valid rule.</summary>
inline std::string ParseRule(const nlohmann::json& value, uam::command_rules::Rule& rule)
{
	if (!value.is_object()) return "Each rule must be an object.";
	const auto text = [&](const char* key, std::string& out) -> bool
	{
		if (!value.contains(key)) { out.clear(); return true; }
		if (!value[key].is_string()) return false;
		out = uam::strings::Trim(value[key].get<std::string>());
		return true;
	};
	std::string action, tool;
	if (!text("id", rule.id) || !text("action", action) || !text("tool", tool) || !text("pattern", rule.pattern) ||
	    !text("providerId", rule.provider_id) || !text("workspace", rule.workspace))
		return "Rule fields must be text.";
	const auto parsed_action = uam::command_rules::ParseAction(action);
	const auto parsed_tool = uam::command_rules::ParseTool(tool);
	if (!parsed_action) return "A rule's action must be allow, ask or deny.";
	if (!parsed_tool) return "A rule's tool is not recognised.";
	rule.action = *parsed_action;
	rule.tool = *parsed_tool;
	rule.provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(rule.provider_id);
	if (value.contains("enabled"))
	{
		if (!value["enabled"].is_boolean()) return "A rule's enabled flag must be true or false.";
		rule.enabled = value["enabled"].get<bool>();
	}
	else rule.enabled = true;
	return uam::command_rules::ValidationError(rule);
}

/// <summary>Load-time parse: invalid entries are dropped, never repaired into something broader.</summary>
inline std::vector<uam::command_rules::Rule> ParseLenient(const nlohmann::json& value)
{
	std::vector<uam::command_rules::Rule> rules;
	if (!value.is_array()) return rules;
	for (const nlohmann::json& item : value)
	{
		uam::command_rules::Rule rule;
		if (ParseRule(item, rule).empty()) rules.push_back(std::move(rule));
	}
	return uam::command_rules::Normalize(std::move(rules));
}

/// <summary>Save-time parse: any invalid rule rejects the whole list with its message.</summary>
inline bool ParseStrict(const nlohmann::json& value, std::vector<uam::command_rules::Rule>& rules, std::string& error)
{
	rules.clear();
	if (!value.is_array()) { error = "Rules must be a list."; return false; }
	if (value.size() > uam::command_rules::kMaxRules) { error = "UAM keeps at most 200 rules."; return false; }
	for (const nlohmann::json& item : value)
	{
		uam::command_rules::Rule rule;
		error = ParseRule(item, rule);
		if (!error.empty()) return false;
		if (std::ranges::any_of(rules, [&](const uam::command_rules::Rule& existing) { return existing.id == rule.id; }))
		{
			error = "Two rules share the id " + rule.id + ".";
			return false;
		}
		rules.push_back(std::move(rule));
	}
	return true;
}

} // namespace uam::permission_command_rules
