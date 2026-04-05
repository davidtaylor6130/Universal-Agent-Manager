#include "common/provider/opencode/opencode_history_service.h"

#include "common/chat/chat_repository.h"
#include "common/runtime/json_runtime.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace
{
	namespace fs = std::filesystem;

	std::string Trim(const std::string& value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
		{
			return "";
		}
		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	std::string TimestampMsToString(int64_t ms)
	{
		auto time = static_cast<std::time_t>(ms / 1000);
		std::tm tm_snapshot{};
#if defined(_WIN32)
		localtime_s(&tm_snapshot, &time);
#else
		localtime_r(&time, &tm_snapshot);
#endif
		std::ostringstream out;
		out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
		return out.str();
	}

	std::string RunProcess(const std::vector<std::string>& args, const fs::path& cwd)
	{
		if (args.empty())
		{
			return "";
		}

		std::string cmd;
		for (std::size_t i = 0; i < args.size(); ++i)
		{
			if (i > 0)
			{
				cmd += " ";
			}
			cmd += args[i];
		}

		if (!cwd.empty())
		{
#if defined(_WIN32)
			cmd = "cd /d \"" + cwd.string() + "\" && " + cmd;
#else
			cmd = "cd \"" + cwd.string() + "\" && " + cmd;
#endif
		}

		cmd += " 2>/dev/null";

		FILE* pipe = popen(cmd.c_str(), "r");
		if (!pipe)
		{
			return "";
		}

		std::string result;
		char buffer[4096];
		while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
		{
			result += buffer;
		}
		pclose(pipe);
		return result;
	}

	MessageRole ParseMessageRole(const std::string& role)
	{
		if (role == "assistant")
			return MessageRole::Assistant;
		if (role == "system")
			return MessageRole::System;
		return MessageRole::User;
	}

	std::string JsonToMessageContent(const JsonValue* parts_array)
	{
		if (parts_array == nullptr || parts_array->type != JsonValue::Type::Array)
		{
			return "";
		}

		std::ostringstream content;
		for (const auto& part : parts_array->array_value)
		{
			if (part.type != JsonValue::Type::Object)
			{
				continue;
			}

			const std::string part_type = JsonStringOrEmpty(part.Find("type"));

			if (part_type == "text")
			{
				content << JsonStringOrEmpty(part.Find("text"));
			}
			else if (part_type == "reasoning")
			{
				content << "[Reasoning]\n" << JsonStringOrEmpty(part.Find("text")) << "\n\n";
			}
			else if (part_type == "tool")
			{
				const std::string tool_name = JsonStringOrEmpty(part.Find("tool"));
				const JsonValue* state_obj = part.Find("state");
				const std::string state = JsonStringOrEmpty(state_obj != nullptr ? state_obj->Find("status") : nullptr);

				content << "[Tool: " << tool_name << " (" << state << ")]\n";

				if (state == "completed" && state_obj != nullptr)
				{
					const std::string output = JsonStringOrEmpty(state_obj->Find("output"));
					if (!output.empty())
					{
						content << "Output: " << output << "\n";
					}
				}
				content << "\n";
			}
		}
		return content.str();
	}

	std::string JsonToThoughts(const JsonValue* parts_array)
	{
		if (parts_array == nullptr || parts_array->type != JsonValue::Type::Array)
		{
			return "";
		}

		std::ostringstream thoughts;
		for (const auto& part : parts_array->array_value)
		{
			if (part.type != JsonValue::Type::Object)
			{
				continue;
			}

			const std::string part_type = JsonStringOrEmpty(part.Find("type"));
			if (part_type == "reasoning")
			{
				thoughts << JsonStringOrEmpty(part.Find("text")) << "\n\n";
			}
		}
		return thoughts.str();
	}

	std::vector<ToolCall> JsonToToolCalls(const JsonValue* parts_array)
	{
		std::vector<ToolCall> tool_calls;
		if (parts_array == nullptr || parts_array->type != JsonValue::Type::Array)
		{
			return tool_calls;
		}

		for (const auto& part : parts_array->array_value)
		{
			if (part.type != JsonValue::Type::Object)
			{
				continue;
			}

			const std::string part_type = JsonStringOrEmpty(part.Find("type"));
			if (part_type == "tool")
			{
				ToolCall tc;
				tc.id = JsonStringOrEmpty(part.Find("callID"));
				tc.name = JsonStringOrEmpty(part.Find("tool"));

				const JsonValue* state_obj = part.Find("state");
				tc.status = JsonStringOrEmpty(state_obj != nullptr ? state_obj->Find("status") : nullptr);

				if (state_obj != nullptr)
				{
					const JsonValue* input_obj = state_obj->Find("input");
					if (input_obj != nullptr)
					{
						tc.args_json = SerializeJson(*input_obj);
					}

					const JsonValue* output_obj = state_obj->Find("output");
					tc.result_text = JsonStringOrEmpty(output_obj);
				}

				tool_calls.push_back(tc);
			}
		}
		return tool_calls;
	}

	Message JsonToMessage(const JsonValue* msg_obj)
	{
		Message msg;
		if (msg_obj == nullptr || msg_obj->type != JsonValue::Type::Object)
		{
			return msg;
		}

		const JsonValue* info_obj = msg_obj->Find("info");
		const JsonValue* parts_obj = msg_obj->Find("parts");

		if (info_obj != nullptr)
		{
			msg.role = ParseMessageRole(JsonStringOrEmpty(info_obj->Find("role")));

			const JsonValue* time_obj = info_obj->Find("time");
			if (time_obj != nullptr && time_obj->type == JsonValue::Type::Object)
			{
				int64_t created_ms = static_cast<int64_t>(JsonNumberOrDefault(time_obj->Find("created"), 0));
				int64_t completed_ms = static_cast<int64_t>(JsonNumberOrDefault(time_obj->Find("completed"), 0));
				msg.created_at = TimestampMsToString(created_ms);

				if (completed_ms > 0 && created_ms > 0)
				{
					msg.processing_time_ms = static_cast<int>(completed_ms - created_ms);
				}
			}

			const JsonValue* tokens_obj = info_obj->Find("tokens");
			if (tokens_obj != nullptr && tokens_obj->type == JsonValue::Type::Object)
			{
				msg.tokens_input = static_cast<int>(JsonNumberOrDefault(tokens_obj->Find("input"), 0));
				msg.tokens_output = static_cast<int>(JsonNumberOrDefault(tokens_obj->Find("output"), 0));
			}

			msg.estimated_cost_usd = JsonNumberOrDefault(info_obj->Find("cost"), 0.0);

			const JsonValue* model_obj = info_obj->Find("model");
			if (model_obj != nullptr && model_obj->type == JsonValue::Type::Object)
			{
				msg.provider = JsonStringOrEmpty(model_obj->Find("providerID"));
			}
		}

		if (parts_obj != nullptr)
		{
			msg.content = JsonToMessageContent(parts_obj);
			msg.thoughts = JsonToThoughts(parts_obj);
			msg.tool_calls = JsonToToolCalls(parts_obj);
		}

		if (msg.content.empty() && msg.tool_calls.empty() && msg.thoughts.empty())
		{
			msg.content = "[Empty message]";
		}

		return msg;
	}

	ChatSession ParseExportJson(const std::string& json_text, const fs::path& workspace_directory)
	{
		ChatSession chat;
		const auto root_opt = ParseJson(json_text);
		if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object)
		{
			return chat;
		}

		const JsonValue& root = root_opt.value();
		const JsonValue* info_obj = root.Find("info");
		if (info_obj != nullptr)
		{
			chat.id = JsonStringOrEmpty(info_obj->Find("id"));
			chat.native_session_id = chat.id;
			chat.title = JsonStringOrEmpty(info_obj->Find("title"));

			const JsonValue* time_obj = info_obj->Find("time");
			if (time_obj != nullptr)
			{
				int64_t created_ms = static_cast<int64_t>(JsonNumberOrDefault(time_obj->Find("created"), 0));
				int64_t updated_ms = static_cast<int64_t>(JsonNumberOrDefault(time_obj->Find("updated"), 0));
				chat.created_at = TimestampMsToString(created_ms);
				chat.updated_at = TimestampMsToString(updated_ms);
			}

			chat.workspace_directory = JsonStringOrEmpty(info_obj->Find("directory"));

			const JsonValue* parent_obj = info_obj->Find("parentID");
			if (parent_obj != nullptr && !parent_obj->string_value.empty())
			{
				chat.parent_chat_id = parent_obj->string_value;
			}
		}

		if (chat.title.empty())
		{
			chat.title = "Imported OpenCode Session";
		}

		if (chat.workspace_directory.empty())
		{
			chat.workspace_directory = workspace_directory.string();
		}

		const JsonValue* msgs_obj = root.Find("messages");
		if (msgs_obj != nullptr && msgs_obj->type == JsonValue::Type::Array)
		{
			for (const auto& msg_entry : msgs_obj->array_value)
			{
				if (msg_entry.type != JsonValue::Type::Object)
				{
					continue;
				}

				Message msg = JsonToMessage(&msg_entry);
				if (!msg.content.empty() || !msg.tool_calls.empty())
				{
					chat.messages.push_back(msg);
				}
			}
		}

		if (chat.branch_root_chat_id.empty())
		{
			chat.branch_root_chat_id = chat.id;
		}

		return chat;
	}

} // namespace

