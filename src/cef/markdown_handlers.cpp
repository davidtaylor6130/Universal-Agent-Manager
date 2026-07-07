#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/markdown_store_service.h"
#include "app/persistence_coordinator.h"
#include "cef/cef_push.h"
#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// Markdown store handlers
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;
using namespace uam::query_handler_async;

namespace
{
	nlohmann::json SerializeMarkdownStoreEntry(const MarkdownStoreService::Entry& entry)
	{
		nlohmann::json entry_json;
		entry_json["id"] = entry.id;
		entry_json["title"] = entry.title;
		entry_json["maker"] = entry.maker;
		entry_json["review"] = entry.review;
		entry_json["dateCreated"] = entry.date_created;
		entry_json["dateUpdated"] = entry.date_updated;
		entry_json["preview"] = entry.preview;
		entry_json["filePath"] = entry.file_path.string();
		return entry_json;
	}
} // namespace

void UamQueryHandler::HandleBrowseMarkdownStoreDirectory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string current_value = payload.value("currentValue", m_app.settings.markdown_store_directory);
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(current_value);
	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::Directory, initial_path, &selected_path, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to browse Markdown Store directory."));
		return;
	}
	cb->Success(nlohmann::json{{"selectedPath", selected_path}}.dump());
}

void UamQueryHandler::HandleSetMarkdownStoreDirectory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string directory = uam::strings::Trim(payload.value("directory", ""));
	if (!directory.empty())
	{
		const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(directory);
		std::string error;
		if (!MarkdownStoreService::IsConfiguredRoot(root, &error))
		{
			cb->Failure(400, FailureDetailOrFallback(error, "Invalid Markdown Store directory."));
			return;
		}
		m_app.settings.markdown_store_directory = root.string();
	}
	else
	{
		m_app.settings.markdown_store_directory.clear();
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist Markdown Store directory."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"directory", m_app.settings.markdown_store_directory}}.dump());
}

void UamQueryHandler::HandleListMarkdownStoreEntries(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	const std::string request_id = payload.value("requestId", "");
	RunAsyncCefQuery(cb,
	                 [root, request_id]()
	                 {
		                 std::string error;
		                 std::vector<MarkdownStoreService::Entry> entries = MarkdownStoreService::ListEntries(root, &error);
		                 if (!error.empty())
		                 {
			                 return AsyncFailure(400, error);
		                 }

		                 nlohmann::json entry_json = nlohmann::json::array();
		                 for (const MarkdownStoreService::Entry& entry : entries)
		                 {
			                 entry_json.push_back(SerializeMarkdownStoreEntry(entry));
		                 }
		                 return AsyncSuccess(WithOptionalRequestId(nlohmann::json{{"directory", root.string()}, {"entries", entry_json}}, request_id));
	                 });
}

void UamQueryHandler::HandleCreateMarkdownStoreEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	MarkdownStoreService::Draft draft;
	draft.title = payload.value("title", "");
	draft.maker = payload.value("maker", "");
	draft.review = payload.value("review", "");
	draft.body = payload.value("body", "");

	MarkdownStoreService::Entry created;
	std::string error;
	if (!MarkdownStoreService::CreateEntry(root, draft, &created, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to create Markdown Store entry."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeMarkdownStoreEntry(created).dump());
}

void UamQueryHandler::HandleRevealMarkdownStoreEntry(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	const std::string file_path = payload.value("filePath", "");
	std::filesystem::path normalized_file;
	std::string error;
	if (!MarkdownStoreService::ValidateStoreFilePath(root, file_path, &normalized_file, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Invalid Markdown Store file."));
		return;
	}

	if (!PlatformServicesFactory::Instance().file_dialog_service.RevealPathInFileManager(normalized_file, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to reveal Markdown Store file."));
		return;
	}
	cb->Success("{}");
}

void UamQueryHandler::HandleEditMarkdownStoreEntry(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	const std::string file_path = payload.value("filePath", "");
	std::filesystem::path normalized_file;
	std::string error;
	if (!MarkdownStoreService::ValidateStoreFilePath(root, file_path, &normalized_file, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Invalid Markdown Store file."));
		return;
	}

	if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFileInTextEditor(normalized_file, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to open Markdown Store file for editing."));
		return;
	}
	cb->Success("{}");
}

