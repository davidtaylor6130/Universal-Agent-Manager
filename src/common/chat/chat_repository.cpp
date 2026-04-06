#include "common/chat/chat_repository.h"

#include "common/paths/app_paths.h"
#include "runtime/json_runtime.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace
{
	namespace fs = std::filesystem;

	std::string RoleToStringLocal(MessageRole role)
	{
		switch (role)
		{
		case MessageRole::User:
			return "user";
		case MessageRole::Assistant:
			return "assistant";
		case MessageRole::System:
			return "system";
		default:
			return "user";
		}
	}

	std::string RoleToString(MessageRole role)
	{
		switch (role)
		{
		case MessageRole::User:
			return "user";
		case MessageRole::Assistant:
			return "assistant";
		case MessageRole::System:
			return "system";
		default:
			return "user";
		}
	}

	MessageRole ParseMessageRole(const std::string& role)
	{
		if (role == "assistant")
			return MessageRole::Assistant;
		if (role == "system")
			return MessageRole::System;
		return MessageRole::User;
	}

	bool WriteTextFile(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.good())
			return false;
		out << content;
		return out.good();
	}

	std::string ReadTextFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.good())
			return "";
		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	std::string StripCarriageReturn(const std::string& line)
	{
		if (!line.empty() && line.back() == '\r')
		{
			return line.substr(0, line.size() - 1);
		}
		return line;
	}

	std::string TimestampNow()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t tt = std::chrono::system_clock::to_time_t(now);
		std::tm tm_snapshot{};
#if defined(_WIN32)
		localtime_s(&tm_snapshot, &tt);
#else
		localtime_r(&tt, &tm_snapshot);
