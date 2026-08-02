#include "common/runtime/acp/acp_session_internal.h"

#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_content.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <string>
#include <vector>

namespace uam::acp_detail
{

nlohmann::json BuildClaudeInputMessage(const std::string& text)
{
	return {
	    {"type", uam::acp_claude_stream::kMessageTypeUser},
	    {"message",
	     {
	         {"role", uam::acp_claude_stream::kMessageTypeUser},
	         {"content", nlohmann::json::array({uam::acp_content::TextPart(text)})},
	     }},
	};
}

std::string ContentTextFromJson(const nlohmann::json& content)
{
	if (content.is_string())
	{
		return content.get_ref<const std::string&>();
	}

	if (content.is_object())
	{
		const std::string type = JsonDiagnosticStringValue(content, "type");
		if (type == uam::acp_content::kTextType && uam::acp_content::HasTextField(content))
		{
			return uam::acp_content::TextFieldOrEmpty(content);
		}

		if (uam::acp_content::HasTextField(content))
		{
			return uam::acp_content::TextFieldOrEmpty(content);
		}

		if (const nlohmann::json* nested_content = uam::nlohmann_json::FindField(content, "content"); nested_content != nullptr)
		{
			return ContentTextFromJson(*nested_content);
		}
	}

	if (content.is_array())
	{
		std::vector<std::string> content_pieces;
		for (const nlohmann::json& item : content)
		{
			const std::string piece = uam::strings::Trim(ContentTextFromJson(item));
			if (piece.empty())
			{
				continue;
			}
			content_pieces.push_back(piece);
		}
		return uam::strings::JoinNonEmpty(content_pieces, "\n");
	}

	return "";
}

	std::string ToolCallContentTextFromJson(const nlohmann::json& tool_call)
{
	for (const char* field : {"content", "rawOutput", "rawInput"})
	{
		const nlohmann::json* value = uam::nlohmann_json::FindField(tool_call, field);
		if (value == nullptr || value->is_null())
		{
			continue;
		}

		std::string text = uam::strings::Trim(ContentTextFromJson(*value));
		if (text.empty() && value->is_object())
		{
			const std::string command = JsonDiagnosticStringValue(*value, "command");
			const std::string file_name = JsonDiagnosticStringValue(*value, "fileName");
			const std::string diff = JsonDiagnosticStringValue(*value, "diff");
			const std::string path = JsonDiagnosticStringValue(*value, "path");
			const std::string url = JsonDiagnosticStringValue(*value, "url");
			text = !command.empty() ? command : !file_name.empty() ? file_name + (diff.empty() ? "" : "\n\n" + diff) : !path.empty() ? path : !url.empty() ? url : !diff.empty() ? diff : value->dump(2);
		}
		else if (text.empty() && !value->empty())
		{
			text = value->dump(2);
		}
		if (!text.empty())
		{
			return text;
		}
	}

	const nlohmann::json locations = JsonArrayValue(tool_call, "locations");
	std::vector<std::string> paths;
	for (const nlohmann::json& location : locations)
	{
		const std::string path = JsonDiagnosticStringValue(location, "path");
		if (!path.empty())
		{
			paths.push_back(path);
		}
	}
	return uam::strings::JoinNonEmpty(paths, "\n");
}

std::string ClaudeContentTextFromMessage(const nlohmann::json& message)
{
	if (!message.is_object())
	{
		return "";
	}
	if (const nlohmann::json* content = uam::nlohmann_json::FindField(message, "content"); content != nullptr)
	{
		return ContentTextFromJson(*content);
	}
	return "";
}

std::string StripLeadingLineBreaks(std::string value)
{
	while (!value.empty() && (value.front() == '\n' || value.front() == '\r'))
	{
		value.erase(value.begin());
	}
	return value;
}

bool StartsWithLineBreak(const std::string& value)
{
	return !value.empty() && (value.front() == '\n' || value.front() == '\r');
}

void AppendThoughtText(std::string& target, const std::string& chunk, bool starts_new_block)
{
	if (chunk.empty())
	{
		return;
	}

	if (!target.empty() && starts_new_block)
	{
		target += "\n\n";
	}
	target += chunk;
}

} // namespace uam::acp_detail
