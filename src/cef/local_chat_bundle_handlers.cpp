#include "cef/uam_query_handler.h"
#include "common/provider/excluded_provider_data.h"
#include "cef/uam_query_handler_async.h"

#include "app/chat_domain_service.h"
#include "app/local_chat_bundle_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

using namespace uam::query_handler_async;

namespace
{
	std::string BundleStatus(bool ok, bool degraded)
	{
		if (ok) return "complete";
		return degraded ? "degraded" : "failed";
	}

	nlohmann::json BaseBundleResult(const std::filesystem::path& folder, bool ok, bool degraded)
	{
		return {
		    {"cancelled", false},
		    {"status", BundleStatus(ok, degraded)},
		    {"folder", uam::paths::Utf8PathString(folder)},
		    {"schema", std::string(uam::LocalChatBundleService::kSchema)},
		    {"version", uam::LocalChatBundleService::kVersion},
		};
	}

	nlohmann::json SerializeExportResult(const uam::LocalChatBundleExportResult& result)
	{
		nlohmann::json body = BaseBundleResult(result.bundle_path, result.ok, result.degraded);
		body["totalCount"] = result.total_count;
		body["exportedCount"] = result.exported_count;
		body["warnings"] = result.warnings;
		body["errors"] = result.errors;
		return body;
	}

	nlohmann::json SerializeImportResult(const uam::LocalChatBundleImportResult& result)
	{
		nlohmann::json items = nlohmann::json::array();
		for (const uam::LocalChatBundleImportItem& item : result.items)
		{
			items.push_back({
			    {"sourceId", item.source_id},
			    {"importedId", item.imported_id},
			    {"importedTitle", item.imported_title},
			    {"renamed", item.renamed},
			    {"imported", item.imported},
			    {"error", item.error},
			});
		}

		nlohmann::json body = BaseBundleResult(result.bundle_path, result.ok, result.degraded);
		body["totalCount"] = result.total_count;
		body["importedCount"] = result.imported_count;
		body["failedCount"] = result.failed_count;
		body["renamedCount"] = result.renamed_count;
		body["items"] = std::move(items);
		body["warnings"] = result.warnings;
		body["errors"] = result.errors;
		return body;
	}

	nlohmann::json CancelledBundleResult(bool import_operation)
	{
		nlohmann::json body = {
		    {"cancelled", true},
		    {"status", "cancelled"},
		    {"folder", ""},
		    {"schema", std::string(uam::LocalChatBundleService::kSchema)},
		    {"version", uam::LocalChatBundleService::kVersion},
		    {"totalCount", 0},
		    {"warnings", nlohmann::json::array()},
		    {"errors", nlohmann::json::array()},
		};
		if (import_operation)
		{
			body["importedCount"] = 0;
			body["failedCount"] = 0;
			body["renamedCount"] = 0;
			body["items"] = nlohmann::json::array();
		}
		else
		{
			body["exportedCount"] = 0;
		}
		return body;
	}

	bool BrowseBundleFolder(
	    const nlohmann::json& payload,
	    std::filesystem::path& selected_out,
	    std::string& error_out)
	{
		const std::filesystem::path initial_path =
		    PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(
		        payload.value("currentValue", ""));
		std::string selected;
		if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(
		        PlatformPathBrowseTarget::Directory, initial_path, &selected, &error_out))
		{
			return false;
		}
		selected_out = uam::paths::PathFromUtf8(selected);
		return true;
	}
} // namespace

void UamQueryHandler::HandleExportLocalChats(
    CefRefPtr<CefBrowser> /*browser*/,
    const nlohmann::json& payload,
    CefRefPtr<Callback> cb)
{
	std::filesystem::path bundle_path;
	std::string browse_error;
	if (!BrowseBundleFolder(payload, bundle_path, browse_error))
	{
		if (!browse_error.empty()) cb->Failure(500, browse_error);
		else cb->Success(CancelledBundleResult(false).dump());
		return;
	}

	const std::filesystem::path data_root = m_app.data_root;
	RunAsyncCefQuery(m_asyncLifetime, cb, [data_root, bundle_path]() {
		return AsyncSuccess(SerializeExportResult(
		    uam::LocalChatBundleService::Export(data_root, bundle_path)));
	});
}

void UamQueryHandler::HandleImportLocalChats(
    CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload,
    CefRefPtr<Callback> cb)
{
	std::filesystem::path bundle_path;
	std::string browse_error;
	if (!BrowseBundleFolder(payload, bundle_path, browse_error))
	{
		if (!browse_error.empty()) cb->Failure(500, browse_error);
		else cb->Success(CancelledBundleResult(true).dump());
		return;
	}

	const std::filesystem::path data_root = m_app.data_root;
	auto import_result = std::make_shared<uam::LocalChatBundleImportResult>();
	auto imported_summaries = std::make_shared<std::vector<ChatSession>>();
	uam::AppState* live_app = &m_app;
	RunAsyncCefQuery(m_asyncLifetime,
	    cb,
	    [data_root, bundle_path, import_result, imported_summaries]() {
		    *import_result = uam::LocalChatBundleService::Import(data_root, bundle_path);
		    imported_summaries->reserve(import_result->imported_count);
		    for (const uam::LocalChatBundleImportItem& item : import_result->items)
		    {
			    if (!item.imported) continue;
			    std::string warning;
			    std::optional<ChatSession> summary =
			        ChatRepository::LoadLocalChat(data_root, item.imported_id, false, &warning);
			    if (!summary)
			    {
				    import_result->ok = false;
				    import_result->degraded = true;
				    import_result->warnings.push_back(
				        "Imported chat is durable but could not be added to the live sidebar: " + item.imported_id +
				        (warning.empty() ? std::string{} : " (" + warning + ")"));
				    continue;
			    }
			    imported_summaries->push_back(std::move(*summary));
		    }
		    // An excluded provider's chats are saved for a fuller build but do not exist here, so they are not reported.
		    std::vector<ChatSession> visible = *imported_summaries;
		    std::unordered_set<std::string> hidden;
		    for (const ChatSession& chat : uam::excluded_provider_data::TakeChats(visible)) hidden.insert(chat.id);
		    if (!hidden.empty())
		    {
			    std::erase_if(import_result->items, [&](const uam::LocalChatBundleImportItem& item)
			    {
				    if (!item.imported || !hidden.contains(item.imported_id)) return false;
				    if (item.renamed) --import_result->renamed_count;
				    return true;
			    });
			    import_result->imported_count -= hidden.size();
			    import_result->total_count -= hidden.size();
		    }
		    return AsyncSuccess(SerializeImportResult(*import_result));
	    },
	    [live_app, browser, imported_summaries](AsyncCefResult& result) {
		    if (!result.ok || imported_summaries->empty()) return;
		    bool changed = false;
		    for (ChatSession& hidden : uam::excluded_provider_data::TakeChats(*imported_summaries))
			    live_app->excluded_provider_chats.push_back(std::move(hidden));
		    for (ChatSession& summary : *imported_summaries)
		    {
			    if (ChatDomainService().FindChatById(*live_app, summary.id) != nullptr) continue;
			    live_app->chats.push_back(std::move(summary));
			    changed = true;
		    }
		    if (changed) uam::PushStateUpdateIfChanged(browser, *live_app);
	    });
}
