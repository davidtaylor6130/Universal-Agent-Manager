#include "common/provider/gemini/base/gemini_history_loader.h"

#include "common/paths/path_utils.h"
#include "common/paths/app_paths.h"
#include "common/provider/provider_runtime.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "runtime/json_runtime.h"

#include <sstream>

namespace fs = std::filesystem;

std::optional<ChatSession> GeminiJsonHistoryStore::ParseFile(const std::filesystem::path& file_path, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options)
{
	if (options.max_file_bytes > 0)
	{
		const std::optional<std::uintmax_t> file_size = uam::paths::FileSizeNoThrow(file_path);
		if (file_size && *file_size > options.max_file_bytes)
		{
			return std::nullopt;
		}
	}

	const std::string file_text = uam::io::ReadTextFile(file_path);

	if (file_text.empty())
	{
		return std::nullopt;
	}

	const std::optional<JsonValue> root_opt = ParseJson(file_text);

	if (!root_opt || root_opt->type != JsonValue::Type::Object)
	{
		return std::nullopt;
	}

	const JsonValue& root = *root_opt;
	const std::string session_id = uam::strings::Trim(JsonStringOrEmpty(root.Find("sessionId")));

	if (session_id.empty())
	{
		return std::nullopt;
	}

	ChatSession chat;
	chat.id = session_id;
	chat.provider_id = provider.id;
	chat.native_session_id = session_id;
	chat.parent_chat_id.clear();
	chat.branch_root_chat_id = session_id;
	chat.branch_from_message_index = -1;
	chat.created_at = JsonStringOrEmpty(root.Find("startTime"));
	chat.updated_at = JsonStringOrEmpty(root.Find("lastUpdated"));
	chat.last_opened_at = chat.updated_at;

	if (chat.created_at.empty())
	{
		chat.created_at = uam::time::TimestampNow();
	}

	if (chat.updated_at.empty())
	{
		chat.updated_at = chat.created_at;
	}

	if (chat.last_opened_at.empty())
	{
		chat.last_opened_at = chat.updated_at;
	}

	const JsonValue* messages = root.Find("messages");

	if (messages != nullptr && messages->type == JsonValue::Type::Array)
	{
		for (const JsonValue& raw_message : messages->array_value)
		{
			if (raw_message.type != JsonValue::Type::Object)
			{
				continue;
			}

			const std::string type = JsonStringOrEmpty(raw_message.Find("type"));
			const std::string timestamp = JsonStringOrEmpty(raw_message.Find("timestamp"));
			const std::string content = uam::strings::Trim(ExtractGeminiContentText(raw_message.Find("content")));

			Message message;
			message.role = ProviderRuntime::RoleFromNativeType(provider, type);
			message.content = content;
			message.created_at = uam::strings::NonEmptyOrFallback(timestamp, chat.updated_at);

			const JsonValue* tool_calls = raw_message.Find("toolCalls");
			if (tool_calls != nullptr && tool_calls->type == JsonValue::Type::Array)
			{
				for (const JsonValue& tc : tool_calls->array_value)
				{
					if (tc.type != JsonValue::Type::Object)
					{
						continue;
					}

					ToolCall tool_call;
					tool_call.id = JsonStringOrEmpty(tc.Find("id"));
					tool_call.name = JsonStringOrEmpty(tc.Find("name"));
					tool_call.status = JsonStringOrEmpty(tc.Find("status"));

					const JsonValue* args = tc.Find("args");
					if (args && args->type == JsonValue::Type::Object)
					{
						tool_call.args_json = SerializeJson(*args);
					}

					const JsonValue* result = tc.Find("result");
					if (result)
					{
						tool_call.result_text = ExtractGeminiContentText(result);
					}

					message.tool_calls.push_back(std::move(tool_call));
				}
			}

			const JsonValue* thoughts = raw_message.Find("thoughts");
			if (thoughts != nullptr && thoughts->type == JsonValue::Type::Array)
			{
				std::ostringstream thought_out;
				bool has_thought_text = false;

				for (const JsonValue& thought : thoughts->array_value)
				{
					const std::string thought_text = ExtractGeminiContentText(&thought);
					if (!thought_text.empty())
					{
						if (has_thought_text)
						{
							thought_out << "\n";
						}

						thought_out << thought_text;
						has_thought_text = true;
					}
				}

				message.thoughts = thought_out.str();
			}

			if (message.content.empty() && uam::strings::IsBlank(message.thoughts) && message.tool_calls.empty())
			{
				continue;
			}

			chat.messages.push_back(std::move(message));

			if (options.max_messages > 0 && chat.messages.size() >= options.max_messages)
			{
				break;
			}
		}
	}

	// Empty Gemini sessions are placeholder artifacts, not user-visible chats.
	// Importing or restoring them causes undeletable ghost entries that contain
	// no user messages.
	if (chat.messages.empty())
	{
		return std::nullopt;
	}

	chat.title = "Session " + chat.created_at;

	for (const Message& message : chat.messages)
	{
		if (message.role == MessageRole::User)
		{
			const std::string title = uam::strings::TrimAndElide(message.content, 48);

			if (!title.empty())
			{
				chat.title = title;
			}

			break;
		}
	}

	return chat;
}