#endif
		std::ostringstream out;
		out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
		return out.str();
	}

	JsonValue MessageToJson(const Message& msg)
	{
		JsonValue obj;
		obj.type = JsonValue::Type::Object;

		obj.object_value["role"].type = JsonValue::Type::String;
		obj.object_value["role"].string_value = RoleToStringLocal(msg.role);

		obj.object_value["content"].type = JsonValue::Type::String;
		obj.object_value["content"].string_value = msg.content;

		obj.object_value["created_at"].type = JsonValue::Type::String;
		obj.object_value["created_at"].string_value = msg.created_at;

		if (!msg.provider.empty())
		{
			obj.object_value["provider"].type = JsonValue::Type::String;
			obj.object_value["provider"].string_value = msg.provider;
		}
		if (msg.tokens_input > 0)
		{
			obj.object_value["tokens_input"].type = JsonValue::Type::Number;
			obj.object_value["tokens_input"].number_value = static_cast<double>(msg.tokens_input);
		}
		if (msg.tokens_output > 0)
		{
			obj.object_value["tokens_output"].type = JsonValue::Type::Number;
			obj.object_value["tokens_output"].number_value = static_cast<double>(msg.tokens_output);
		}
		if (msg.estimated_cost_usd > 0.0)
		{
			obj.object_value["estimated_cost_usd"].type = JsonValue::Type::Number;
			obj.object_value["estimated_cost_usd"].number_value = msg.estimated_cost_usd;
		}
		if (msg.time_to_first_token_ms > 0)
		{
			obj.object_value["time_to_first_token_ms"].type = JsonValue::Type::Number;
			obj.object_value["time_to_first_token_ms"].number_value = static_cast<double>(msg.time_to_first_token_ms);
		}
		if (msg.processing_time_ms > 0)
		{
			obj.object_value["processing_time_ms"].type = JsonValue::Type::Number;
			obj.object_value["processing_time_ms"].number_value = static_cast<double>(msg.processing_time_ms);
		}
		if (msg.interrupted)
		{
			obj.object_value["interrupted"].type = JsonValue::Type::Bool;
			obj.object_value["interrupted"].bool_value = true;
		}
		if (!msg.thoughts.empty())
		{
			obj.object_value["thoughts"].type = JsonValue::Type::String;
			obj.object_value["thoughts"].string_value = msg.thoughts;
		}
		if (!msg.tool_calls.empty())
		{
			JsonValue tc_arr;
			tc_arr.type = JsonValue::Type::Array;
			for (const auto& tc : msg.tool_calls)
			{
				JsonValue tc_obj;
				tc_obj.type = JsonValue::Type::Object;
				tc_obj.object_value["id"].type = JsonValue::Type::String;
				tc_obj.object_value["id"].string_value = tc.id;
				tc_obj.object_value["name"].type = JsonValue::Type::String;
				tc_obj.object_value["name"].string_value = tc.name;
				tc_obj.object_value["args_json"].type = JsonValue::Type::String;
				tc_obj.object_value["args_json"].string_value = tc.args_json;
				tc_obj.object_value["result_text"].type = JsonValue::Type::String;
				tc_obj.object_value["result_text"].string_value = tc.result_text;
				tc_obj.object_value["status"].type = JsonValue::Type::String;
				tc_obj.object_value["status"].string_value = tc.status;
				tc_arr.array_value.push_back(tc_obj);
			}
			obj.object_value["tool_calls"] = std::move(tc_arr);
		}
		return obj;
	}

	Message JsonToMessage(const JsonValue& obj)
	{
		Message msg;
		if (obj.object_value.contains("role"))
			msg.role = ParseMessageRole(JsonStringOrEmpty(obj.Find("role")));
		if (obj.object_value.contains("content"))
			msg.content = JsonStringOrEmpty(obj.Find("content"));
		if (obj.object_value.contains("created_at"))
			msg.created_at = JsonStringOrEmpty(obj.Find("created_at"));
		if (obj.object_value.contains("provider"))
			msg.provider = JsonStringOrEmpty(obj.Find("provider"));
		if (obj.object_value.contains("tokens_input"))
			msg.tokens_input = static_cast<int>(JsonNumberOrDefault(obj.Find("tokens_input"), 0));
		if (obj.object_value.contains("tokens_output"))
			msg.tokens_output = static_cast<int>(JsonNumberOrDefault(obj.Find("tokens_output"), 0));
		if (obj.object_value.contains("estimated_cost_usd"))
			msg.estimated_cost_usd = JsonNumberOrDefault(obj.Find("estimated_cost_usd"), 0);
		if (obj.object_value.contains("time_to_first_token_ms"))
			msg.time_to_first_token_ms = static_cast<int>(JsonNumberOrDefault(obj.Find("time_to_first_token_ms"), 0));
		if (obj.object_value.contains("processing_time_ms"))
			msg.processing_time_ms = static_cast<int>(JsonNumberOrDefault(obj.Find("processing_time_ms"), 0));
		if (obj.object_value.contains("interrupted"))
			msg.interrupted = obj.Find("interrupted")->type == JsonValue::Type::Bool && obj.Find("interrupted")->bool_value;
		if (obj.object_value.contains("thoughts"))
			msg.thoughts = JsonStringOrEmpty(obj.Find("thoughts"));
		if (obj.object_value.contains("tool_calls"))
		{
			const JsonValue* tc_arr = obj.Find("tool_calls");
			if (tc_arr && tc_arr->type == JsonValue::Type::Array)
			{
				for (const auto& tc : tc_arr->array_value)
				{
					if (tc.type != JsonValue::Type::Object)
						continue;
					ToolCall tool_call;
					tool_call.id = JsonStringOrEmpty(tc.Find("id"));
					tool_call.name = JsonStringOrEmpty(tc.Find("name"));
					tool_call.args_json = JsonStringOrEmpty(tc.Find("args_json"));
					tool_call.result_text = JsonStringOrEmpty(tc.Find("result_text"));
					tool_call.status = JsonStringOrEmpty(tc.Find("status"));
					msg.tool_calls.push_back(std::move(tool_call));
				}
			}
		}
		return msg;
	}

} // namespace

