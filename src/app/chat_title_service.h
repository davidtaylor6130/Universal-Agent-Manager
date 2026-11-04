#pragma once
#include "common/models/app_models.h"
#include <stop_token>
#include "common/chat/chat_repository.h"
#include <string>
#include <string_view>

namespace uam
{
	struct AppState;
	struct ChatTitleResult
	{
		bool ok = false;
		std::string title;
		std::string error;
	};
	struct ChatTitleIdentity
	{
		std::string id, title, provider_id, execution_host_id, model_id, native_session_id, updated_at, last_opened_at;
		static ChatTitleIdentity Capture(const ChatSession& chat);
		bool Matches(const ChatSession& chat, bool include_activity = true) const;
	};
	struct PreparedChatTitleUpdate
	{
		ChatTitleIdentity identity;
		std::string title, updated_at, error;
		std::shared_ptr<PreparedChatSave> save;
	};
	/// <summary>Generates a title with a separate isolated provider worker and a bounded, quoted transcript.</summary>
	class ChatTitleService
	{
	  public:
		static std::string BuildPrompt(const ChatSession& chat);
		static ChatTitleResult ParseResult(std::string_view output, std::string_view provider_id);
		static PreparedChatTitleUpdate PrepareUpdate(const std::filesystem::path& data_root, const ChatTitleIdentity& identity, std::string title, std::stop_token stop_token = {});
		static ChatTitleResult Generate(const AppState& app, const ChatSession& chat, std::stop_token stop_token = {});
	};
}
