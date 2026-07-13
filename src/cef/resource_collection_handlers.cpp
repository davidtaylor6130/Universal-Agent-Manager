#include "cef/uam_query_handler.h"

#include "app/resource_collection_service.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace
{
	int ResourceCollectionFailureCode(const std::string& error)
	{
		if (error.find("not found") != std::string::npos)
		{
			return 404;
		}
		if (error.find("persist") != std::string::npos)
		{
			return 500;
		}
		return 400;
	}

	bool ParseStringArray(const nlohmann::json& payload, const char* field, std::vector<std::string>* values)
	{
		const auto found = payload.find(field);
		if (found == payload.end() || !found->is_array())
		{
			return false;
		}
		values->clear();
		values->reserve(found->size());
		for (const nlohmann::json& value : *found)
		{
			if (!value.is_string())
			{
				return false;
			}
			values->push_back(value.get<std::string>());
		}
		return true;
	}

	void FailResourceCollection(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, const std::string& error)
	{
		callback->Failure(ResourceCollectionFailureCode(error), error);
	}
} // namespace

void UamQueryHandler::HandleCreateResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ResourceCollection created;
	std::string error;
	if (!uam::ResourceCollectionService::Create(m_app, payload.value("name", ""), &created, &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(uam::StateSerializer::SerializeResourceCollection(created).dump());
}

void UamQueryHandler::HandleRenameResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	if (!uam::ResourceCollectionService::Rename(m_app, payload.value("collectionId", ""), payload.value("name", ""), &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleDeleteResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	if (!uam::ResourceCollectionService::Delete(m_app, payload.value("collectionId", ""), &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleToggleResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	if (!uam::ResourceCollectionService::ToggleCollapsed(m_app, payload.value("collectionId", ""), &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleReorderResourceCollections(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::vector<std::string> collection_ids;
	if (!ParseStringArray(payload, "collectionIds", &collection_ids))
	{
		cb->Failure(400, "collectionIds must be an array of strings.");
		return;
	}
	std::string error;
	if (!uam::ResourceCollectionService::ReorderCollections(m_app, collection_ids, &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleAddResourceReference(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ResourceReference created;
	std::string error;
	if (!uam::ResourceCollectionService::AddReference(m_app,
	                                                payload.value("collectionId", ""),
	                                                payload.value("type", ""),
	                                                payload.value("target", ""),
	                                                payload.value("label", ""),
	                                                &created,
	                                                &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(uam::StateSerializer::SerializeResourceReference(created).dump());
}

void UamQueryHandler::HandleRemoveResourceReference(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	if (!uam::ResourceCollectionService::RemoveReference(m_app, payload.value("collectionId", ""), payload.value("referenceId", ""), &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleReorderResourceReferences(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::vector<std::string> reference_ids;
	if (!ParseStringArray(payload, "referenceIds", &reference_ids))
	{
		cb->Failure(400, "referenceIds must be an array of strings.");
		return;
	}
	std::string error;
	if (!uam::ResourceCollectionService::ReorderReferences(m_app, payload.value("collectionId", ""), reference_ids, &error))
	{
		FailResourceCollection(cb, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
