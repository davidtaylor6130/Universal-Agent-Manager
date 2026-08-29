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
#include "common/platform/platform_services.h"
#include "common/utils/time_utils.h"
#include "remote/runner_client.h"

#include <nlohmann/json.hpp>
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
	struct RemoteOpenCodeDiscovery
	{
		std::vector<ChatHistorySyncService::RemoteOpenCodeSession> sessions;
		std::string error;
	};

	RemoteOpenCodeDiscovery DiscoverRemoteOpenCodeSessions(
	    const ChatFolder& folder, const ExecutionHost& host)
	{
		RemoteOpenCodeDiscovery result;
		uam::remote::RunnerClient client(
		    PlatformServicesFactory::Instance().process_service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version,
		                               host.runner_directory),
		    host.runner_version);
		if (!client.Connect(&result.error)) return result;
		std::string session_id = "history-" +
		    PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (session_id == "history-")
			session_id += uam::time::SteadyEpochNanosecondsTokenNow();
		if (!client.StartProcess(
		        session_id, std::filesystem::path(folder.directory),
		        {"opencode", "session", "list", "--format", "json", "--pure",
		         "--max-count", "200"},
		        {}, &result.error) || !client.CloseProcessInput(session_id, &result.error))
		{
			(void)client.StopProcess(session_id);
			(void)client.RemoveProcess(session_id);
			return result;
		}
		std::string output;
		std::string diagnostic;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < deadline)
		{
			uam::remote::ProcessPollResult polled;
			if (!client.PollProcess(session_id, polled, &result.error)) break;
			if (output.size() + polled.standard_output.size() >
			        uam::platform::kCapturedCommandMaxOutputBytes ||
			    diagnostic.size() + polled.standard_error.size() >
			        uam::platform::kCapturedCommandMaxOutputBytes)
			{
				result.error = "Remote OpenCode session discovery returned too much output.";
				break;
			}
			output += polled.standard_output;
			diagnostic += polled.standard_error;
			if (!polled.running)
			{
				(void)client.RemoveProcess(session_id);
				if (polled.exit_code != 0)
				{
					result.error = "Remote OpenCode session discovery failed (exit " +
					               std::to_string(polled.exit_code) + "): " +
					               uam::strings::NonEmptyOrFallback(
					                   uam::strings::Trim(diagnostic), uam::strings::Trim(output));
					return result;
				}
				const nlohmann::json values = nlohmann::json::parse(output, nullptr, false);
				if (!values.is_array() || values.size() > 200)
				{
					result.error = "Remote OpenCode returned an invalid bounded session list.";
					return result;
				}
				for (const nlohmann::json& value : values)
				{
					if (!value.is_object() || !value.contains("id") || !value["id"].is_string() ||
					    !value.contains("title") || !value["title"].is_string() ||
					    !value.contains("directory") || !value["directory"].is_string() ||
					    !value.contains("created") || !value["created"].is_number_integer() ||
					    !value.contains("updated") || !value["updated"].is_number_integer())
					{
						result.sessions.clear();
						result.error = "Remote OpenCode returned malformed session metadata.";
						return result;
					}
					result.sessions.push_back({
					    value["id"].get<std::string>(), value["title"].get<std::string>(),
					    value["directory"].get<std::string>(), value["created"].get<std::int64_t>(),
					    value["updated"].get<std::int64_t>()});
				}
				return result;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (result.error.empty()) result.error = "Remote OpenCode session discovery timed out.";
		(void)client.StopProcess(session_id);
		(void)client.RemoveProcess(session_id);
		return result;
	}

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
	const std::string folder_id = payload.value("folderId", "");

	if (!DeleteFolderById(m_app, folder_id))
	{
		cb->Failure(FolderFailureCode(m_app.status_line), m_app.status_line);
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
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
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::Directory, initial_path, &selected_path, &error))
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

