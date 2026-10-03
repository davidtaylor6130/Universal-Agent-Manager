#pragma once

#include "common/runtime/acp/acp_tool_items.h"
#include "common/utils/nlohmann_json_utils.h"
#include <nlohmann/json.hpp>
#include <string>

namespace uam::codex
{
	/// <summary>Uses the same lossless tool projection for app-server events and reloaded thread items.</summary>
	inline std::string ToolItemTitle(const nlohmann::json& item)
	{
		const std::string type = item.value("type", "");
		if (type == acp_tool_items::kCommandExecution)
			return nlohmann_json::TrimmedStringValueOr(item, "command", "Command");
		if (type == acp_tool_items::kFileChange)
			return "File changes";
		if (type == "functionCallOutput")
			return nlohmann_json::TrimmedStringValueOr(item, "name", "Tool output");
		if (type == "webSearch")
			return nlohmann_json::TrimmedStringValueOr(item, "query", "Web search");
		return nlohmann_json::TrimmedStringValueOr(item, "tool", type);
	}

	inline std::string ToolItemArguments(const nlohmann::json& item)
	{
		if (const nlohmann::json* arguments = nlohmann_json::FindField(item, "arguments"); arguments != nullptr && !arguments->is_null())
		{
			return arguments->dump();
		}
		if (item.value("type", "") == acp_tool_items::kCommandExecution)
		{
			return nlohmann::json{{"command", std::string(nlohmann_json::StringViewOrEmpty(item, "command"))}, {"cwd", nlohmann_json::ValueOrNull(nlohmann_json::FindField(item, "cwd"))}}.dump();
		}
		return {};
	}

	inline std::string ToolItemContent(const nlohmann::json& item)
	{
		if (item.value("type", "") == acp_tool_items::kCommandExecution)
		{
			const nlohmann::json* output = nlohmann_json::FindField(item, "aggregatedOutput");
			return output == nullptr || output->is_null() ? std::string{} : output->is_string() ? output->get<std::string>() : output->dump(2);
		}
		// Preserve diffs, structured MCP results, errors, contentItems and future optional fields.
		return item.dump(2);
	}
} // namespace uam::codex
