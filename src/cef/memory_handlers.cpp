#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/memory_library_service.h"
#include "app/memory_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Memory handlers (entries, search, scan) 
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;
using namespace uam::query_handler_async;

namespace
{
	std::vector<std::string> SearchTokens(const std::string& query)
	{
		std::istringstream in(uam::strings::ToLowerAscii(uam::strings::Trim(query)));
		std::vector<std::string> tokens;
		std::string token;
		while (in >> token)
		{
			tokens.push_back(token);
		}
		return tokens;
	}
} // namespace

void UamQueryHandler::HandleSearchChatMessages(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::vector<std::string> tokens = SearchTokens(payload.value("query", ""));
	const std::filesystem::path data_root = m_app.data_root;
	const std::string request_id = payload.value("requestId", "");
	nlohmann::json result;
	result["chatIds"] = nlohmann::json::array();
	if (!request_id.empty())
	{
		result["requestId"] = request_id;
	}
	if (tokens.empty())
	{
		cb->Success(result.dump());
		return;
	}

	RunAsyncCefQuery(cb,
	                 [data_root, tokens, request_id]()
	                 {
		                 nlohmann::json async_result;
		                 async_result["chatIds"] = nlohmann::json::array();
		                 if (!request_id.empty())
		                 {
			                 async_result["requestId"] = request_id;
		                 }

		                 std::string warning;
		                 const std::vector<ChatSession> chats = ChatRepository::LoadLocalChats(data_root, &warning);
		                 for (const ChatSession& chat : chats)
		                 {
			                 std::string haystack = uam::strings::ToLowerAscii(chat.title + " " + chat.provider_id + " " + chat.workspace_directory);
			                 for (const Message& message : chat.messages)
			                 {
				                 haystack += " " + uam::strings::ToLowerAscii(message.content);
				                 haystack += " " + uam::strings::ToLowerAscii(message.thoughts);
				                 haystack += " " + uam::strings::ToLowerAscii(message.plan_summary);
			                 }

			                 const bool matches = std::ranges::all_of(tokens, [&](const std::string& token) { return uam::strings::Contains(haystack, token); });
			                 if (matches)
			                 {
				                 async_result["chatIds"].push_back(chat.id);
			                 }
		                 }

		                 if (!warning.empty())
		                 {
			                 async_result["warning"] = warning;
		                 }
		                 return AsyncSuccess(std::move(async_result));
	                 });
}

void UamQueryHandler::HandleListMemoryEntries(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}
	const std::string request_id = payload.value("requestId", "");

	RunAsyncCefQuery(cb,
	                 [scope, request_id]()
	                 {
		                 std::string error;
		                 const std::vector<MemoryLibraryService::Entry> entries = MemoryLibraryService::ListEntries(scope, &error);
		                 if (!error.empty())
		                 {
			                 return AsyncFailure(500, error);
		                 }

		                 nlohmann::json scope_json;
		                 scope_json["scopeType"] = scope.scope_type;
		                 scope_json["folderId"] = scope.folder_id;
		                 scope_json["label"] = scope.label;
		                 scope_json["rootPath"] = uam::strings::NonEmptyOrFallback(scope.root_path.string(), "Global and project memory roots");
		                 scope_json["rootCount"] = scope.roots.size();

		                 nlohmann::json response;
		                 response["scope"] = std::move(scope_json);
		                 response["entries"] = nlohmann::json::array();
		                 for (const MemoryLibraryService::Entry& entry : entries)
		                 {
			                 response["entries"].push_back({
			                     {"id", entry.id},
			                     {"title", entry.title},
			                     {"category", entry.category},
			                     {"scope", entry.scope},
			                     {"confidence", entry.confidence},
			                     {"sourceChatId", entry.source_chat_id},
			                     {"lastObserved", entry.last_observed},
			                     {"occurrenceCount", entry.occurrence_count},
			                     {"preview", entry.preview},
			                     {"filePath", entry.file_path.string()},
			                     {"scopeType", entry.scope_type},
			                     {"folderId", entry.folder_id},
			                     {"scopeLabel", entry.scope_label},
			                     {"rootPath", entry.root_path.string()},
			                 });
		                 }
		                 return AsyncSuccess(WithOptionalRequestId(std::move(response), request_id));
	                 });
}

