#include "uam_query_handler.h"
#include "app/startup_native_import_service.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"

void UamQueryHandler::HandleGetStartupNativeImports(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const int64_t offset = payload.value("offset", int64_t{0});
	const int64_t limit = payload.value("limit", int64_t{100});
	std::string folder_id;
	const nlohmann::json::const_iterator folder_value = payload.find("folderId");
	if (folder_value != payload.end() && !folder_value->is_null())
	{
		if (!folder_value->is_string() || folder_value->get_ref<const std::string&>().empty() || folder_value->get_ref<const std::string&>().size() > 128)
		{
			cb->Failure(400, "Invalid import workspace.");
			return;
		}
		folder_id = folder_value->get<std::string>();
	}
	if (offset < 0 || limit < 1 || limit > 200)
	{
		cb->Failure(400, "Invalid import page.");
		return;
	}
	const uam::StartupNativeImportSnapshot snapshot = m_app.startup_native_import_service
	    ? m_app.startup_native_import_service->Snapshot(static_cast<std::size_t>(offset), static_cast<std::size_t>(limit), folder_id)
	    : uam::StartupNativeImportSnapshot{};
	cb->Success(uam::StateSerializer::SerializeStartupNativeImports(snapshot).dump());
}

void UamQueryHandler::HandleResolveStartupNativeImports(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!m_app.startup_native_import_service)
	{
		cb->Failure(409, "Startup imports are unavailable.");
		return;
	}
	std::string folder_id;
	const nlohmann::json::const_iterator folder_value = payload.find("folderId");
	if (folder_value != payload.end() && !folder_value->is_null())
	{
		if (!folder_value->is_string() || folder_value->get_ref<const std::string&>().empty() || folder_value->get_ref<const std::string&>().size() > 128)
		{
			cb->Failure(400, "Invalid import workspace.");
			return;
		}
		folder_id = folder_value->get<std::string>();
	}
	const std::string generation = payload.value("generation", std::string{});
	std::vector<std::string> selected;
	std::vector<std::string> declined;
	for (const std::pair<const char*, std::vector<std::string>*>& field : {
	         std::pair{"selectedIds", &selected}, std::pair{"declinedIds", &declined}})
	{
		const nlohmann::json::const_iterator value = payload.find(field.first);
		if (value == payload.end()) continue;
		if (!value->is_array() || value->size() > 200)
		{
			cb->Failure(400, "Invalid import selection.");
			return;
		}
		for (const nlohmann::json& id : *value)
		{
			if (!id.is_string() || id.get_ref<const std::string&>().size() > 128)
			{
				cb->Failure(400, "Invalid import selection.");
				return;
			}
			field.second->push_back(id.get<std::string>());
		}
	}
	std::string error;
	if (generation.size() > 128 || !m_app.startup_native_import_service->Decide(m_app, generation, selected, declined, &error, folder_id))
	{
		cb->Failure(409, error.empty() ? "Import selection changed. Review it again." : error);
		return;
	}
	cb->Success(uam::StateSerializer::SerializeStartupNativeImports(m_app.startup_native_import_service->Snapshot(0, 100, folder_id)).dump());
	uam::PushStateUpdateIfChanged(browser, m_app);
}