bool ChatRepository::SaveChat(const std::filesystem::path& data_root, const ChatSession& chat)
{
	const fs::path file_path = AppPaths::UamChatFilePath(data_root, chat.id);

	std::error_code ec;
	fs::create_directories(file_path.parent_path(), ec);
	if (ec)
		return false;

	JsonValue root;
	root.type = JsonValue::Type::Object;

	root.object_value["id"].type = JsonValue::Type::String;
	root.object_value["id"].string_value = chat.id;

	root.object_value["provider_id"].type = JsonValue::Type::String;
	root.object_value["provider_id"].string_value = chat.provider_id;

	root.object_value["native_session_id"].type = JsonValue::Type::String;
	root.object_value["native_session_id"].string_value = chat.native_session_id;

	root.object_value["parent_chat_id"].type = JsonValue::Type::String;
	root.object_value["parent_chat_id"].string_value = chat.parent_chat_id;

	root.object_value["branch_root_chat_id"].type = JsonValue::Type::String;
	root.object_value["branch_root_chat_id"].string_value = chat.branch_root_chat_id;

	root.object_value["branch_from_message_index"].type = JsonValue::Type::Number;
	root.object_value["branch_from_message_index"].number_value = static_cast<double>(chat.branch_from_message_index);

	root.object_value["folder_id"].type = JsonValue::Type::String;
	root.object_value["folder_id"].string_value = chat.folder_id;

	root.object_value["template_override_id"].type = JsonValue::Type::String;
	root.object_value["template_override_id"].string_value = chat.template_override_id;

	root.object_value["prompt_profile_bootstrapped"].type = JsonValue::Type::Bool;
	root.object_value["prompt_profile_bootstrapped"].bool_value = chat.prompt_profile_bootstrapped;

	root.object_value["rag_enabled"].type = JsonValue::Type::Bool;
	root.object_value["rag_enabled"].bool_value = chat.rag_enabled;

	root.object_value["title"].type = JsonValue::Type::String;
	root.object_value["title"].string_value = chat.title;

	root.object_value["created_at"].type = JsonValue::Type::String;
	root.object_value["created_at"].string_value = chat.created_at;

	root.object_value["updated_at"].type = JsonValue::Type::String;
	root.object_value["updated_at"].string_value = chat.updated_at;

	root.object_value["workspace_directory"].type = JsonValue::Type::String;
	root.object_value["workspace_directory"].string_value = chat.workspace_directory;

	root.object_value["approval_mode"].type = JsonValue::Type::String;
	root.object_value["approval_mode"].string_value = chat.approval_mode;

	root.object_value["model_id"].type = JsonValue::Type::String;
	root.object_value["model_id"].string_value = chat.model_id;

	root.object_value["extra_flags"].type = JsonValue::Type::String;
	root.object_value["extra_flags"].string_value = chat.extra_flags;

	if (!chat.rag_source_directories.empty())
	{
		JsonValue dirs;
		dirs.type = JsonValue::Type::Array;
		for (const auto& d : chat.rag_source_directories)
		{
			JsonValue item;
			item.type = JsonValue::Type::String;
			item.string_value = d;
			dirs.array_value.push_back(item);
		}
		root.object_value["rag_source_directories"] = std::move(dirs);
	}

	if (!chat.linked_files.empty())
	{
		JsonValue files;
		files.type = JsonValue::Type::Array;
		for (const auto& f : chat.linked_files)
		{
			JsonValue item;
			item.type = JsonValue::Type::String;
			item.string_value = f;
			files.array_value.push_back(item);
		}
		root.object_value["linked_files"] = std::move(files);
	}

	if (!chat.messages.empty())
	{
		JsonValue msgs;
		msgs.type = JsonValue::Type::Array;
		for (const auto& m : chat.messages)
			msgs.array_value.push_back(MessageToJson(m));
		root.object_value["messages"] = std::move(msgs);
	}

	const std::string json = SerializeJson(root);
	return WriteTextFile(file_path, json);
}

