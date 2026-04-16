#include "common/chat/chat_repository.h"

#include "common/paths/app_paths.h"
#include "common/utils/io_utils.h"
#include "common/utils/time_utils.h"
#include "common/runtime/json_runtime.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace
{
	namespace fs = std::filesystem;
	using namespace uam::io;
	using namespace uam::time;

	MessageRole ParseMessageRole(const std::string& role)
	{
		if (role == "assistant")
			return MessageRole::Assistant;
		if (role == "system")
			return MessageRole::System;
		return MessageRole::User;
	}

	std::string StripCarriageReturn(const std::string& line)
	{
		if (!line.empty() && line.back() == '\r')
		{
			return line.substr(0, line.size() - 1);
		}
		return line;
	}

	bool IsSafeChatId(const std::string& chat_id)
	{
		if (chat_id.empty() || chat_id == "." || chat_id == "..")
		{
			return false;
		}

		return chat_id.find('/') == std::string::npos &&
		       chat_id.find('\\') == std::string::npos &&
		       chat_id.find("..") == std::string::npos;
	}

	JsonValue MessageToJson(const Message& msg)
	{
		JsonValue obj;
		obj.type = JsonValue::Type::Object;

		obj.object_value["role"].type = JsonValue::Type::String;
		obj.object_value["role"].string_value = RoleToString(msg.role);

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

	JsonValue StringArrayToJson(const std::vector<std::string>& values)
	{
		JsonValue arr;
		arr.type = JsonValue::Type::Array;

		for (const std::string& value : values)
		{
			JsonValue item;
			item.type = JsonValue::Type::String;
			item.string_value = value;
			arr.array_value.push_back(std::move(item));
		}

		return arr;
	}

	std::vector<std::string> JsonStringArrayOrEmpty(const JsonValue* value)
	{
		std::vector<std::string> out;

		if (value == nullptr || value->type != JsonValue::Type::Array)
		{
			return out;
		}

		for (const JsonValue& item : value->array_value)
		{
			if (item.type == JsonValue::Type::String)
			{
				out.push_back(item.string_value);
			}
		}

		return out;
	}

	bool ToolCallsEquivalentForRecovery(const std::vector<ToolCall>& lhs, const std::vector<ToolCall>& rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < lhs.size(); ++i)
		{
			const ToolCall& left = lhs[i];
			const ToolCall& right = rhs[i];

			if (left.id != right.id ||
			    left.name != right.name ||
			    left.args_json != right.args_json ||
			    left.result_text != right.result_text ||
			    left.status != right.status)
			{
				return false;
			}
		}

		return true;
	}

	bool MessagesEquivalentForRecovery(const std::vector<Message>& lhs, const std::vector<Message>& rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < lhs.size(); ++i)
		{
			const Message& left = lhs[i];
			const Message& right = rhs[i];

			if (left.role != right.role ||
			    left.content != right.content ||
			    left.created_at != right.created_at ||
			    left.provider != right.provider ||
			    left.tokens_input != right.tokens_input ||
			    left.tokens_output != right.tokens_output ||
			    left.estimated_cost_usd != right.estimated_cost_usd ||
			    left.time_to_first_token_ms != right.time_to_first_token_ms ||
			    left.processing_time_ms != right.processing_time_ms ||
			    left.interrupted != right.interrupted ||
			    left.thoughts != right.thoughts ||
			    !ToolCallsEquivalentForRecovery(left.tool_calls, right.tool_calls))
			{
				return false;
			}
		}

		return true;
	}

	struct LoadChatResult
	{
		std::optional<ChatSession> chat;
		std::string error;
	};

	bool ChatsEquivalentForRecovery(const ChatSession& lhs, const ChatSession& rhs)
	{
		return lhs.id == rhs.id &&
		       lhs.provider_id == rhs.provider_id &&
		       lhs.native_session_id == rhs.native_session_id &&
		       lhs.parent_chat_id == rhs.parent_chat_id &&
		       lhs.branch_root_chat_id == rhs.branch_root_chat_id &&
		       lhs.branch_from_message_index == rhs.branch_from_message_index &&
		       lhs.folder_id == rhs.folder_id &&
		       lhs.title == rhs.title &&
		       lhs.created_at == rhs.created_at &&
		       lhs.updated_at == rhs.updated_at &&
		       lhs.linked_files == rhs.linked_files &&
		       lhs.workspace_directory == rhs.workspace_directory &&
		       lhs.approval_mode == rhs.approval_mode &&
		       lhs.model_id == rhs.model_id &&
		       lhs.extra_flags == rhs.extra_flags &&
		       MessagesEquivalentForRecovery(lhs.messages, rhs.messages);
	}

	LoadChatResult ParseLocalChatFile(const fs::path& path)
	{
		const std::string file_text = ReadTextFile(path);
		if (file_text.empty())
		{
			return {std::nullopt, "is empty"};
		}

		const auto root_opt = ParseJson(file_text);
		if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object)
		{
			return {std::nullopt, "contains invalid JSON"};
		}

		const JsonValue& root = root_opt.value();
		ChatSession chat;

			chat.id = JsonStringOrEmpty(root.Find("id"));
			if (chat.id.empty())
			{
				return {std::nullopt, "is missing a chat id"};
			}

			if (!IsSafeChatId(chat.id))
			{
				return {std::nullopt, "contains an unsafe chat id"};
			}

		chat.provider_id = JsonStringOrEmpty(root.Find("provider_id"));
		chat.native_session_id = JsonStringOrEmpty(root.Find("native_session_id"));
		chat.parent_chat_id = JsonStringOrEmpty(root.Find("parent_chat_id"));
		chat.branch_root_chat_id = JsonStringOrEmpty(root.Find("branch_root_chat_id"));
		if (root.object_value.contains("branch_from_message_index"))
			chat.branch_from_message_index = static_cast<int>(JsonNumberOrDefault(root.Find("branch_from_message_index"), -1));
		chat.folder_id = JsonStringOrEmpty(root.Find("folder_id"));
		chat.title = JsonStringOrEmpty(root.Find("title"));
		chat.created_at = JsonStringOrEmpty(root.Find("created_at"));
		chat.updated_at = JsonStringOrEmpty(root.Find("updated_at"));
		if (const JsonValue* linked_files = root.Find("linked_files"))
			chat.linked_files = JsonStringArrayOrEmpty(linked_files);
		chat.workspace_directory = JsonStringOrEmpty(root.Find("workspace_directory"));
		chat.approval_mode = JsonStringOrEmpty(root.Find("approval_mode"));
		chat.model_id = JsonStringOrEmpty(root.Find("model_id"));
		chat.extra_flags = JsonStringOrEmpty(root.Find("extra_flags"));

		if (chat.created_at.empty())
			chat.created_at = TimestampNow();
		if (chat.updated_at.empty())
			chat.updated_at = chat.created_at;
		if (chat.native_session_id.empty())
			chat.native_session_id = chat.id;
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

		return {std::move(chat), ""};
	}

} // namespace

