#include "uam_query_handler.h"
#include "app/startup_native_import_service.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"

void UamQueryHandler::HandleDiscoverSystemNativeImports(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	if (!m_app.startup_native_import_service)
	{
		cb->Failure(409, "Native history discovery is unavailable.");
		return;
	}
	std::string error;
	if (!m_app.startup_native_import_service->DiscoverLocalHistory(m_app, &error))
	{
		cb->Failure(409, error);
		return;
	}
	cb->Success(uam::StateSerializer::SerializeStartupNativeImports(m_app.startup_native_import_service->Snapshot()).dump());
	uam::PushStateUpdateIfChanged(browser, m_app);
}

void UamQueryHandler::HandleAssignSystemNativeImportWorkspace(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.is_object())
	{
		cb->Failure(400, "Invalid workspace assignment.");
		return;
	}
	for (const auto& [key, limit] : {std::pair{"generation", std::size_t{128}}, std::pair{"candidateId", std::size_t{128}}, std::pair{"title", std::size_t{256}}})
	{
		const auto value = payload.find(key);
		if (value == payload.end() || !value->is_string() || value->get_ref<const std::string&>().empty() || value->get_ref<const std::string&>().size() > limit)
		{
			cb->Failure(400, "Invalid workspace assignment.");
			return;
		}
	}
	const auto offset = payload.find("offset"), limit = payload.find("limit"), folder = payload.find("folderId");
	if ((offset != payload.end() && (!offset->is_number_integer() || *offset < 0 || *offset > 1000000)) ||
	    (limit != payload.end() && (!limit->is_number_integer() || *limit < 1 || *limit > 200)) ||
	    (folder != payload.end() && !folder->is_null() && (!folder->is_string() || folder->get_ref<const std::string&>().empty() || folder->get_ref<const std::string&>().size() > 128)))
	{
		cb->Failure(400, "Invalid import page.");
		return;
	}
	const std::size_t page_offset = payload.value("offset", std::size_t{0}), page_limit = payload.value("limit", std::size_t{100});
	const std::string folder_id = folder == payload.end() || folder->is_null() ? std::string{} : folder->get<std::string>();

	std::string error;
	if (!m_app.startup_native_import_service || !m_app.startup_native_import_service->AssignWorkspace(m_app,
	    payload.at("generation").get<std::string>(), payload.at("candidateId").get<std::string>(), payload.at("title").get<std::string>(), &error))
	{
		cb->Failure(409, error.empty() ? "Native workspace assignment is unavailable." : error);
		return;
	}
	cb->Success(uam::StateSerializer::SerializeStartupNativeImports(m_app.startup_native_import_service->Snapshot(page_offset, page_limit, folder_id)).dump());
	uam::PushStateUpdateIfChanged(browser, m_app);
}

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
	const nlohmann::json::const_iterator import_all = payload.find("importAll");
	if (import_all != payload.end() && !import_all->is_boolean())
	{
		cb->Failure(400, "Invalid import-all decision.");
		return;
	}
	if (generation.size() > 128 || !m_app.startup_native_import_service->Decide(m_app, generation, selected, declined, &error, folder_id,
	    import_all != payload.end() && import_all->get<bool>()))
	{
		cb->Failure(409, error.empty() ? "Import selection changed. Review it again." : error);
		return;
	}
	cb->Success(uam::StateSerializer::SerializeStartupNativeImports(m_app.startup_native_import_service->Snapshot(0, 100, folder_id)).dump());
	uam::PushStateUpdateIfChanged(browser, m_app);
}
