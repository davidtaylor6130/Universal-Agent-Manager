#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/markdown_store_service.h"
#include "app/persistence_coordinator.h"
#include "cef/cef_push.h"
#include "common/platform/platform_services.h"
#include "common/paths/path_utils.h"
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
		entry_json["body"] = entry.body;
		entry_json["favorite"] = entry.favorite;
		entry_json["sourceProvider"] = entry.source_provider;
		entry_json["sourcePath"] = entry.source_path;
		entry_json["commandName"] = entry.command_name;
		entry_json["group"] = entry.group;
		entry_json["filePath"] = uam::paths::Utf8PathString(entry.file_path);
		return entry_json;
	}

	nlohmann::json SerializeImportCandidate(const MarkdownStoreService::ImportCandidate& candidate)
	{
		return {
			{"id", candidate.id}, {"title", candidate.title}, {"maker", candidate.maker}, {"review", candidate.review},
			{"preview", candidate.preview}, {"sourceProvider", candidate.source_provider},
			{"sourcePath", uam::paths::Utf8PathString(candidate.source_path)}, {"supported", candidate.supported},
			{"validationError", candidate.validation_error}, {"collisionPath", uam::paths::Utf8PathString(candidate.collision_path)},
		};
	}

	MarkdownStoreService::ImportConflictAction ParseConflictAction(std::string value)
	{
		value = uam::strings::TrimAndLowerAscii(value);
		if (value == "replace") return MarkdownStoreService::ImportConflictAction::Replace;
		if (value == "separate") return MarkdownStoreService::ImportConflictAction::Separate;
		return MarkdownStoreService::ImportConflictAction::Skip;
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
	const std::string previous_directory = m_app.settings.markdown_store_directory;
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
		m_app.settings.markdown_store_directory = uam::paths::Utf8PathString(root);
	}
	else
	{
		m_app.settings.markdown_store_directory.clear();
	}

	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings.markdown_store_directory = previous_directory;
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
		                 return AsyncSuccess(WithOptionalRequestId(nlohmann::json{{"directory", uam::paths::Utf8PathString(root)}, {"entries", entry_json}}, request_id));
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
	draft.group = payload.value("group", "");

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

void UamQueryHandler::HandleUpdateMarkdownStoreEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	MarkdownStoreService::Draft draft{payload.value("title", ""), payload.value("maker", ""), payload.value("review", ""), payload.value("body", ""), payload.value("group", "")};
	MarkdownStoreService::Entry updated;
	std::string error;
	if (!MarkdownStoreService::UpdateEntry(root, payload.value("filePath", ""), draft, &updated, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to update Markdown Store entry."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeMarkdownStoreEntry(updated).dump());
}

void UamQueryHandler::HandleSetMarkdownStoreFavorite(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	MarkdownStoreService::Entry updated;
	std::string error;
	if (!MarkdownStoreService::SetFavorite(root, payload.value("filePath", ""), payload.value("favorite", false), &updated, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Failed to update Markdown Store favorite."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(SerializeMarkdownStoreEntry(updated).dump());
}

void UamQueryHandler::HandleBrowseMarkdownStoreImport(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const PlatformPathBrowseTarget target = payload.value("kind", "file") == "folder" ? PlatformPathBrowseTarget::Directory : PlatformPathBrowseTarget::File;
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(payload.value("currentValue", ""));
	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(target, initial_path, &selected_path, &error))
	{
		cb->Failure(500, FailureDetailOrFallback(error, "Failed to browse import source."));
		return;
	}
	cb->Success(nlohmann::json{{"selectedPath", selected_path}}.dump());
}

void UamQueryHandler::HandlePreviewMarkdownStoreImports(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	std::vector<MarkdownStoreService::ImportSource> sources;
	if (payload.value("includeProviders", false)) sources = MarkdownStoreService::DefaultImportSources();
	if (const auto found = payload.find("paths"); found != payload.end() && found->is_array())
	{
		for (const auto& value : *found)
		{
			if (value.is_string())
				sources.push_back({"manual", uam::paths::PathFromUtf8(value.get<std::string>())});
		}
	}
	RunAsyncCefQuery(cb, [root, sources = std::move(sources)]()
	{
		std::string error;
		const auto candidates = MarkdownStoreService::PreviewImports(root, sources, &error);
		if (!error.empty()) return AsyncFailure(400, error);
		nlohmann::json values = nlohmann::json::array();
		for (const auto& candidate : candidates) values.push_back(SerializeImportCandidate(candidate));
		return AsyncSuccess(nlohmann::json{{"candidates", values}});
	});
}

void UamQueryHandler::HandleImportMarkdownStoreEntries(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path root = MarkdownStoreService::NormalizeRoot(m_app.settings.markdown_store_directory);
	std::vector<MarkdownStoreService::ImportRequest> requests;
	if (const auto found = payload.find("imports"); found != payload.end() && found->is_array())
	{
		for (const auto& value : *found)
		{
			if (!value.is_object()) continue;
			requests.push_back({value.value("sourceProvider", "manual"), uam::paths::PathFromUtf8(value.value("sourcePath", "")), ParseConflictAction(value.value("conflictAction", "skip"))});
		}
	}
	std::string error;
	const auto results = MarkdownStoreService::ImportEntries(root, requests, &error);
	if (!error.empty())
	{
		cb->Failure(400, error);
		return;
	}
	nlohmann::json values = nlohmann::json::array();
	for (const auto& result : results)
	{
		nlohmann::json value{{"sourcePath", uam::paths::Utf8PathString(result.source_path)}, {"status", result.status}, {"message", result.message}};
		if (!result.entry.id.empty()) value["entry"] = SerializeMarkdownStoreEntry(result.entry);
		values.push_back(std::move(value));
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"results", values}}.dump());
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
