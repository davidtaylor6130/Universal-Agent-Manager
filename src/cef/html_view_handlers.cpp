#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "common/chat/html_view.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"

#include <nlohmann/json.hpp>
#include <string>

using uam::query_handler_internal::FailureDetailOrFallback;

namespace
{
	bool ReadHtmlViewPayload(const nlohmann::json& payload, std::string& title, std::string& html, std::string& error)
	{
		if (!payload.is_object() || !payload.contains("title") || !payload["title"].is_string() ||
		    !payload.contains("html") || !payload["html"].is_string())
		{
			error = "HTML view payload needs a title and html.";
			return false;
		}
		title = payload["title"].get<std::string>();
		html = payload["html"].get<std::string>();
		error = uam::html_view::ValidationError(title, html);
		return error.empty();
	}
} // namespace

void UamQueryHandler::HandleOpenHtmlView(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string title, html, error;
	if (!ReadHtmlViewPayload(payload, title, html, error))
	{
		cb->Failure(400, error);
		return;
	}
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [html = std::move(html)]() {
		const std::optional<std::filesystem::path> temp = uam::paths::TempDirectoryPathNoThrow();
		if (!temp) return uam::query_handler_async::AsyncFailure(500, "No temporary folder is available for the HTML view.");
		// Content-addressed, so reopening the same view reuses one file instead of accumulating copies.
		const std::filesystem::path path = *temp / "uam-html-views" / ("view-" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(html)) + ".html");
		std::string open_error;
		if (!uam::html_view::PrepareOpenedCopyFolder(path.parent_path()) || uam::paths::IsLinkOrReparsePointNoThrow(path) ||
		    !uam::io::WriteTextFile(path, uam::html_view::DocumentWithPolicy(html)))
			return uam::query_handler_async::AsyncFailure(500, "The HTML view could not be written to a temporary file.");
		if (!PlatformServicesFactory::Instance().file_dialog_service.OpenFileWithDefaultApplication(path, &open_error))
			return uam::query_handler_async::AsyncFailure(500, FailureDetailOrFallback(open_error, "The HTML view could not be opened in the system browser."));
		return uam::query_handler_async::AsyncSuccess({{"opened", true}});
	});
}

void UamQueryHandler::HandleSaveHtmlView(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string title, html, error;
	if (!ReadHtmlViewPayload(payload, title, html, error))
	{
		cb->Failure(400, error);
		return;
	}
	std::string selected;
	// ponytail: folder picker plus a unique file name; a native Save As dialog needs new platform code.
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::Directory, {}, &selected, &error))
	{
		if (!error.empty()) cb->Failure(500, error);
		else cb->Success(nlohmann::json{{"saved", false}}.dump());
		return;
	}
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [html = std::move(html), title = std::move(title), selected = std::move(selected)]() {
		const std::filesystem::path folder = uam::paths::PathFromUtf8(selected);
		const std::string name = uam::html_view::FileName(title);
		const std::string stem = name.substr(0, name.size() - 5);
		std::filesystem::path path = folder / uam::paths::PathFromUtf8(name);
		for (int index = 2; uam::paths::PathExistsNoThrow(path) && index < 1000; ++index)
			path = folder / uam::paths::PathFromUtf8(stem + " " + std::to_string(index) + ".html");
		if (uam::paths::PathExistsNoThrow(path) || !uam::io::WriteTextFile(path, html))
			return uam::query_handler_async::AsyncFailure(500, "The HTML view could not be saved in that folder.");
		return uam::query_handler_async::AsyncSuccess({{"saved", true}, {"path", uam::paths::Utf8PathString(path)}});
	});
}
