#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "cef/uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/runtime_orchestration_services.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"
#include "common/chat/chat_folder_store.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/provider_ids.h"
#include "common/platform/platform_services.h"
#include "common/utils/time_utils.h"
#include "remote/runner_client.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Folder handlers (create, rename, delete, toggle, browse)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	int FolderFailureCode(const std::string& status_line)
	{
		if (uam::strings::ContainsAny(status_line, {"no longer exists", "not found"}))
		{
			return 404;
		}

		return 400;
	}

	nlohmann::json RecoveryChatJson(const uam::WorkspaceFolderRecoveryChat& chat)
	{
		return {
		    {"id", chat.id},
		    {"title", chat.title},
		    {"directory", chat.directory},
		    {"executionHostId", chat.execution_host_id},
		    {"reason", chat.reason},
		};
	}

	nlohmann::json RecoveryPreviewJson(const uam::WorkspaceFolderRecoveryPreview& preview)
	{
		nlohmann::json groups = nlohmann::json::array();
		for (const uam::WorkspaceFolderRecoveryGroup& group : preview.groups)
		{
			groups.push_back({
			    {"title", group.title},
			    {"directory", group.directory},
			    {"executionHostId", group.execution_host_id},
			    {"existingFolderId", group.existing_folder_id},
			    {"chatIds", group.chat_ids},
			});
		}
		const auto chats_json = [](const std::vector<uam::WorkspaceFolderRecoveryChat>& chats)
		{
			nlohmann::json result = nlohmann::json::array();
			for (const uam::WorkspaceFolderRecoveryChat& chat : chats) result.push_back(RecoveryChatJson(chat));
			return result;
		};
		return {
		    {"groups", std::move(groups)},
		    {"missing", chats_json(preview.missing)},
		    {"unavailable", chats_json(preview.unavailable)},
		    {"noLocation", chats_json(preview.no_location)},
		};
	}
} // namespace

void UamQueryHandler::HandleCreateFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Folder");
	const std::string directory = payload.value("directory", "");
	const std::string execution_host_id = payload.value("executionHostId", "local");
	std::string created_folder_id;

	if (!CreateFolder(m_app, title, directory, &created_folder_id, execution_host_id))
	{
		cb->Failure(400, m_app.status_line);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	ChatFolder* folder = ChatDomainService().FindFolderById(m_app, created_folder_id);
	if (folder == nullptr)
	{
		cb->Success("{}");
		return;
	}

	cb->Success(uam::StateSerializer::SerializeFolder(*folder).dump());
}

void UamQueryHandler::HandleRenameFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = payload.value("folderId", "");
	const std::string title = payload.value("title", "");
	const std::string directory = payload.value("directory", "");

	const ChatFolder* existing = ChatDomainService().FindFolderById(m_app, folder_id);
	if (existing != nullptr && !uam::paths::IsControllerLocalWorkspace(*existing) && directory != existing->directory)
	{
		const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, existing->execution_host_id);
		if (host == nullptr) { cb->Failure(404, "The workspace's SSH host no longer exists."); return; }
		if (!uam::execution_hosts::IsAbsoluteRemotePath(host->platform, directory) || directory.size() > 4096)
		{ cb->Failure(400, "Choose an absolute directory on this SSH host."); return; }
		const ExecutionHost observed_host = *host;
		const ChatFolder observed_folder = *existing;
		(void)uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
		    [observed_host, directory]()
		    {
			    uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			        uam::remote::SshBridgeArgv(observed_host.ssh_alias, observed_host.platform, observed_host.runner_version, observed_host.runner_directory, observed_host.runner_protocol_version), observed_host.runner_version, observed_host.runner_protocol_version);
			    uam::remote::DirectoryListing listing;
			    std::string error;
			    return client.ListDirectories(uam::paths::PathFromUtf8(directory), listing, &error)
			        ? uam::query_handler_async::AsyncSuccess({}) : uam::query_handler_async::AsyncFailure(502, error);
		    },
		    [this, browser, observed_host, observed_folder, title, directory](uam::query_handler_async::AsyncCefResult& response)
		    {
			    if (!response.ok) return;
			    const ChatFolder* folder = ChatDomainService().FindFolderById(m_app, observed_folder.id);
			    const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, observed_host.id);
			    if (folder == nullptr || host == nullptr || !uam::execution_hosts::SameConnection(*host, observed_host) || folder->execution_host_id != observed_folder.execution_host_id || folder->directory != observed_folder.directory || folder->title != observed_folder.title)
			    { response = uam::query_handler_async::AsyncFailure(409, "The SSH workspace changed while its destination was being checked."); return; }
			    if (!RenameFolderById(m_app, observed_folder.id, title, directory))
			    { response = uam::query_handler_async::AsyncFailure(FolderFailureCode(m_app.status_line), m_app.status_line); return; }
			    uam::PushStateUpdateIfChanged(browser, m_app);
		    });
		return;
	}

	if (!RenameFolderById(m_app, folder_id, title, directory))
	{
		cb->Failure(FolderFailureCode(m_app.status_line), m_app.status_line);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	StartWorkspaceDeletion(browser, {payload.value("folderId", "")}, cb);
}