std::vector<ChatSession> GeminiJsonHistoryStore::Load(const std::filesystem::path& chats_dir, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options, std::stop_token stop_token)
{
	std::vector<ChatSession> chats;

	if (chats_dir.empty() || !uam::paths::IsDirectoryNoThrow(chats_dir))
	{
		return chats;
	}

	std::error_code ec;

	for (fs::directory_iterator it(chats_dir, ec), end; !ec && it != end; it.increment(ec))
	{
		const fs::directory_entry& item = *it;
		if (stop_token.stop_requested())
		{
			break;
		}

		if (!uam::paths::IsRegularFileWithExtensionNoThrow(item, ".json"))
		{
			continue;
		}

		if (options.max_file_bytes > 0)
		{
			const std::optional<std::uintmax_t> file_size = uam::paths::FileSizeNoThrow(item.path());
			if (file_size && *file_size > options.max_file_bytes)
			{
				continue;
			}
		}

		const std::optional<ChatSession> parsed = ParseFile(item.path(), provider, options);

		if (parsed)
		{
			chats.push_back(*parsed);
		}
	}

	return chats;
}

bool GeminiJsonHistoryStore::SaveFile(const std::filesystem::path& file_path, const ChatSession& chat)
{
	JsonValue root = uam::json::Object();

	const std::string session_id = uam::strings::Trim(uam::strings::NonEmptyOrFallback(chat.native_session_id, chat.id));

	if (session_id.empty())
	{
		return false;
	}

	uam::json::SetString(root, "sessionId", session_id);
	uam::json::SetString(root, "startTime", chat.created_at.empty() ? uam::time::TimestampNow() : chat.created_at);
	uam::json::SetString(root, "lastUpdated", chat.updated_at.empty() ? uam::time::TimestampNow() : chat.updated_at);

	JsonValue messages_arr = uam::json::Array();

	for (const Message& msg : chat.messages)
	{
		JsonValue msg_obj = uam::json::Object();
			uam::json::SetString(msg_obj, "type", msg.role == MessageRole::User ? "user" : msg.role == MessageRole::System ? "info" : "model");
		uam::json::SetString(msg_obj, "timestamp", msg.created_at.empty() ? uam::time::TimestampNow() : msg.created_at);
		uam::json::SetString(msg_obj, "content", msg.content);
		uam::json::PushValue(messages_arr, std::move(msg_obj));
	}

	uam::json::SetValue(root, "messages", std::move(messages_arr));

	return uam::io::WriteTextFile(file_path, SerializeJson(root));
}

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir, const ProviderProfile& profile, const ProviderRuntimeHistoryLoadOptions& options, std::stop_token stop_token)
{
	GeminiJsonHistoryStoreOptions native_options;
	native_options.max_file_bytes = options.native_max_file_bytes;
	native_options.max_messages = options.native_max_messages;
	return GeminiJsonHistoryStore::Load(chats_dir, profile, native_options, stop_token);
}

std::vector<ProviderChatSource> DiscoverGeminiTmpChatSources(std::string* error_out)
{
	std::vector<ProviderChatSource> sources;
	if (error_out != nullptr) error_out->clear();
	const fs::path gemini_home = AppPaths::GeminiHomePath();
	const fs::path tmp_root = gemini_home / "tmp";

	std::error_code ec;
	const bool tmp_exists = fs::exists(tmp_root, ec);
	if (ec)
	{
		if (error_out != nullptr) *error_out = "Could not inspect Gemini history: " + ec.message();
		return sources;
	}
	if (!tmp_exists)
	{
		return sources;
	}
	if (!fs::is_directory(tmp_root, ec) || ec)
	{
		if (error_out != nullptr) *error_out = ec ? "Could not inspect Gemini history: " + ec.message() : "Gemini history path is not a directory.";
		return sources;
	}

	for (fs::directory_iterator it(tmp_root, ec), end; !ec && it != end; it.increment(ec))
	{
		const fs::directory_entry& item = *it;
		if (!uam::paths::IsDirectoryEntryNoThrow(item))
		{
			continue;
		}

		const fs::path project_root_file = item.path() / ".project_root";
		if (!uam::paths::PathExistsNoThrow(project_root_file))
		{
			continue;
		}

		const fs::path chats_dir = item.path() / "chats";
		if (!uam::paths::IsDirectoryNoThrow(chats_dir))
		{
			continue;
		}

		const std::string project_root = uam::strings::Trim(uam::io::ReadTextFile(project_root_file));
		if (project_root.empty())
		{
			continue;
		}

		ProviderChatSource source;
		source.folder_title = item.path().filename().string();
		source.folder_directory = project_root;
		source.chats_dir = chats_dir;
		sources.push_back(std::move(source));
	}
	if (ec && error_out != nullptr) *error_out = "Could not finish scanning Gemini history: " + ec.message();
	return sources;
}
