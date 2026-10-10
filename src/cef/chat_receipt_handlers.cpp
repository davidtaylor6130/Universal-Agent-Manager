#include "uam_query_handler.h"
#include "uam_query_handler_async.h"
#include "uam_query_handler_internal.h"
#include "app/chat_file_change_receipt_service.h"
#include "app/chat_domain_service.h"
#include "common/chat/chat_repository.h"
#include "common/paths/workspace_root.h"
#include "common/paths/path_utils.h"

namespace
{
	nlohmann::json SerializeReceipts(const ChatSession& chat, const std::string& workspace)
	{
		const uam::ChatFileChangeReceipts result = uam::ChatFileChangeReceiptService::Extract(chat, workspace);
		nlohmann::json receipts = nlohmann::json::array();
		for (const uam::ChatFileChangeReceipt& receipt : result.receipts)
		{
			receipts.push_back({{"chatId", receipt.chat_id}, {"messageIndex", receipt.message_index},
			    {"messageCreatedAt", receipt.message_created_at}, {"providerId", receipt.provider_id},
			    {"toolCallId", receipt.tool_call_id}, {"reportedPath", receipt.reported_path},
			    {"path", receipt.workspace_relative_path}, {"destinationPath", receipt.workspace_relative_destination}, {"kind", nlohmann::json::parse(receipt.kind_json)},
			    {"patch", receipt.patch}});
		}
		return {{"chatId", chat.id}, {"scope", "recorded-provider-edits"}, {"receipts", std::move(receipts)}, {"omittedCount", result.omitted_count}};
	}
}

void UamQueryHandler::HandleGetChatFileChangeReceipts(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string id = payload.value("chatId", "");
	ChatSession* chat = uam::query_handler_internal::FindChatOrFail(m_app, id, cb, "Chat not found: " + id);
	if (chat == nullptr) return;
	const std::string workspace = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(m_app, *chat));
	if (chat->messages_loaded)
	{
		cb->Success(SerializeReceipts(*chat, workspace).dump());
		return;
	}
	const std::filesystem::path data_root = m_app.data_root;
	const std::string host = chat->execution_host_id;
	const std::size_t count = chat->persisted_message_count;
	const std::string digest = chat->persisted_messages_digest;
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [data_root, id, workspace, host, count, digest]()
	{
		std::string warning;
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(data_root, id, true, &warning);
		if (!loaded) return uam::query_handler_async::AsyncFailure(500,
		    uam::query_handler_internal::FailureDetailOrFallback(warning, "Failed to load recorded chat edits."));
		if (loaded->execution_host_id != host ||
		    !uam::ChatFileChangeReceiptService::MatchesPersistedSnapshot(*loaded, id, count, digest))
			return uam::query_handler_async::AsyncFailure(409, "Saved chat history changed while loading recorded edits. Refresh to retry.");
		return uam::query_handler_async::AsyncSuccess(SerializeReceipts(*loaded, workspace));
	}, [this, id, workspace, host, count, digest](uam::query_handler_async::AsyncCefResult& response)
	{
		if (!response.ok) return;
		const ChatSession* current = ChatDomainService().FindChatById(m_app, id);
		if (!current)
		{
			response = uam::query_handler_async::AsyncFailure(404, "Chat was removed while its recorded edits were loading.");
			return;
		}
		if (current->execution_host_id != host ||
		    uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(m_app, *current)) != workspace)
		{
			response = uam::query_handler_async::AsyncFailure(409, "Chat workspace changed while loading recorded edits. Refresh to retry.");
			return;
		}
		if (current->messages_loaded)
			response = uam::query_handler_async::AsyncSuccess(SerializeReceipts(*current, workspace));
		else if (current->persisted_message_count != count || current->persisted_messages_digest != digest)
			response = uam::query_handler_async::AsyncFailure(409, "Chat history changed while loading recorded edits. Refresh to retry.");
	});
}
