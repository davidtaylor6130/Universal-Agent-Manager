#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "app/attachment_preview_service.h"
#include "app/chat_domain_service.h"
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
	if (chat_id.empty() || chat_id.size() > 256 || attachment_id.empty() || attachment_id.size() > 16384 || index_value > std::numeric_limits<std::size_t>::max())
	{
		cb->Failure(400, "Preview identity is invalid.");
		return;
	}
	const std::size_t index = static_cast<std::size_t>(index_value);
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (!chat)
	{
		cb->Failure(404, "Preview message is unavailable.");
		return;
	}
	const auto data_root = m_app.data_root;
	const auto incarnation = chat->metadata_incarnation;
	const auto birth = chat->created_at;
	const uam::attachment_preview::ChatSnapshot chat_snapshot = uam::attachment_preview::CaptureChatSnapshot(*chat, data_root);
	std::optional<uam::attachment_preview::AttachmentSnapshot> attachment_snapshot;
	if (chat_snapshot.messages_loaded)
	{
		if (index < chat->messages.size())
		{
			const Message& message = chat->messages[index];
			const std::vector<MessageAttachment>::const_iterator found = std::find_if(message.attachments.begin(), message.attachments.end(),
			    [&attachment_id](const MessageAttachment& value) { return value.id == attachment_id; });
			if (found != message.attachments.end() && std::count_if(message.attachments.begin(), message.attachments.end(),
			    [&attachment_id](const MessageAttachment& value) { return value.id == attachment_id; }) == 1)
				attachment_snapshot = uam::attachment_preview::AttachmentSnapshot{*found};
		}
	}
	if (chat_snapshot.messages_loaded && !attachment_snapshot)
	{
		cb->Failure(404, "Preview attachment identity is unavailable or ambiguous.");
		return;
	}
	std::shared_ptr<std::optional<uam::attachment_preview::AttachmentSnapshot>> resolved_snapshot =
	    std::make_shared<std::optional<uam::attachment_preview::AttachmentSnapshot>>(std::move(attachment_snapshot));
	const auto dispatch = [this, cb](auto worker, auto completion)
	{
		if (m_attachmentPreviewDispatcherForTests)
			return m_attachmentPreviewDispatcherForTests(cb, std::move(worker), std::move(completion));
		return uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, std::move(worker), std::move(completion));
	};
	dispatch(
	    [data_root, chat_snapshot, index, attachment_id, resolved_snapshot,
	        local = chat_snapshot.execution_host_id == uam::execution_hosts::kLocalHostId]()
	    {
		    if (!*resolved_snapshot)
			    *resolved_snapshot = uam::attachment_preview::LoadPersistedAttachmentSnapshot(data_root, chat_snapshot, index, attachment_id);
		    if (!*resolved_snapshot) return uam::query_handler_async::AsyncFailure(404,
			    "Preview attachment identity is unavailable or ambiguous.");
		    const uam::attachment_preview::Image image = uam::attachment_preview::Read(data_root, (*resolved_snapshot)->attachment, local, chat_snapshot.chat_id);
		    if (image.png.empty()) return uam::query_handler_async::AsyncSuccess({{"available", false}});
		    return uam::query_handler_async::AsyncSuccess({{"available", true}, {"mimeType", "image/png"},
		        {"width", image.width}, {"height", image.height}, {"dataBase64", uam::base64::Encode(image.png)}});
	    },
	    [this, data_root, incarnation, birth, chat_id, index, attachment_id, chat_snapshot, resolved_snapshot](uam::query_handler_async::AsyncCefResult& response)
	    {
		    if (!response.ok) return;
		    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		    if (m_app.data_root != data_root || !current || current->metadata_incarnation != incarnation || current->created_at != birth ||
		        !chat_snapshot.storage_revision || ChatRepository::ChatStorageRevision(data_root, chat_id) != chat_snapshot.storage_revision ||
		        !*resolved_snapshot || !uam::attachment_preview::MatchesCurrentAttachmentSnapshot(
			        *current, chat_snapshot, index, attachment_id, **resolved_snapshot))
		    {
			    response = uam::query_handler_async::AsyncFailure(409, "Chat history changed while its preview was loading.");
			    return;
		    }
	    });
}