void UamQueryHandler::HandleDeleteFolders(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto ids = payload.find("folderIds");
	if (ids == payload.end() || !ids->is_array() || ids->empty() ||
	    !std::ranges::all_of(*ids, [](const nlohmann::json& id) { return id.is_string(); }))
	{
		cb->Failure(400, "Workspace ids are required.");
		return;
	}
	StartWorkspaceDeletion(browser, ids->get<std::vector<std::string>>(), cb);
}

bool UamQueryHandler::HasActiveDeletionHistoryLoad() const
{
	return m_app.native_chat_load_task.running ||
	       std::ranges::any_of(m_app.native_chat_load_tasks, [](const auto& entry) { return entry.second.running; });
}

void UamQueryHandler::StartWorkspaceDeletion(CefRefPtr<CefBrowser> browser, const std::vector<std::string>& folder_ids, CefRefPtr<Callback> cb)
{
	if (HasActiveDeletionHistoryLoad())
	{
		cb->Failure(409, "Wait for native history loading to finish.");
		return;
	}
	const std::shared_ptr<uam::WorkspaceDeletionTask> task = std::make_shared<uam::WorkspaceDeletionTask>();
	if (!uam::PrepareWorkspaceDeletion(m_app, folder_ids, *task))
	{
		cb->Failure(FolderFailureCode(m_app.status_line), m_app.status_line);
		return;
	}
	StartPreparedDeletion(browser, task, false, cb);
}

void UamQueryHandler::StartChatDeletion(CefRefPtr<CefBrowser> browser, const std::vector<std::string>& chat_ids, bool single_chat, CefRefPtr<Callback> cb)
{
	if (HasActiveDeletionHistoryLoad())
	{
		cb->Failure(409, "Wait for native history loading to finish.");
		return;
	}
	const std::shared_ptr<uam::WorkspaceDeletionTask> task = std::make_shared<uam::WorkspaceDeletionTask>();
	if (!uam::PrepareChatDeletion(m_app, chat_ids, *task))
	{
		cb->Failure(409, m_app.status_line);
		return;
	}
	StartPreparedDeletion(browser, task, single_chat, cb);
}

void UamQueryHandler::StartPreparedDeletion(CefRefPtr<CefBrowser> browser,
                                             std::shared_ptr<uam::WorkspaceDeletionTask> task,
                                             bool single_chat,
                                             CefRefPtr<Callback> cb)
{
	if (HasActiveDeletionHistoryLoad())
	{
		cb->Failure(409, "Wait for native history loading to finish.");
		return;
	}
	for (const std::string& id : task->deleted_ids)
	{
		if (m_nativeHistoryRequests.contains(id))
		{
			cb->Failure(409, task->deleting_chats ? "Wait for chat history to finish loading." : "Wait for workspace history to finish loading.");
			return;
		}
	}
	m_workspaceDeletionPending = true;
	m_app.worktree_operation_chat_ids.insert(task->deleted_ids.begin(), task->deleted_ids.end());
	const auto unlock = [this, task]()
	{
		for (const std::string& id : task->deleted_ids) m_app.worktree_operation_chat_ids.erase(id);
		m_workspaceDeletionPending = false;
	};
	if (!uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb,
	    [task]()
	    {
		    return uam::StageWorkspaceDeletion(*task)
		        ? uam::query_handler_async::AsyncSuccess({})
		        : uam::query_handler_async::AsyncFailure(500, task->snapshot.status_line);
	    },
	    [this, browser, cb, task, single_chat, unlock](uam::query_handler_async::AsyncCefResult& response)
	    {
		    if (!response.ok) { unlock(); return; }
		    uam::CommitWorkspaceDeletion(m_app, *task);
		    if (task->commit_rejected)
		    {
			    response = uam::query_handler_async::AsyncFailure(500, m_app.status_line);
			    return;
		    }
		    const std::string selected_id = ChatDomainService().SelectedChatId(m_app);
		    if (single_chat)
		        response = uam::query_handler_async::AsyncSuccess({});
		    else
		        response = uam::query_handler_async::AsyncSuccess({
		            {"deletedChatIds", task->deleted_ids},
		            {"selectedChatId", selected_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(selected_id)}});
		    cb->Success(response.body);
		    response.callback_deferred = true;
		    uam::PushStateUpdateIfChanged(browser, m_app);
		    if (!CefPostTask(TID_FILE_BACKGROUND, new uam::query_handler_async::CefQueryWorkerTask(m_asyncLifetime, nullptr,
		        [task]()
		        {
			        (void)uam::CleanupWorkspaceDeletion(*task);
			        return uam::query_handler_async::AsyncSuccess({});
		        },
		        [this, browser, task, unlock](uam::query_handler_async::AsyncCefResult& result)
		        {
			        unlock();
			        m_app.status_line = result.ok ? task->snapshot.status_line
			            : (task->deleting_chats
			                ? "Chat deletion is committed. Disk cleanup will finish safely on restart."
			                : "Workspace deletion is committed. Disk cleanup will finish safely on restart.");
			        uam::PushStateUpdateIfChanged(browser, m_app);
		        })))
		    {
			    unlock();
			    m_app.status_line = task->deleting_chats
			        ? "Chat deletion is committed. Disk cleanup will finish safely on restart."
			        : "Workspace deletion is committed. Disk cleanup will finish safely on restart.";
		    }
	    })) unlock();
}