std::string OpenCodeHistoryService::RunOpenCodeCommand(const std::vector<std::string>& args, const fs::path& cwd)
{
	return RunProcess(args, cwd);
}

std::vector<ChatSession> OpenCodeHistoryService::LoadOpenCodeHistory(const fs::path& data_root, const fs::path& workspace_directory)
{
	std::vector<ChatSession> result;

	if (workspace_directory.empty())
	{
		return result;
	}

	const std::string list_output = RunOpenCodeCommand({"opencode", "session", "list", "--format", "json"}, workspace_directory);

	if (list_output.empty())
	{
		return result;
	}

	const auto root_opt = ParseJson(list_output);
	if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Array)
	{
		return result;
	}

	const JsonValue& sessions = root_opt.value();

	if (sessions.array_value.empty())
	{
		return result;
	}

	for (const auto& session : sessions.array_value)
	{
		if (session.type != JsonValue::Type::Object)
		{
			continue;
		}

		const std::string session_id = JsonStringOrEmpty(session.Find("id"));

		if (session_id.empty())
		{
			continue;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		const std::string export_output = RunOpenCodeCommand({"opencode", "export", session_id}, workspace_directory);

		if (export_output.empty())
		{
			continue;
		}

		ChatSession chat = ParseExportJson(export_output, workspace_directory);

		if (!chat.id.empty())
		{
			chat.provider_id = "opencode-cli";
			ChatRepository::SaveChat(data_root, chat);
			result.push_back(chat);
		}
	}

	return result;
}