void UamQueryHandler::HandleRescanFolderChats(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id = uam::strings::Trim(payload.value("folderId", ""));
	const ChatFolder* matched_folder = ChatDomainService().FindFolderById(m_app, folder_id);
	if (matched_folder == nullptr)
	{
		cb->Failure(404, "Folder not found: " + folder_id);
		return;
	}
	if (!uam::paths::IsControllerLocalWorkspace(*matched_folder))
	{
		const ExecutionHost* configured = uam::execution_hosts::Find(
		    m_app.settings.execution_hosts, matched_folder->execution_host_id);
		if (configured == nullptr || configured->runner_status != "ready")
		{
			cb->Failure(409, "The selected remote helper is not ready.");
			return;
		}
		const ChatFolder folder = *matched_folder;
		const ExecutionHost host = *configured;
		const std::string selected_chat_id = ChatDomainService().SelectedChatId(m_app);
		const std::string composer_text = m_app.composer_text;
		auto discovery = std::make_shared<RemoteOpenCodeDiscovery>();
		auto codex_discovery =
		    std::make_shared<ChatHistorySyncService::RemoteCodexDiscovery>();
		uam::query_handler_async::RunAsyncCefQuery(
		    cb,
		    [folder, host, discovery, codex_discovery]()
		    {
			    *discovery = DiscoverRemoteOpenCodeSessions(folder, host);
			    *codex_discovery = ChatHistorySyncService().DiscoverRemoteCodexSessions(
			        host, folder);
			    return discovery->error.empty() || codex_discovery->error.empty() ||
			            !codex_discovery->sessions.empty()
			        ? uam::query_handler_async::AsyncSuccess({{"ok", true}})
			        : uam::query_handler_async::AsyncFailure(
			              502, "Remote history discovery failed. OpenCode: " +
			                       discovery->error + " Codex: " + codex_discovery->error);
		    },
		    [this, browser, folder, selected_chat_id, composer_text, discovery,
		     codex_discovery](
		        uam::query_handler_async::AsyncCefResult& response)
		    {
			    if (!response.ok) return;
			    const ChatFolder* current = ChatDomainService().FindFolderById(m_app, folder.id);
			    if (current == nullptr || current->execution_host_id != folder.execution_host_id ||
			        current->directory != folder.directory)
			    {
				    response = uam::query_handler_async::AsyncFailure(
				        409, "The workspace changed while remote history was being scanned.");
				    return;
			    }
			    ChatHistorySyncService::ImportResult result;
			    if (discovery->error.empty())
				    result.Merge(ChatHistorySyncService().ImportRemoteOpenCodeChatsForFolder(
				        m_app, folder.id, discovery->sessions));
			    else
				    result.Fail("OpenCode: " + discovery->error);
			    if (codex_discovery->error.empty() || !codex_discovery->sessions.empty())
				    result.Merge(ChatHistorySyncService().ImportRemoteCodexChatsForFolder(
				        m_app, folder.id, codex_discovery->sessions));
			    if (!codex_discovery->error.empty())
				    result.Fail("Codex: " + codex_discovery->error);
			    ChatHistorySyncService().MergeSidebarChatsPreservingCurrent(m_app);
			    if (!selected_chat_id.empty())
			    {
				    ChatDomainService().SelectChatById(m_app, selected_chat_id);
				    m_app.composer_text = composer_text;
			    }
			    const std::string error_detail = uam::strings::Join(result.errors, " ");
			    m_app.status_line = result.partial()
			        ? "Imported " + std::to_string(result.imported_count) +
			              " remote chat" + (result.imported_count == 1 ? "" : "s") +
			              ", but some history was unavailable: " + error_detail
			        : !result.success
			        ? "Could not rescan remote history: " + error_detail
			        : result.imported_count == 0
			            ? "No new remote chats found."
			            : "Imported " + std::to_string(result.imported_count) +
			                  " remote chat" + (result.imported_count == 1 ? "." : "s.");
			    uam::PushStateUpdate(browser, m_app);
			    response = uam::query_handler_async::AsyncSuccess({
			        {"success", result.success}, {"partial", result.partial()},
			        {"errors", result.errors}, {"importedCount", result.imported_count},
			        {"scannedCount", result.total_count}});
		    });
		return;
	}

	const std::string selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	const std::string composer_text = m_app.composer_text;
	const ChatHistorySyncService::ImportResult result =
	    ChatHistorySyncService().ImportProviderChatsForFolder(m_app, folder_id);
	ChatHistorySyncService().MergeSidebarChatsPreservingCurrent(m_app);
	if (!selected_chat_id.empty())
	{
		ChatDomainService().SelectChatById(m_app, selected_chat_id);
		m_app.composer_text = composer_text;
	}
	const std::string error_detail = uam::strings::Join(result.errors, " ");
	if (!result.success)
	{
		m_app.status_line = result.partial()
		                        ? "Imported " + std::to_string(result.imported_count) + " chat" + (result.imported_count == 1 ? "" : "s") + ", but some history could not be read: " + error_detail
		                        : "Could not rescan native history: " + error_detail;
	}
	else
	{
		m_app.status_line = result.imported_count == 0
		                        ? "No new chats found."
		                        : "Imported " + std::to_string(result.imported_count) + " chat" +
		                              (result.imported_count == 1 ? "." : "s.");
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"success", result.success}, {"partial", result.partial()}, {"errors", result.errors}, {"importedCount", result.imported_count}, {"scannedCount", result.total_count}}.dump());
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