bool ChatRepository::SaveChat(const std::filesystem::path& data_root, const ChatSession& chat)
{
	static std::mutex save_mutex;
	std::lock_guard<std::mutex> lock(save_mutex);

	const fs::path file_path = AppPaths::UamChatFilePath(data_root, chat.id);

	if (!IsSafeChatId(chat.id))
	{
		return false;
	}

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

	root.object_value["title"].type = JsonValue::Type::String;
	root.object_value["title"].string_value = chat.title;

	root.object_value["created_at"].type = JsonValue::Type::String;
	root.object_value["created_at"].string_value = chat.created_at;

	root.object_value["updated_at"].type = JsonValue::Type::String;
	root.object_value["updated_at"].string_value = chat.updated_at;

	root.object_value["linked_files"] = StringArrayToJson(chat.linked_files);

	root.object_value["workspace_directory"].type = JsonValue::Type::String;
	root.object_value["workspace_directory"].string_value = chat.workspace_directory;

	root.object_value["approval_mode"].type = JsonValue::Type::String;
	root.object_value["approval_mode"].string_value = chat.approval_mode;

	root.object_value["model_id"].type = JsonValue::Type::String;
	root.object_value["model_id"].string_value = chat.model_id;

	root.object_value["extra_flags"].type = JsonValue::Type::String;
	root.object_value["extra_flags"].string_value = chat.extra_flags;

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
			{
				char* parse_end = nullptr;
				const long parsed = std::strtol(value.c_str(), &parse_end, 10);
				chat.branch_from_message_index = (parse_end != nullptr && *parse_end == '\0') ? static_cast<int>(parsed) : -1;
			}
			else if (key == "folder")
				chat.folder_id = value;
			else if (key == "title")
				chat.title = value;
			else if (key == "created_at")
				chat.created_at = value;
			else if (key == "updated_at")
				chat.updated_at = value;
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

std::vector<ChatSession> ChatRepository::LoadLocalChats(const std::filesystem::path& data_root, std::string* warning_out)
{
	std::vector<ChatSession> chats;
	std::unordered_set<std::string> migrated_chat_ids;
	if (warning_out != nullptr)
	{
		warning_out->clear();
	}

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
					if (SaveChat(data_root, chat))
					{
						chats.push_back(chat);
						migrated_chat_ids.insert(chat.id);
					}
					else if (warning_out != nullptr)
					{
						*warning_out = "Failed to migrate legacy chat folder: " + folder.path().string();
					}
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

		const LoadChatResult primary_chat = ParseLocalChatFile(entry.path());
		if (primary_chat.chat.has_value())
		{
			if (migrated_chat_ids.find(primary_chat.chat->id) != migrated_chat_ids.end())
			{
				continue;
			}

			const fs::path backup_path = entry.path().string() + ".bak";
			if (fs::exists(backup_path))
			{
				const LoadChatResult backup_chat = ParseLocalChatFile(backup_path);
				if (backup_chat.chat.has_value() && !ChatsEquivalentForRecovery(primary_chat.chat.value(), backup_chat.chat.value()))
				{
					if (warning_out != nullptr)
					{
						*warning_out = "Recovered chat file " + entry.path().string() + " differs from backup " + backup_path.string() + ".";
					}
				}
			}

			chats.push_back(primary_chat.chat.value());
			continue;
		}

		const fs::path backup_path = entry.path().string() + ".bak";
		if (fs::exists(backup_path))
		{
			LoadChatResult backup_chat = ParseLocalChatFile(backup_path);
			if (backup_chat.chat.has_value())
			{
				if (migrated_chat_ids.find(backup_chat.chat->id) != migrated_chat_ids.end())
				{
					continue;
				}

					ChatSession recovered = backup_chat.chat.value();
					const std::string recovered_id = entry.path().stem().string();
					recovered.id = recovered_id;
					if (recovered.native_session_id.empty())
					{
						recovered.native_session_id = recovered.id;
					}
					const std::string previous_id = backup_chat.chat->id;
					if (recovered.branch_root_chat_id.empty() || recovered.branch_root_chat_id == previous_id)
						recovered.branch_root_chat_id = recovered.id;

				if (SaveChat(data_root, recovered))
				{
					chats.push_back(recovered);
				}
				else if (warning_out != nullptr)
				{
					*warning_out = "Recovered backup file " + backup_path.string() + ", but failed to save " + entry.path().string() + ".";
				}
			}
			else if (warning_out != nullptr)
			{
				*warning_out = "Skipped corrupted backup file " + backup_path.string() + ": " + backup_chat.error + ".";
			}
		}
		else if (warning_out != nullptr && !primary_chat.error.empty())
		{
			*warning_out = "Skipped malformed chat file " + entry.path().string() + ": " + primary_chat.error + ".";
		}
	}

	for (const auto& entry : fs::directory_iterator(chats_root, ec))
	{
		if (ec || !entry.is_regular_file() || entry.path().extension() != ".bak")
			continue;

		const fs::path primary_path = entry.path().parent_path() / entry.path().stem();
		if (fs::exists(primary_path))
		{
			continue;
		}

		LoadChatResult backup_chat = ParseLocalChatFile(entry.path());
		if (!backup_chat.chat.has_value())
		{
			if (warning_out != nullptr)
			{
				*warning_out = "Skipped corrupted backup file " + entry.path().string() + ": " + backup_chat.error + ".";
			}
			continue;
		}

			ChatSession recovered = backup_chat.chat.value();
			const std::string recovered_id = primary_path.stem().string();
			recovered.id = recovered_id;
			if (recovered.native_session_id.empty())
			{
				recovered.native_session_id = recovered.id;
			}
			const std::string previous_id = backup_chat.chat->id;
			if (recovered.branch_root_chat_id.empty() || recovered.branch_root_chat_id == previous_id)
				recovered.branch_root_chat_id = recovered.id;

		if (migrated_chat_ids.find(recovered.id) != migrated_chat_ids.end())
		{
			continue;
		}

		if (SaveChat(data_root, recovered))
		{
			chats.push_back(recovered);
		}
		else if (warning_out != nullptr)
		{
			*warning_out = "Recovered backup file " + entry.path().string() + ", but failed to save " + primary_path.string() + ".";
		}
	}

	std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) { return a.updated_at > b.updated_at; });
	return chats;
}
