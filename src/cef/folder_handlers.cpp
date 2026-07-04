#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"
#include "common/chat/chat_folder_store.h"
#include "common/platform/platform_services.h"

#include <nlohmann/json.hpp>
#include <string>

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
} // namespace

void UamQueryHandler::HandleCreateFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string title = payload.value("title", "New Folder");
	const std::string directory = payload.value("directory", "");
	std::string created_folder_id;

	if (!CreateFolder(m_app, title, directory, &created_folder_id))
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

