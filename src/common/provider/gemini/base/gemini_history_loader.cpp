#include "common/provider/gemini/base/gemini_history_loader.h"

#include "common/provider/provider_runtime.h"
#include "runtime/json_runtime.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

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

	std::string ReadTextFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);

		if (!in.good())
		{
			return "";
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

} // namespace

std::optional<ChatSession> GeminiJsonHistoryStore::ParseFile(const std::filesystem::path& file_path, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options)
{
	const std::string file_text = ReadTextFile(file_path);

	if (file_text.empty())
	{
		return std::nullopt;
	}

	const std::optional<JsonValue> root_opt = ParseJson(file_text);

	if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object)
	{
		return std::nullopt;
	}

	const JsonValue& root = root_opt.value();
	const std::string session_id = JsonStringOrEmpty(root.Find("sessionId"));

	if (session_id.empty())
	{
		return std::nullopt;
	}

	std::string short_id;
	const std::string filename = file_path.filename().string();

	if (filename.length() >= 10 && filename.ends_with(".json"))
	{
		const std::string name_part = filename.substr(0, filename.length() - 5);
		const std::string::size_type last_dash = name_part.rfind('-');

		if (name_part.rfind("session-", 0) == 0)
		{
			if (last_dash != std::string::npos && last_dash >= 20)
			{
				short_id = name_part.substr(last_dash + 1, 8);
			}
			else
			{
				short_id = name_part;
			}
		}
		else if (last_dash != std::string::npos && last_dash >= 7)
		{
			short_id = name_part;
		}
	}

	ChatSession chat;
	chat.id = short_id.empty() ? session_id.substr(0, 8) : short_id;
	chat.provider_id = provider.id;
	chat.native_session_id = session_id;
	chat.uses_native_session = true;
	chat.parent_chat_id.clear();
	chat.branch_root_chat_id = session_id;
	chat.branch_from_message_index = -1;
	chat.created_at = JsonStringOrEmpty(root.Find("startTime"));
	chat.updated_at = JsonStringOrEmpty(root.Find("lastUpdated"));

	if (chat.created_at.empty())
	{
		chat.created_at = TimestampNow();
	}

	if (chat.updated_at.empty())
	{
		chat.updated_at = chat.created_at;
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
			const std::string content = Trim(ExtractGeminiContentText(raw_message.Find("content")));

			if (content.empty())
			{
				continue;
			}

			Message message;
			message.role = ProviderRuntime::RoleFromNativeType(provider, type);
			message.content = content;
			message.created_at = timestamp.empty() ? chat.updated_at : timestamp;
			chat.messages.push_back(std::move(message));

			if (options.max_messages > 0 && chat.messages.size() >= options.max_messages)
			{
				break;
			}
		}
	}

	chat.title = "Session " + chat.created_at;

	for (const Message& message : chat.messages)
	{
		if (message.role == MessageRole::User)
		{
			std::string title = Trim(message.content);

			if (title.size() > 48)
			{
				title = title.substr(0, 45) + "...";
			}

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
	namespace fs = std::filesystem;
	std::vector<ChatSession> chats;

	if (chats_dir.empty() || !fs::exists(chats_dir) || !fs::is_directory(chats_dir))
	{
		return chats;
	}

	std::error_code ec;

	for (const auto& item : fs::directory_iterator(chats_dir, ec))
	{
		if (stop_token.stop_requested())
		{
			break;
		}

		if (ec || !item.is_regular_file() || item.path().extension() != ".json")
		{
			continue;
		}

		if (options.max_file_bytes > 0)
		{
			std::error_code size_ec;
			const std::uintmax_t file_size = fs::file_size(item.path(), size_ec);

			if (!size_ec && file_size > options.max_file_bytes)
			{
				continue;
			}
		}

		const std::optional<ChatSession> parsed = ParseFile(item.path(), provider, options);

		if (parsed.has_value())
		{
			chats.push_back(parsed.value());
		}
	}

	return chats;
}

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir, const ProviderProfile& profile, const ProviderRuntimeHistoryLoadOptions& options, std::stop_token stop_token)
{
	GeminiJsonHistoryStoreOptions native_options;
	native_options.max_file_bytes = options.native_max_file_bytes;
	native_options.max_messages = options.native_max_messages;
	return GeminiJsonHistoryStore::Load(chats_dir, profile, native_options, stop_token);
}