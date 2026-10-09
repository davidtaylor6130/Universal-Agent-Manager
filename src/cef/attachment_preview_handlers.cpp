#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "app/attachment_preview_service.h"
#include "app/chat_domain_service.h"
#include "common/chat/native_chat_identity.h"
#include "common/config/execution_host_config.h"
#include "common/utils/base64.h"
#include <algorithm>
#include <limits>

void UamQueryHandler::HandleGetAttachmentImagePreview(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.contains("chatId") || !payload["chatId"].is_string() || !payload.contains("attachmentId") ||
	    !payload["attachmentId"].is_string() || !payload.contains("messageIndex") || !payload["messageIndex"].is_number_integer() ||
	    (payload["messageIndex"].is_number_integer() && !payload["messageIndex"].is_number_unsigned() && payload["messageIndex"].get<std::int64_t>() < 0))
	{
		cb->Failure(400, "Preview requires a chat, message index and attachment id.");
		return;
	}
	const std::string chat_id = payload["chatId"].get<std::string>();
	const std::string attachment_id = payload["attachmentId"].get<std::string>();
	const std::uint64_t index_value = payload["messageIndex"].get<std::uint64_t>();
	if (chat_id.empty() || chat_id.size() > 256 || attachment_id.empty() || attachment_id.size() > 256 || index_value > std::numeric_limits<std::size_t>::max())
	{
		cb->Failure(400, "Preview identity is invalid.");
		return;
	}
	const std::size_t index = static_cast<std::size_t>(index_value);
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (!chat || !chat->messages_loaded || index >= chat->messages.size())
	{
		cb->Failure(404, "Preview message is unavailable. Load its saved history first.");
		return;
	}
	const std::vector<MessageAttachment>& attachments = chat->messages[index].attachments;
	const std::size_t matches = static_cast<std::size_t>(std::count_if(attachments.begin(), attachments.end(), [&attachment_id](const MessageAttachment& value) { return value.id == attachment_id; }));
	if (matches != 1)
	{
		cb->Failure(matches == 0 ? 404 : 409, "Preview attachment identity is unavailable or ambiguous.");
		return;
	}
	const MessageAttachment snapshot = *std::find_if(attachments.begin(), attachments.end(), [&attachment_id](const MessageAttachment& value) { return value.id == attachment_id; });
	const std::string identity = uam::chat_identity::NativeIdentityKeyForHistoryImport(*chat);
	const std::string host = chat->execution_host_id;
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
	    [data_root = m_app.data_root, snapshot, local = host == uam::execution_hosts::kLocalHostId]()
	    {
		    const uam::attachment_preview::Image image = uam::attachment_preview::Read(data_root, snapshot, local);
		    if (image.png.empty()) return uam::query_handler_async::AsyncSuccess({{"available", false}});
		    return uam::query_handler_async::AsyncSuccess({{"available", true}, {"mimeType", "image/png"},
		        {"width", image.width}, {"height", image.height}, {"dataBase64", uam::base64::Encode(image.png)}});
	    },
	    [this, chat_id, index, attachment_id, identity, host, snapshot](uam::query_handler_async::AsyncCefResult& response)
	    {
		    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		    if (!current || !current->messages_loaded || current->execution_host_id != host ||
		        uam::chat_identity::NativeIdentityKeyForHistoryImport(*current) != identity ||
		        index >= current->messages.size())
		    {
			    response = uam::query_handler_async::AsyncFailure(409, "Chat history changed while its preview was loading.");
			    return;
		    }
		    const std::vector<MessageAttachment>& values = current->messages[index].attachments;
		    if (std::count_if(values.begin(), values.end(), [&attachment_id, &snapshot](const MessageAttachment& value) { return value.id == attachment_id && value == snapshot; }) != 1 ||
		        std::count_if(values.begin(), values.end(), [&attachment_id](const MessageAttachment& value) { return value.id == attachment_id; }) != 1)
			    response = uam::query_handler_async::AsyncFailure(409, "Attachment changed while its preview was loading.");
	    });
}
