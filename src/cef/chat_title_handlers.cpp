#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "cef/cef_push.h"
#include "app/chat_domain_service.h"
#include "app/chat_title_service.h"

using namespace uam::query_handler_async;

void UamQueryHandler::HandleRegenerateChatTitle(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr) { cb->Failure(404, "Chat not found."); return; }
	if (m_titleRequests.contains(chat_id)) { cb->Failure(409, "A title is already being generated for this chat."); return; }
	if (m_titleRequests.size() >= 8) { cb->Failure(429, "Wait for another title request to finish."); return; }
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat_id) || m_app.pending_chat_save_tasks.contains(chat_id))
	{ cb->Failure(409, "The chat is still being saved. Try again shortly."); return; }
	if (!m_titleQueries) m_titleQueries = CefThread::CreateThread("uam-chat-titles");
	if (!m_titleQueries) { cb->Failure(503, "The title worker is unavailable."); return; }
	const uam::ChatTitleIdentity identity = uam::ChatTitleIdentity::Capture(*chat);
	// Copy configuration only; transcript loading and serialization stay on the worker.
	const std::shared_ptr<uam::AppState> snapshot = std::make_shared<uam::AppState>();
	snapshot->data_root = m_app.data_root;
	snapshot->settings = m_app.settings;
	snapshot->folders = m_app.folders;
	snapshot->provider_profiles = m_app.provider_profiles;
	snapshot->runtime_cli_versions_by_provider_id = m_app.runtime_cli_versions_by_provider_id;
	const std::shared_ptr<std::stop_source> cancellation = std::make_shared<std::stop_source>();
	const std::shared_ptr<uam::PreparedChatTitleUpdate> update = std::make_shared<uam::PreparedChatTitleUpdate>();
	m_titleRequests[chat_id] = cancellation;
	if (!m_titleQueries->GetTaskRunner()->PostTask(new CefQueryWorkerTask(m_asyncLifetime, cb,
	    [snapshot, identity, cancellation, update]()
	    {
		    if (cancellation->stop_requested()) return AsyncFailure(409, "Title generation was cancelled.");
		    const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(snapshot->data_root, identity.id, true, nullptr, false);
		    if (!persisted || !identity.Matches(*persisted)) return AsyncFailure(409, "The chat changed. Try regenerating its title again.");
		    const uam::ChatTitleResult generated = uam::ChatTitleService::Generate(*snapshot, *persisted, cancellation->get_token());
		    if (!generated.ok) return AsyncFailure(502, generated.error);
		    *update = uam::ChatTitleService::PrepareUpdate(snapshot->data_root, identity, generated.title, cancellation->get_token());
		    if (!update->save) return AsyncFailure(409, update->error);
		    return AsyncSuccess({{"chatId", identity.id}, {"title", update->title}});
	    },
	    [this, browser, snapshot, identity, cancellation, update](AsyncCefResult& result)
	    {
		    const std::unordered_map<std::string, std::shared_ptr<std::stop_source>>::iterator request = m_titleRequests.find(identity.id);
		    if (request == m_titleRequests.end() || request->second != cancellation)
		    { result = AsyncFailure(409, "This title request is no longer active."); return; }
		    m_titleRequests.erase(request);
		    if (cancellation->stop_requested()) { result = AsyncFailure(409, "Title generation was cancelled."); return; }
		    if (!result.ok) return;
		    ChatSession* current = ChatDomainService().FindChatById(m_app, identity.id);
		    if (m_app.data_root != snapshot->data_root || current == nullptr || !update->identity.Matches(*current) ||
		        m_workspaceDeletionPending || m_app.worktree_operation_chat_ids.contains(identity.id) ||
		        m_app.pending_chat_save_at_by_chat_id.contains(identity.id) || m_app.pending_chat_save_tasks.contains(identity.id))
		    { result = AsyncFailure(409, "The chat changed. Its current name was kept. Try again."); return; }
		    if (!ChatRepository::PublishPreparedChatSave(update->save, true, current))
		    { result = AsyncFailure(409, "The chat changed or could not be saved. Its current name was kept."); return; }
		    current->title = update->title;
		    current->updated_at = update->updated_at;
		    uam::PushStateUpdateIfChanged(browser, m_app);
	    })))
	{
		m_titleRequests.erase(chat_id);
		cb->Failure(503, "The title worker is unavailable.");
	}
}

void UamQueryHandler::HandleCancelChatTitle(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::unordered_map<std::string, std::shared_ptr<std::stop_source>>::iterator request = m_titleRequests.find(payload.value("chatId", ""));
	if (request != m_titleRequests.end()) request->second->request_stop();
	cb->Success("{}");
}