void UamQueryHandler::HandleToggleFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	(void)browser;
	const std::string folder_id = payload.value("folderId", "");
	ChatFolder* folder = ChatDomainService().FindFolderById(m_app, folder_id);
	if (!folder)
	{
		cb->Failure(404, "Folder not found: " + folder_id);
		return;
	}

	folder->collapsed = !folder->collapsed;

	if (!ChatFolderStore::Save(m_app.data_root, m_app.folders))
	{
		folder->collapsed = !folder->collapsed;
		cb->Failure(500, "Failed to persist folder state.");
		return;
	}

	cb->Success("{}");
}

void UamQueryHandler::HandleBrowseFolderDirectory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string current_value = payload.value("currentValue", "");
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(current_value);

	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(
	    payload.value("application", false) ? PlatformPathBrowseTarget::File : PlatformPathBrowseTarget::Directory,
	    initial_path, &selected_path, &error))
	{
		if (!error.empty())
		{
			cb->Failure(500, error);
		}
		else
		{
			nlohmann::json result;
			result["selectedPath"] = "";
			cb->Success(result.dump());
		}
		return;
	}

	nlohmann::json result;
	result["selectedPath"] = selected_path;
	cb->Success(result.dump());
}

void UamQueryHandler::HandleReorderFolders(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const nlohmann::json* ordered_ids = uam::nlohmann_json::FindArrayField(payload, "folderIds");
	if (ordered_ids == nullptr)
	{
		cb->Failure(400, "folderIds must be an array.");
		return;
	}

	std::unordered_map<std::string, ChatFolder> folders_by_id;
	for (const ChatFolder& folder : m_app.folders)
	{
		folders_by_id.emplace(folder.id, folder);
	}
	std::unordered_set<std::string> added;
	std::vector<ChatFolder> reordered;
	reordered.reserve(m_app.folders.size());
	for (const nlohmann::json& value : *ordered_ids)
	{
		if (!value.is_string())
		{
			continue;
		}
		const std::string id = uam::strings::Trim(value.get<std::string>());
		const auto folder = folders_by_id.find(id);
		if (folder != folders_by_id.end() && added.insert(id).second)
		{
			reordered.push_back(folder->second);
		}
	}
	for (const ChatFolder& folder : m_app.folders)
	{
		if (added.insert(folder.id).second)
		{
			reordered.push_back(folder);
		}
	}

	if (!ChatFolderStore::Save(m_app.data_root, reordered))
	{
		cb->Failure(500, "Failed to persist folder order.");
		return;
	}
	m_app.folders = std::move(reordered);
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRescanFolderChats(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = uam::strings::Trim(payload.value("folderId", ""));
	if (ChatDomainService().FindFolderById(m_app, folder_id) == nullptr)
	{
		cb->Failure(404, "Folder not found: " + folder_id);
		return;
	}
	// Review the startup snapshot. This action never scans a provider or imports a chat.
	cb->Success(nlohmann::json{{"success", true}, {"reviewStartupImports", true}, {"folderId", folder_id}}.dump());
}

void UamQueryHandler::HandlePreviewUnsortedWorkspaceFolders(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	cb->Success(RecoveryPreviewJson(uam::PreviewUnsortedWorkspaceFolders(m_app)).dump());
}

void UamQueryHandler::HandleRebuildUnsortedWorkspaceFolders(CefRefPtr<CefBrowser> browser, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	uam::WorkspaceFolderRecoveryResult result;
	if (!uam::RebuildUnsortedWorkspaceFolders(m_app, &result))
	{
		cb->Failure(409, m_app.status_line);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{
	                {"organizedChatCount", result.organized_chat_count},
	                {"createdFolderCount", result.created_folder_count},
	                {"reusedFolderCount", result.reused_folder_count},
	            }
	                .dump());
}
