#pragma once

#include "common/models/app_models.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	struct ChatFileChangeReceipt
	{
		std::string chat_id;
		std::size_t message_index = 0;
		std::string message_created_at;
		std::string provider_id;
		std::string tool_call_id;
		std::string reported_path;
		std::string workspace_relative_path;
		std::string workspace_relative_destination;
		std::string kind_json;
		std::string patch;
	};

	struct ChatFileChangeReceipts
	{
		std::vector<ChatFileChangeReceipt> receipts;
		std::size_t omitted_count = 0;
	};

	/// <summary>Extract immutable provider-reported edit patches, never infer chat ownership from repository dirt or read paths.</summary>
	class ChatFileChangeReceiptService
	{
	public:
		static bool MatchesPersistedSnapshot(const ChatSession& loaded, std::string_view chat_id, std::size_t message_count, std::string_view digest);
		static ChatFileChangeReceipts Extract(const ChatSession& chat, std::string_view workspace_root);
	};
}
