#include "cef/uam_query_handler.h"

#include "app/resource_collection_service.h"
#include "app/persistence_coordinator.h"
#include "common/chat/chat_folder_store.h"
#include "common/config/custom_icon.h"
#include "common/platform/platform_services.h"
#include "include/cef_image.h"
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

/// <summary>Sets portable icon metadata on an existing sidebar domain with persistence rollback.</summary>
void UamQueryHandler::HandleSetCustomIcon(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string type = payload.value("targetType", "");
	const std::string id = payload.value("targetId", "");
	CustomIcon* target = nullptr;
	if (type == "workspace")
	{
		for (ChatFolder& folder : m_app.folders) if (folder.id == id) target = &folder.custom_icon;
	}
	else if (type == "host")
	{
		for (ExecutionHost& host : m_app.settings.execution_hosts) if (host.id == id) target = &host.custom_icon;
	}
	else if (type == "collection")
	{
		for (ResourceCollection& collection : m_app.resource_collections) if (collection.id == id) target = &collection.custom_icon;
	}
	else if (type == "desktop-app")
	{
		for (ResourceCollection& collection : m_app.resource_collections)
			for (ResourceReference& reference : collection.references)
				if (reference.id == id && reference.type == "desktop-app") target = &reference.custom_icon;
	}
	if (target == nullptr)
	{
		cb->Failure(404, "Icon target not found.");
		return;
	}
	CustomIcon icon;
	std::filesystem::path imported_file;
	if (payload.contains("icon") && !payload["icon"].is_null())
	{
		const nlohmann::json& value = payload["icon"];
		if (!value.is_object() || !value.contains("type") || !value["type"].is_string())
		{
			cb->Failure(400, "Choose a character, emoji, or PNG icon.");
			return;
		}
		if (value["type"] == "png")
		{
			std::string bytes;
			if (!value.contains("base64") || !value["base64"].is_string() ||
			    value["base64"].get_ref<const std::string&>().size() > 350000 ||
			    !uam::base64::Decode(value["base64"].get_ref<const std::string&>(), bytes) ||
			    !uam::icons::HasBoundedPngHeader(bytes))
			{
				cb->Failure(400, "Use a PNG no larger than 256 by 256 pixels and 256 KB.");
				return;
			}
			CefRefPtr<CefImage> image = CefImage::CreateImage();
			if (!image || !image->AddPNG(1.0f, bytes.data(), bytes.size()) ||
			    image->GetWidth() > 256 || image->GetHeight() > 256)
			{
				cb->Failure(400, "The PNG could not be read.");
				return;
			}
			icon = {"png", PlatformServicesFactory::Instance().process_service.GenerateUuid() + ".png"};
			const std::filesystem::path directory = m_app.data_root / "icons";
			std::error_code error;
			if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error)))
			{
				cb->Failure(500, "The icon folder cannot be used.");
				return;
			}
			error.clear();
			std::filesystem::create_directories(directory, error);
			imported_file = directory / icon.value;
			if (error || !uam::icons::IsValid(icon) || !uam::io::WriteBinaryFile(imported_file, bytes))
			{
				cb->Failure(500, "Could not save the icon.");
				return;
			}
		}
		else
		{
			icon = uam::icons::Parse(value);
			if (icon.type != "text")
			{
				cb->Failure(400, "Choose a character or emoji icon.");
				return;
			}
		}
	}
	const CustomIcon previous = *target;
	*target = icon;
	const bool saved = type == "workspace" ? ChatFolderStore::Save(m_app.data_root, m_app.folders)
	    : type == "host" ? PersistenceCoordinator().SaveSettings(m_app)
	    : uam::ResourceCollectionService::Save(m_app.data_root, m_app.resource_collections);
	if (!saved)
	{
		*target = previous;
		if (!imported_file.empty())
		{
			std::error_code error;
			std::filesystem::remove(imported_file, error);
		}
		cb->Failure(500, "Could not save the icon setting.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
