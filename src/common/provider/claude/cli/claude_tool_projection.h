#pragma once

#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace uam::claude
{
	/// <summary>Projects native Claude tool arguments identically for live and imported history.</summary>
	inline std::string ToolTitle(std::string_view name, const nlohmann::json& input)
	{
		std::string detail;
		if (name == "Bash") detail = nlohmann_json::TrimmedStringValue(input, {"command"});
		else if (name == "Read" || name == "Edit" || name == "Write" || name == "MultiEdit" || name == "NotebookEdit")
			detail = nlohmann_json::TrimmedStringValue(input, {"file_path", "notebook_path", "path"});
		else if (name == "Grep" || name == "Glob") detail = nlohmann_json::TrimmedStringValue(input, {"pattern"});
		else if (name == "WebSearch") detail = nlohmann_json::TrimmedStringValue(input, {"query"});
		else if (name == "WebFetch") detail = nlohmann_json::TrimmedStringValue(input, {"url"});
		else if (name == "Agent" || name == "Task") detail = nlohmann_json::TrimmedStringValue(input, {"description"});
		return detail.empty() ? std::string(name) : detail;
	}

	inline std::string ToolKind(std::string_view name)
	{
		if (name == "Bash") return "execute";
		if (name == "Read") return "read";
		if (name == "Grep" || name == "Glob" || name == "WebSearch" || name == "WebFetch") return "search";
		if (name == "Edit" || name == "Write" || name == "MultiEdit" || name == "NotebookEdit") return "edit";
		if (name == "Agent" || name == "Task") return "sub-agent";
		return std::string(name);
	}

	/// <summary>Only Claude's complete internal notification envelope is treated as a task event.</summary>
	inline nlohmann::json TaskNotification(std::string_view text)
	{
		text = strings::TrimAsciiView(text);
		if (!text.starts_with("<task-notification>") || !text.ends_with("</task-notification>")) return nullptr;
		nlohmann::json result = {{"type", "system"}, {"subtype", "task_notification"}};
		for (const std::string field : {"task-id", "tool-use-id", "status", "summary"})
		{
			const std::string start = "<" + field + ">";
			const std::string end = "</" + field + ">";
			const std::size_t offset = text.find(start);
			if (offset == std::string_view::npos) continue;
			const std::size_t value_start = offset + start.size();
			const std::size_t value_end = text.find(end, value_start);
			if (value_end == std::string_view::npos) return nullptr;
			result[field == "task-id" ? "task_id" : field == "tool-use-id" ? "tool_use_id" : field] =
			    std::string(text.substr(value_start, value_end - value_start));
		}
		if (!result.contains("task_id") || result["task_id"] == "" || !result.contains("status")) return nullptr;
		if (result["status"] != "completed" && result["status"] != "failed" && result["status"] != "stopped" && result["status"] != "cancelled") return nullptr;
		return result;
	}
}