ChatSession LoadLegacyChatFromDirectory(const fs::path& chat_root)
{
	ChatSession chat;
	chat.id = chat_root.filename().string();
	chat.title = "Imported Chat";

	const fs::path meta_file = chat_root / "meta.txt";

	if (fs::exists(meta_file))
	{
		std::istringstream lines(ReadTextFile(meta_file));
		std::string line;

		while (std::getline(lines, line))
		{
			// Strip carriage return for Windows compatibility
			line = StripCarriageReturn(line);
			
			const auto equals_at = line.find('=');
			if (equals_at == std::string::npos)
				continue;

			const std::string key = line.substr(0, equals_at);
			const std::string value = line.substr(equals_at + 1);

			if (key == "provider_id")
				chat.provider_id = value;
			else if (key == "native_session_id")
				chat.native_session_id = value;
			else if (key == "parent_chat")
				chat.parent_chat_id = value;
			else if (key == "branch_root")
				chat.branch_root_chat_id = value;
			else if (key == "branch_from_index")
				chat.branch_from_message_index = static_cast<int>(JsonNumberOrDefault(nullptr, -1));
			else if (key == "folder")
				chat.folder_id = value;
			else if (key == "template_override")
				chat.template_override_id = value;
			else if (key == "prompt_profile_bootstrapped" || key == "gemini_md_bootstrapped")
				chat.prompt_profile_bootstrapped = (value == "1" || value == "true");
			else if (key == "rag_enabled")
				chat.rag_enabled = (value == "1" || value == "true");
			else if (key == "title")
				chat.title = value;
			else if (key == "created_at")
				chat.created_at = value;
			else if (key == "updated_at")
				chat.updated_at = value;
			else if (key == "rag_source_directory" && !value.empty())
				chat.rag_source_directories.push_back(value);
			else if (key == "file" && !value.empty())
				chat.linked_files.push_back(value);
		}
	}

	if (chat.created_at.empty())
		chat.created_at = TimestampNow();
	if (chat.updated_at.empty())
		chat.updated_at = chat.created_at;

	if (chat.native_session_id.empty() && !chat.id.empty())
		chat.native_session_id = chat.id;

	if (chat.branch_root_chat_id.empty())
		chat.branch_root_chat_id = chat.id;

	const fs::path messages_dir = chat_root / "messages";
	if (fs::exists(messages_dir))
	{
		std::vector<fs::path> message_files;
		std::error_code ec;
		for (const auto& file : fs::directory_iterator(messages_dir, ec))
		{
			if (!ec && file.is_regular_file() && file.path().extension() == ".txt")
				message_files.push_back(file.path());
		}

		std::sort(message_files.begin(), message_files.end());

		for (const auto& message_file : message_files)
		{
			const std::string file_name = message_file.filename().string();
			const auto underscore = file_name.find('_');
			const auto dot = file_name.find_last_of('.');
			std::string role_str = "user";

			if (underscore != std::string::npos && dot != std::string::npos && dot > underscore)
				role_str = file_name.substr(underscore + 1, dot - underscore - 1);

			Message message;
			message.role = ParseMessageRole(role_str);
			message.content = ReadTextFile(message_file);
			message.created_at = chat.updated_at;
			chat.messages.push_back(std::move(message));
		}
	}

	return chat;
}