void UamQueryHandler::HandleCreateMemoryEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string requested_scope_type = payload.value("scopeType", "global");
	const std::string requested_folder_id = payload.value("folderId", "");
	std::string concrete_scope_type = requested_scope_type;
	std::string concrete_folder_id = requested_folder_id;
	if (uam::strings::Trim(requested_scope_type) == "all")
	{
		concrete_scope_type = payload.value("targetScopeType", "");
		concrete_folder_id = payload.value("targetFolderId", "");
		if (uam::strings::IsBlank(concrete_scope_type))
		{
			cb->Failure(400, "A target memory scope is required.");
			return;
		}
	}

	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, concrete_scope_type, concrete_folder_id, scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}

	MemoryLibraryService::Draft draft;
	draft.category = payload.value("category", "");
	draft.title = payload.value("title", "");
	draft.memory = payload.value("memory", "");
	draft.evidence = payload.value("evidence", "");
	draft.confidence = payload.value("confidence", "medium");
	draft.source_chat_id = payload.value("sourceChatId", "");

	MemoryLibraryService::Entry created;
	if (!MemoryLibraryService::CreateEntry(scope, draft, &created, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to create memory entry."));
		return;
	}

	nlohmann::json response;
	response["id"] = created.id;
	response["title"] = created.title;
	response["category"] = created.category;
	response["scope"] = created.scope;
	response["confidence"] = created.confidence;
	response["sourceChatId"] = created.source_chat_id;
	response["lastObserved"] = created.last_observed;
	response["occurrenceCount"] = created.occurrence_count;
	response["preview"] = created.preview;
	response["filePath"] = created.file_path.string();
	response["scopeType"] = created.scope_type;
	response["folderId"] = created.folder_id;
	response["scopeLabel"] = created.scope_label;
	response["rootPath"] = created.root_path.string();
	MemoryService::RefreshMemoryActivity(m_app);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(response.dump());
}

void UamQueryHandler::HandleDeleteMemoryEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}

	if (!MemoryLibraryService::DeleteEntry(scope, payload.value("entryId", ""), &error))
	{
		cb->Failure(404, FailureDetailOrFallback(error, "Failed to delete memory entry."));
		return;
	}

	MemoryService::RefreshMemoryActivity(m_app);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleOpenMemoryRoot(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}
	if (scope.scope_type == "all")
	{
		cb->Failure(400, "The all memory view has multiple roots. Reveal a memory entry instead.");
		return;
	}

	if (!MemoryService::EnsureMemoryLayout(scope.root_path))
	{
		cb->Failure(500, "Failed to create memory root.");
		return;
	}

	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInFileManager(scope.root_path, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open memory root."));
		return;
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleRevealMemoryEntry(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	MemoryLibraryService::Scope scope;
	std::string error;
	if (!MemoryLibraryService::ResolveScope(m_app, payload.value("scopeType", "global"), payload.value("folderId", ""), scope, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to resolve memory scope."));
		return;
	}

	const std::string entry_id = payload.value("entryId", "");
	if (entry_id.empty())
	{
		cb->Failure(400, "Memory entry id is required.");
		return;
	}

	const std::vector<MemoryLibraryService::Entry> entries = MemoryLibraryService::ListEntries(scope, &error);
	if (!error.empty())
	{
		cb->Failure(500, error);
		return;
	}

	for (const MemoryLibraryService::Entry& entry : entries)
	{
		if (entry.id != entry_id)
		{
			continue;
		}

		if (!PlatformServicesFactory::Instance().file_dialog_service.RevealPathInFileManager(entry.file_path, &error))
		{
			cb->Failure(500, FailureDetailOrFallback(error, "Failed to reveal memory file."));
			return;
		}

		cb->Success("{}");
		return;
	}

	cb->Failure(404, "Memory entry not found: " + entry_id);
}


void UamQueryHandler::HandleListMemoryScanCandidates(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	const std::vector<MemoryService::ManualScanCandidate> candidates = MemoryService::ListManualScanCandidates(m_app);
	nlohmann::json response;
	response["candidates"] = nlohmann::json::array();
	for (const MemoryService::ManualScanCandidate& candidate : candidates)
	{
		response["candidates"].push_back({
		    {"chatId", candidate.chat_id},
		    {"title", candidate.title},
		    {"folderId", candidate.folder_id},
		    {"folderTitle", candidate.folder_title},
		    {"providerId", candidate.provider_id},
		    {"messageCount", candidate.message_count},
		    {"memoryEnabled", candidate.memory_enabled},
		    {"memoryLastProcessedAt", candidate.memory_last_processed_at},
		    {"alreadyFullyProcessed", candidate.already_fully_processed},
		});
	}
	cb->Success(response.dump());
}

void UamQueryHandler::HandleScanCurrentChats(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::vector<std::string> chat_ids = uam::nlohmann_json::TrimmedStringArrayField(payload, "chatIds");
	if (chat_ids.empty() && uam::nlohmann_json::FindArrayField(payload, "chatIds") == nullptr)
	{
		cb->Failure(400, "chatIds is required.");
		return;
	}

	std::string error;
	int queued_count = 0;
	if (!MemoryService::QueueManualScan(m_app, chat_ids, &queued_count, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "No chats were queued for memory scanning."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	nlohmann::json response;
	response["queuedCount"] = queued_count;
	cb->Success(response.dump());
}

