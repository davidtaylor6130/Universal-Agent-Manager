#include "cef/uam_query_handler.h"

#include "app/persistence_coordinator.h"
#include "app/theme_service.h"
#include "cef/cef_push.h"
#include "common/config/settings_normalization.h"
#include "common/utils/nlohmann_json_utils.h"

#include <nlohmann/json.hpp>

#include <utility>

void UamQueryHandler::HandleListThemes(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& /*payload*/, CefRefPtr<Callback> cb)
{
	cb->Success(nlohmann::json{{"themes", ThemeService::List(m_app.data_root)}}.dump());
}

void UamQueryHandler::HandleSaveTheme(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto theme_it = payload.find("theme");
	if (theme_it == payload.end())
	{
		cb->Failure(400, "Theme payload is required.");
		return;
	}

	nlohmann::json normalized;
	std::string error;
	if (!ThemeService::Save(m_app.data_root, *theme_it, &normalized, &error))
	{
		cb->Failure(400, error.empty() ? "Theme is invalid." : error);
		return;
	}
	cb->Success(nlohmann::json{{"theme", std::move(normalized)}}.dump());
}

void UamQueryHandler::HandleDeleteTheme(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string id = uam::nlohmann_json::TrimmedStringValue(payload, {"id"});
	std::string error;
	if (!ThemeService::Delete(m_app.data_root, id, &error))
	{
		cb->Failure(400, error.empty() ? "Failed to delete theme." : error);
		return;
	}

	if (m_app.settings.ui_theme == id)
	{
		m_app.settings.ui_theme = uam::settings::kDarkThemeId;
		if (!PersistenceCoordinator().SaveSettings(m_app))
		{
			cb->Failure(500, "Theme was deleted, but the fallback setting could not be saved.");
			return;
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
	}
	cb->Success("{}");
}