std::vector<ChatSession> ChatRepository::LoadLocalChats(const std::filesystem::path& data_root)
{
	std::vector<ChatSession> chats;

	// Migration: Check for old-style chats in data_root/chats/ and convert to new JSON format
	const fs::path old_chats_root = AppPaths::ChatsRootPath(data_root);
	if (fs::exists(old_chats_root))
	{
		std::error_code ec;
		for (const auto& folder : fs::directory_iterator(old_chats_root, ec))
		{
			if (ec || !folder.is_directory())
				continue;

			const std::string chat_id = folder.path().filename().string();
			const fs::path new_chat_file = AppPaths::UamChatFilePath(data_root, chat_id);

			// Only migrate if not already migrated
			if (!fs::exists(new_chat_file))
			{
				ChatSession chat = LoadLegacyChatFromDirectory(folder.path());
				if (!chat.id.empty())
				{
					// Save in new format
					SaveChat(data_root, chat);
					chats.push_back(chat);
					continue;
				}
			}
		}
	}

	// Load new JSON format chats
	const fs::path chats_root = AppPaths::UamChatsRootPath(data_root);

	if (!fs::exists(chats_root))
		return chats;

	std::error_code ec;

	for (const auto& entry : fs::directory_iterator(chats_root, ec))
	{
		if (ec || !entry.is_regular_file() || entry.path().extension() != ".json")
			continue;

		const std::string file_text = ReadTextFile(entry.path());
		if (file_text.empty())
			continue;

		const auto root_opt = ParseJson(file_text);
		if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object)
			continue;

		const JsonValue& root = root_opt.value();
		ChatSession chat;

		chat.id = JsonStringOrEmpty(root.Find("id"));
		if (chat.id.empty())
			continue;

		chat.provider_id = JsonStringOrEmpty(root.Find("provider_id"));
		chat.native_session_id = JsonStringOrEmpty(root.Find("native_session_id"));
		chat.parent_chat_id = JsonStringOrEmpty(root.Find("parent_chat_id"));
		chat.branch_root_chat_id = JsonStringOrEmpty(root.Find("branch_root_chat_id"));
		chat.branch_from_message_index = static_cast<int>(JsonNumberOrDefault(root.Find("branch_from_message_index"), -1));
		chat.folder_id = JsonStringOrEmpty(root.Find("folder_id"));
		chat.template_override_id = JsonStringOrEmpty(root.Find("template_override_id"));
		chat.prompt_profile_bootstrapped = JsonBoolOrDefault(root.Find("prompt_profile_bootstrapped"), false);
		chat.rag_enabled = JsonBoolOrDefault(root.Find("rag_enabled"), true);

		const JsonValue* dirs = root.Find("rag_source_directories");
		if (dirs != nullptr && dirs->type == JsonValue::Type::Array)
		{
			for (const auto& d : dirs->array_value)
			{
				if (d.type == JsonValue::Type::String)
					chat.rag_source_directories.push_back(d.string_value);
			}
		}

		const JsonValue* files = root.Find("linked_files");
		if (files != nullptr && files->type == JsonValue::Type::Array)
		{
			for (const auto& f : files->array_value)
			{
				if (f.type == JsonValue::Type::String)
					chat.linked_files.push_back(f.string_value);
			}
		}
		chat.title = JsonStringOrEmpty(root.Find("title"));
		chat.created_at = JsonStringOrEmpty(root.Find("created_at"));
		chat.updated_at = JsonStringOrEmpty(root.Find("updated_at"));
		chat.workspace_directory = JsonStringOrEmpty(root.Find("workspace_directory"));
		chat.approval_mode = JsonStringOrEmpty(root.Find("approval_mode"));
		chat.model_id = JsonStringOrEmpty(root.Find("model_id"));
		chat.extra_flags = JsonStringOrEmpty(root.Find("extra_flags"));

		if (chat.created_at.empty())
			chat.created_at = TimestampNow();
		if (chat.updated_at.empty())
			chat.updated_at = chat.created_at;

		if (chat.branch_root_chat_id.empty())
			chat.branch_root_chat_id = chat.id;

		const JsonValue* msgs = root.Find("messages");
		if (msgs != nullptr && msgs->type == JsonValue::Type::Array)
		{
			for (const auto& m : msgs->array_value)
			{
				if (m.type == JsonValue::Type::Object)
					chat.messages.push_back(JsonToMessage(m));
			}
		}

		chats.push_back(std::move(chat));
	}

	std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) { return a.updated_at > b.updated_at; });
	return chats;
}